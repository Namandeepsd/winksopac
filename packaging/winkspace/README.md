# WinkSpace on Linux

[Project and demo](../../README.md) ·
[Downloads](https://github.com/Namandeepsd/winksopac/releases/tag/v0.1.0-beta.1) ·
[Model setup](../../doc/ai-connections.md)

## Supported release build

The 0.1.0 Beta 1 binaries are built on **Ubuntu 25.10, amd64/x86_64**, using
GTK4. They require compatible system libraries. Other distributions and older
Ubuntu releases have not been tested; this is not a universal AppImage.

AI models, CLI providers, their sign-ins, and API keys are not included. The
editor can run without a model, but AI generation requires a configured
[connection](../../doc/ai-connections.md). A terminal connector can still use an
online provider.

### Install the Debian package

Download `winkspace_0.1.0-beta.1_amd64.deb` from the release, then run the following
in the directory containing it:

```sh
sudo apt install ./winkspace_0.1.0-beta.1_amd64.deb
winkspace
```

APT resolves the package's declared dependencies. The files live in
`/opt/winkspace`, with a `winkspace` launcher and a desktop entry. The package
does not replace the system `inkscape` executable. Remove it with:

```sh
sudo apt remove winkspace
```

### Use the archive

Download `WinkSpace-0.1.0-beta.1-linux-x86_64.tar.xz`, then run:

```sh
tar -xf WinkSpace-0.1.0-beta.1-linux-x86_64.tar.xz
cd WinkSpace-0.1.0-beta.1
./install-runtime.sh
./winkspace
```

`install-runtime.sh` uses APT to install the Ubuntu runtime packages listed in
`DEPENDENCIES.txt`; it does not install the editor system-wide. If the matching
dependencies are already present, launch `./winkspace` directly. Keep the whole
extracted folder together. You can move it to another location; the installed
libraries use relative runtime paths.

### Check downloads

Save `SHA256SUMS` beside the downloaded release assets. To verify one file:

```sh
sha256sum winkspace_0.1.0-beta.1_amd64.deb
```

Compare that value to the matching line in `SHA256SUMS`. If you downloaded every
file listed in the checksum file, run `sha256sum -c SHA256SUMS` instead.

### Preferences and workspace

The launcher uses a separate application instance tag. **Inkscape-format
preferences are currently shared.** To use an independent profile, set its path:

```sh
INKSCAPE_PROFILE_DIR="$HOME/.config/winkspace" winkspace
```

The default workspace contains essential tools and a **More tools** toggle.
**Advanced** contains filters, extensions, XML editing, and tiled clones. New
profiles open Layers, Fill and Stroke, Align, Export, and AI Assistant. Saved
panel layouts remain respected.

## Build from source

Use a C++20 compiler, CMake, Ninja, GTK >= 4.14, gtkmm >= 4.13.3,
glibmm >= 2.78.1, libsoup 3, and nlohmann JSON. The project also requires the
upstream vector editor's graphics, text, and XML dependencies.

For the Ubuntu build environment, install the development packages:

```sh
sudo apt install build-essential cmake ninja-build pkg-config gettext \
  libboost-dev libboost-filesystem-dev libboost-stacktrace-dev \
  libgtk-4-dev libgtkmm-4.0-dev libglibmm-2.68-dev libsigc++-3.0-dev \
  libgtksourceview-5-dev libspelling-1-dev libsoup-3.0-dev nlohmann-json3-dev \
  libgc-dev libgsl-dev libdouble-conversion-dev libepoxy-dev \
  liblcms2-dev libfontconfig-dev libpango1.0-dev libcairo2-dev \
  libpng-dev libjpeg-dev libpotrace-dev libxml2-dev libxslt1-dev \
  libpoppler-glib-dev libpoppler-private-dev libreadline-dev \
  libgtest-dev libgmock-dev python3-lxml
```

Initialize the exact submodule revisions and build:

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install_dir" \
  -DWINKSPACE_BUNDLE_LEARNING_RESOURCES=OFF \
  -DWINKSPACE_INSTALL_UPSTREAM_DESKTOP_ENTRIES=OFF
cmake --build build -j2
cmake --install build
./build/install_dir/bin/winkspace
```

For a complete release source archive, extract it and run the CMake commands;
the submodule contents are already included, so the Git initialization step is
unnecessary. General compilation background is in the
[upstream Linux guide](../../doc/building/linux.md).

Offline upstream tutorials and examples are omitted from a fresh install by
default. Set `-DWINKSPACE_BUNDLE_LEARNING_RESOURCES=ON` to include them. Changing
the option does not remove resources from an existing installation; stage into
a fresh directory when producing a release. Extensions remain included because
they also implement import/export formats.

## Create a release

Start from a clean, committed checkout with all submodules initialized and a
configured Release build. Install `dpkg-dev` and `xz-utils` if needed. Run:

```sh
python3 packaging/winkspace/build-linux-release.py --version 0.1.0-beta.1
```

The script builds the editor, installs into a fresh staging directory, calculates
versioned runtime dependencies, and creates these files in `build/releases/`:

| Artifact | Contents |
| --- | --- |
| `winkspace_0.1.0-beta.1_amd64.deb` | Native package, private libraries, desktop launcher, resources, and notices |
| `WinkSpace-0.1.0-beta.1-linux-x86_64.tar.xz` | Relocatable installed tree, launcher, and runtime dependency installer |
| `WinkSpace-0.1.0-beta.1-source.tar.xz` | Tracked source and every nested submodule, without Git metadata or local builds |
| `BUILD-INFO.json` | Source commit, submodule revisions, compiler, toolkit, and platform |
| `SHA256SUMS` | SHA256 values for the packaged artifacts and build information |

The full recording is published separately as `winkspace-demo.mp4`; screenshots
and their extraction script are tracked in the source. Release build/install
logs remain in `build/releases/` for local diagnosis. Source archives normalize
timestamps and ownership; this does not claim a bit-for-bit reproducible binary
build across toolchains.

## Release verification

Before publishing, verify the Debian package metadata and dependency resolution,
extract both binary formats into new locations, check for missing libraries,
and run the relocated launchers with `--version` and an SVG-to-PNG export.
Inspect the full source archive for its submodule files and notices, then verify
the release checksums and download links.

The project's offline AI regression targets cover layout search, workflow
scheduling, planner/asset behavior, connectors, and SVG handling. They use
controlled fixtures and do not measure real-provider visual quality. For example:

```sh
cmake --build build --target test_ai_layout_search test_ai_workflow \
  test_ai_design_agents test_ai_design_services test_ai_design_assets \
  test_ai_connection -j2
build/bin/test_ai_layout_search
build/bin/test_ai_workflow
build/bin/test_ai_design_agents
build/bin/test_ai_design_services
build/bin/test_ai_design_assets
python3 testfiles/src/ai-connection-test.py build/bin/test_ai_connection
```

GoogleTest-based checks require GoogleTest to be installed when configuring.
The standalone AI targets remain available without it. Display-dependent UI
checks are described in the [assistant guide](../../doc/ai-assistant-ui.md).

## Generate inside selection

1. Select one visible, unlocked, closed vector shape or closed compound path.
   Groups, open paths, and boundaries with existing clips/masks are unsupported.
2. Open AI Assistant and choose **Generate inside selection**.
3. Describe the contents and Send. Optionally attach an image using **Use as
   reference**. Local tracing remains a Create workflow.
4. Review the clipped preview and choose Insert. The original boundary remains;
   editable artwork is grouped above it with a locally generated clipping path.
5. **Ctrl+Z** undoes the insertion. The saved SVG retains its contents and clip.

The captured boundary, holes, and parent transforms determine the clip. Changing
the document while waiting invalidates the preview. Restored conversation artwork
has no live boundary snapshot and must be regenerated for insertion into a shape.
Output fits the boundary's bounding rectangle; composition quality depends on
the model.

## Source, credits, and notices

This checkout and release use GTK4. They are separate from any older GTK3 beta
archive. Preserve [COPYING](../../COPYING), [AUTHORS](../../AUTHORS),
[TRANSLATORS](../../TRANSLATORS), [LICENSES](../../LICENSES/),
[WINKSPACE-NOTICES.txt](../../WINKSPACE-NOTICES.txt), and component notices.
The package includes collected component notice files with their source paths.
The complete source asset accompanies the binaries in the same release.

See **Help → About WinkSpace** for application credits and licenses. The project
extends Inkscape and does not claim authorship of the upstream editor.
