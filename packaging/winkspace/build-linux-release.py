#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a native Linux beta, a relocatable archive, and corresponding source.

Run on the target distribution from a clean, committed checkout with initialized
submodules and an already configured Release build. No root access is required.
"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import tarfile
import tempfile


REPO = Path(__file__).resolve().parents[2]
PROJECT_URL = "https://github.com/Namandeepsd/winksopac"


def run(args, *, cwd=REPO, log=None):
    if log:
        with log.open("w") as stream:
            result = subprocess.run(args, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f"Command failed; see {log}\n{log.read_text()[-6000:]}")
        return ""
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def write(path, text, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    if executable:
        path.chmod(0o755)


def archive(destination, entries, epoch, extra=None):
    """Stream a normalized tar to threaded xz without following symlinks."""
    with destination.open("wb") as output:
        process = subprocess.Popen(["xz", "-T2", "-4", "--stdout"], stdin=subprocess.PIPE, stdout=output)
        try:
            with tarfile.open(fileobj=process.stdin, mode="w|", format=tarfile.PAX_FORMAT) as tar:
                for path, name in entries:
                    info = tar.gettarinfo(str(path), arcname=name)
                    info.uid = info.gid = 0
                    info.uname = info.gname = "root"
                    info.mtime = epoch
                    if info.isfile():
                        with path.open("rb") as stream:
                            tar.addfile(info, stream)
                    else:
                        tar.addfile(info)
                if extra:
                    for name, data in extra.items():
                        info = tarfile.TarInfo(name)
                        info.size = len(data)
                        info.mode = 0o644
                        info.mtime = epoch
                        tar.addfile(info, io.BytesIO(data))
        finally:
            process.stdin.close()
        if process.wait():
            raise RuntimeError(f"Compression failed: {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=REPO / "build")
    parser.add_argument("--output-dir", type=Path, default=REPO / "build/releases")
    parser.add_argument("--version", default="0.1.0-beta.1")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-beta\.\d+)?", args.version):
        parser.error("Version must look like 0.1.0 or 0.1.0-beta.1")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    for command in ("cmake", "dpkg-deb", "dpkg-shlibdeps", "readelf", "xz", "git"):
        if not shutil.which(command):
            raise RuntimeError(f"Required tool is missing: {command}")
    if run(["git", "status", "--porcelain"]):
        raise RuntimeError("Commit the source changes before packaging a release")
    submodules = run(["git", "submodule", "status", "--recursive"])
    if any(line[0] != " " for line in subprocess.check_output(
        ["git", "submodule", "status", "--recursive"], cwd=REPO, text=True,
    ).splitlines()):
        raise RuntimeError("Initialize submodules and check out their recorded commits")
    build = args.build_dir.resolve()
    cache = (build / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        raise RuntimeError("Configure the build with -DCMAKE_BUILD_TYPE=Release")
    if "WINKSPACE_BUNDLE_LEARNING_RESOURCES:BOOL=OFF" not in cache:
        raise RuntimeError("Configure with -DWINKSPACE_BUNDLE_LEARNING_RESOURCES=OFF")
    if "WINKSPACE_INSTALL_UPSTREAM_DESKTOP_ENTRIES:BOOL=OFF" not in cache:
        raise RuntimeError("Configure with -DWINKSPACE_INSTALL_UPSTREAM_DESKTOP_ENTRIES=OFF")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    commit = run(["git", "rev-parse", "HEAD"])
    epoch = int(run(["git", "show", "-s", "--format=%ct", "HEAD"]))
    os.environ["SOURCE_DATE_EPOCH"] = str(epoch)
    architecture = run(["dpkg", "--print-architecture"])
    maintainer = run(["git", "show", "-s", "--format=%an <%ae>", "HEAD"])
    os_info = platform.freedesktop_os_release()
    metadata = {
        "project": "WinkSpace", "version": args.version, "source_commit": commit,
        "source_url": f"{PROJECT_URL}/tree/{commit}",
        "platform": os_info.get("PRETTY_NAME", platform.system()),
        "architecture": architecture, "build_type": "Release",
        "compiler": run(["c++", "--version"]).splitlines()[0],
        "gtk": run(["pkg-config", "--modversion", "gtk4"]),
        "submodules": [line.strip() for line in submodules.splitlines()],
    }
    print("Building WinkSpace...", flush=True)
    run(["cmake", "--build", str(build), "-j", str(args.jobs)], log=output / "build.log")
    with tempfile.TemporaryDirectory(prefix="stage-", dir=output) as temp:
        stage = Path(temp)
        root = stage / "deb"
        prefix = root / "opt/winkspace"
        print("Installing into a fresh staging directory...", flush=True)
        run(["cmake", "--install", str(build), "--prefix", str(prefix), "--strip"], log=output / "install.log")
        docs = prefix / "share/inkscape/doc"
        tracked = [Path(p.decode()) for p in subprocess.check_output(
            ["git", "ls-files", "--recurse-submodules", "-z"], cwd=REPO,
        ).split(b"\0") if p]
        # Retain component notices with their source paths, including static libs.
        for path in tracked:
            if str(path).startswith(("src/3rdparty/", "share/")) and re.match(
                r"^(?:LICEN[CS]E|COPYING|NOTICE|COPYRIGHT|OFL)(?:[._-]|$)", path.name, re.I,
            ) and (REPO / path).is_file():
                target = docs / "component-notices" / path
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(REPO / path, target)
        guide = (REPO / "packaging/winkspace/README.md").read_text()
        guide = guide.replace("(../../", f"({PROJECT_URL}/blob/{commit}/")
        (docs / "README.md").write_text(guide)
        info = json.dumps(metadata, indent=2) + "\n"
        write(prefix / "BUILD-INFO.json", info)
        write(docs / "SOURCE.txt", f"Corresponding source: {PROJECT_URL}/releases/tag/v{args.version}\n"
              f"Download WinkSpace-{args.version}-source.tar.xz, including all submodules.\nCommit: {commit}\n")

        # Install the package privately; leave system Inkscape binaries untouched.
        write(root / "usr/bin/winkspace", '#!/bin/sh\nexec /opt/winkspace/bin/winkspace "$@"\n', True)
        desktop = root / "usr/share/applications/org.inkscape.Inkscape.WinkSpace.desktop"
        desktop.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / "packaging/winkspace/winkspace.desktop", desktop)
        icon = root / "usr/share/icons/hicolor/scalable/apps/winkspace.svg"
        icon.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / "share/icons/application/scalable/winkspace.svg", icon)

        # Let dpkg compute versioned dependencies; private libraries are self-owned.
        debian = stage / "metadata/debian"
        write(debian / "control", f"Source: winkspace\nSection: graphics\nPriority: optional\nMaintainer: {maintainer}\n\n"
              "Package: winkspace\nArchitecture: any\nDescription: AI-assisted vector editor built on Inkscape\n")
        libraries = sorted(p for p in (prefix / "lib").rglob("*.so*") if p.is_file() and not p.is_symlink())
        shlibs = []
        for library in libraries:
            dynamic = run(["readelf", "-d", str(library)])
            soname = re.search(r"\(SONAME\).*\[([^\]]+)\]", dynamic)
            if soname and (match := re.fullmatch(r"(.+)\.so\.(.+)", soname[1])):
                shlibs.append(f"{match[1]} {match[2]} winkspace")
        write(debian / "shlibs.local", "\n".join(shlibs) + "\n")
        binaries = [prefix / "bin/inkscape", prefix / "bin/inkview", *libraries]
        command = ["dpkg-shlibdeps", "-O", "-xwinkspace"]
        command.extend(f"-l{p}" for p in sorted({p.parent for p in libraries}))
        command.extend(f"-e{p}" for p in binaries)
        dependency_output = run(command, cwd=debian.parent)
        native = next(line.removeprefix("shlibs:Depends=") for line in dependency_output.splitlines() if line.startswith("shlibs:Depends="))
        python = "ca-certificates, python3, python3-lxml, python3-numpy, python3-cssselect, python3-scour, python3-packaging, python3-webencodings, python3-tinycss2, python3-bs4, python3-requests"
        depends = native + ", " + python
        write(prefix / "DEPENDENCIES.txt", f"Built on {metadata['platform']} ({architecture}).\nSystem package dependencies:\n{depends}\n")
        packages = sorted({re.split(r"[ (]", item.strip())[0] for item in depends.split(",")})
        write(prefix / "install-runtime.sh", "#!/bin/sh\nset -eu\n"
              f"# Runtime packages for {metadata['platform']}. The .deb installs these automatically.\n"
              "exec sudo apt install -- " + " ".join(shlex.quote(p) for p in packages) + "\n", True)
        size = sum(p.stat().st_size for p in root.rglob("*") if p.is_file()) // 1024
        version = args.version.replace("-beta.", "~beta.")
        write(root / "DEBIAN/control", f"Package: winkspace\nVersion: {version}\nArchitecture: {architecture}\n"
              f"Maintainer: {maintainer}\nSection: graphics\nPriority: optional\nInstalled-Size: {size}\n"
              f"Homepage: {PROJECT_URL}\nDepends: {depends}\nRecommends: fonts-dejavu-core, python3-pil, python3-serial\n"
              "Description: AI-assisted SVG creation and editing built on Inkscape\n"
              " Generate and refine editable vector artwork with an integrated AI\n"
              " assistant. A separately configured model connection is required for AI.\n")
        package = output / f"winkspace_{args.version}_{architecture}.deb"
        print("Creating the Debian package...", flush=True)
        run(["dpkg-deb", "--root-owner-group", "-Zxz", "-z4", "--build", str(root), str(package)])

        name = f"WinkSpace-{args.version}"
        bundle = stage / name
        shutil.copytree(prefix, bundle, symlinks=True)
        write(bundle / "winkspace", '#!/bin/sh\nset -eu\napp_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexec "$app_dir/bin/winkspace" "$@"\n', True)
        write(bundle / "README.txt", f"WinkSpace {args.version}\nBuilt on {metadata['platform']} ({architecture}).\n\n"
              "Run ./winkspace from this directory. System libraries are required.\n"
              "On the matching Ubuntu release, ./install-runtime.sh installs runtime dependencies.\n"
              "The .deb is recommended for automatic dependency installation and desktop integration.\n"
              "Configure a model in AI Assistant > Connection settings for AI features.\n"
              "Inkscape preferences are currently shared unless INKSCAPE_PROFILE_DIR is set.\n\n"
              f"Guide and matching source: {PROJECT_URL}/releases/tag/v{args.version}\n"
              "Credits and licenses: share/inkscape/doc/\n")
        print("Compressing the Linux archive...", flush=True)
        machine = "x86_64" if architecture == "amd64" else architecture
        binary = output / f"{name}-linux-{machine}.tar.xz"
        entries = [(p, str(Path(name) / p.relative_to(bundle))) for p in sorted(bundle.rglob("*"))]
        archive(binary, entries, epoch)

        print("Archiving corresponding source, including every submodule...", flush=True)
        source = output / f"{name}-source.tar.xz"
        entries = [(REPO / p, str(Path(name + "-source") / p)) for p in tracked]
        archive(source, entries, epoch, {f"{name}-source/RELEASE-SOURCE.json": info.encode()})
    write(output / "BUILD-INFO.json", info)
    artifacts = [package, binary, source, output / "BUILD-INFO.json"]
    checksums = ""
    for artifact in artifacts:
        with artifact.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        checksums += f"{digest}  {artifact.name}\n"
    write(output / "SHA256SUMS", checksums)
    print(f"Release artifacts saved to {output}", flush=True)
    for artifact in artifacts:
        print(f"  {artifact.name}: {artifact.stat().st_size / 1024**2:.1f} MiB", flush=True)


if __name__ == "__main__":
    main()
