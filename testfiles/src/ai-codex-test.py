#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Standalone Codex transport regression test. Uses a fake CLI; no model calls.

Run with the compiled smoke driver path as the first argument.
The driver reads a chat-completions request on stdin and calls run_codex().
"""
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    driver = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="codex-transport-test-") as directory:
        root = Path(directory)
        fake = root / "codex"
        fake.write_text("#!" + sys.executable + "\n" + '''
import json, os, pathlib, sys, time
args = sys.argv[1:]
prompt = sys.stdin.read()
images = [pathlib.Path(args[i+1]).read_bytes().hex() for i,a in enumerate(args) if a == '--image']
pathlib.Path(os.environ['CODEX_TEST_LOG']).write_text(json.dumps({'args':args,'prompt':prompt,'images':images}))
mode = os.environ.get('CODEX_TEST_MODE', '')
if mode == 'cancel': time.sleep(30)
if mode == 'fail':
    print('Test login failure', file=sys.stderr)
    sys.exit(2)
pathlib.Path(args[args.index('--output-last-message')+1]).write_text('' if mode == 'empty' else '{"targets":["p0"],"operation":{"type":"recolor","color":"#FF69B4"}}')
''')
        fake.chmod(0o700)
        env = dict(os.environ, PATH=directory + os.pathsep + os.environ.get("PATH", ""), CODEX_TEST_LOG=str(root / "log.json"))
        body = {"model": "test-model", "messages": [
            {"role": "system", "content": "Return only JSON."},
            {"role": "user", "content": [
                {"type": "text", "text": "Make it pink. $(do-not-execute)"},
                {"type": "image_url", "image_url": {"url": "data:image/png;base64," + base64.b64encode(b"image-bytes").decode()}}
            ]}
        ]}
        result = subprocess.run([driver], input=json.dumps(body), text=True, capture_output=True, env=env, timeout=10)
        assert result.returncode == 0, result.stderr
        reply = json.loads(result.stdout)["choices"][0]
        assert json.loads(reply["message"]["content"])["operation"]["color"] == "#FF69B4"
        record = json.loads((root / "log.json").read_text())
        args = record["args"]
        assert args[args.index("--sandbox") + 1] == "read-only"
        assert args[args.index("--model") + 1] == "test-model"
        assert "$(do-not-execute)" in record["prompt"]
        assert record["images"] == [b"image-bytes".hex()]
        assert not Path(args[args.index("-C") + 1]).exists(), "Temporary artwork was not removed"
        for mode in ("fail", "empty", "cancel"):
            env["CODEX_TEST_MODE"] = mode
            result = subprocess.run([driver] + (["--cancel"] if mode == "cancel" else []), input=json.dumps(body), text=True, capture_output=True, env=env, timeout=10)
            assert result.returncode != 0, mode
            record = json.loads((root / "log.json").read_text())
            assert not Path(record["args"][record["args"].index("-C") + 1]).exists(), mode
        print("PASS: Codex image delivery, prompt transport, model selection, output parsing, failures, cancellation and temporary-file cleanup")


if __name__ == "__main__":
    main()
