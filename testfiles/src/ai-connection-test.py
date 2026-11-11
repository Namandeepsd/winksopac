#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline connectors test: fake executables and a local HTTP fixture only."""
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

DRIVER = str(Path(sys.argv[1]).resolve())
BODY = {"model": "test-model", "stream": False, "max_completion_tokens": 64,
        "messages": [{"role": "user", "content": "Hello $(not-a-shell-command)"}]}
ENVELOPE = {"choices": [{"message": {"content": "OK"}, "finish_reason": "stop"}]}


def invoke(config, env=None, success=True):
    response = subprocess.run([DRIVER], input=json.dumps(config), text=True,
                              capture_output=True, env=env, timeout=10)
    assert (response.returncode == 0) == success, response.stderr + response.stdout
    if success:
        assert json.loads(response.stdout)["choices"][0]["message"]["content"] == "OK"
    return response


def main():
    subprocess.run([DRIVER, "--endpoints"], check=True)
    with tempfile.TemporaryDirectory(prefix="wink-connectors-test-") as directory:
        root = Path(directory)
        fake = root / "fake-cli"
        fake.write_text("#!" + sys.executable + "\n" + '''
import json, os, pathlib, sys, time
args = sys.argv[1:]
prompt = sys.stdin.read()
pathlib.Path(os.environ['CONNECTOR_TEST_LOG']).write_text(json.dumps({'args':args,'input':prompt,'cwd':os.getcwd()}))
mode = os.environ.get('CONNECTOR_TEST_MODE', '')
if mode == 'slow': time.sleep(30)
if mode == 'overflow':
    sys.stdout.write('x' * (5 * 1024 * 1024))
    sys.exit(0)
if mode == 'stderr-overflow':
    sys.stderr.write('x' * (300 * 1024))
    sys.exit(1)
if mode == 'fail': sys.exit(2)
if mode == 'malformed': print('not json'); sys.exit(0)
if '--print' in args:
    print(json.dumps({'type':'system', 'subtype':'init'}))
    print(json.dumps({'type':'result', 'subtype':'error_during_execution' if mode == 'error' else 'success', 'is_error':mode == 'error', 'result':'OK'}))
elif '--output-last-message' in args:
    pathlib.Path(args[args.index('--output-last-message')+1]).write_text('OK')
else:
    print(json.dumps({'choices':[{'message':{'content':'OK'}, 'finish_reason':'stop'}]}))
''')
        fake.chmod(0o700)
        log = root / "log.json"
        env = dict(os.environ, CONNECTOR_TEST_LOG=str(log))
        for provider in ("codex", "claude", "bridge"):
            config = dict(provider=provider, executable=str(fake), body=BODY,
                          arguments=["--test-argument", "$(literal)"])
            invoke(config, env)
            record = json.loads(log.read_text())
            assert not Path(record["cwd"]).exists() if provider != "codex" else True
            assert "$(not-a-shell-command)" in record["input"]
            if provider == "claude":
                args = record["args"]
                assert "--safe-mode" in args and "--no-session-persistence" in args
                assert args[args.index("--tools") + 1] == ""
                assert "--strict-mcp-config" in args
                assert args[args.index("--input-format") + 1] == "stream-json"
                assert args[args.index("--output-format") + 1] == "stream-json" and "--verbose" in args
                assert json.loads(record["input"])["message"]["role"] == "user"
            if provider == "bridge": assert record["args"] == config["arguments"]
            invoke(dict(config, **{"async": True}), env)
            env["CONNECTOR_TEST_MODE"] = "fail"
            invoke(config, env, False)
            env["CONNECTOR_TEST_MODE"] = "slow"
            invoke(dict(config, cancel=True), env, False)
            invoke(dict(config, cancel=True, **{"async": True}), env, False)
            response = invoke(dict(config, timeout=1), env, False)
            assert "timed out" in response.stderr
            env.pop("CONNECTOR_TEST_MODE")
        image = {"type": "image_url", "image_url": {"url": "data:image/png;base64," + base64.b64encode(b"pixels").decode()}}
        pictured = dict(BODY, messages=[{"role": "user", "content": [{"type": "text", "text": "Inspect"}, image]}])
        config = dict(provider="claude", executable=str(fake), body=pictured)
        invoke(config, env)
        parts = json.loads(json.loads(log.read_text())["input"])["message"]["content"]
        source = next(part["source"] for part in parts if part["type"] == "image")
        assert source["media_type"] == "image/png" and base64.b64decode(source["data"]) == b"pixels"
        invoke(dict(config, provider="bridge", images=False), env, False)
        invoke(dict(config, provider="bridge", images=True), env)
        for mode in ("error", "malformed", "overflow", "stderr-overflow"):
            env["CONNECTOR_TEST_MODE"] = mode
            invoke(config, env, False)
            assert not Path(json.loads(log.read_text())["cwd"]).exists()
        env.pop("CONNECTOR_TEST_MODE")

    records = []
    class Fixture(BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_POST(self):
            data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            records.append((self.path, data, self.headers.get("Authorization")))
            if self.path == "/redirect":
                self.send_response(307)
                self.send_header("Location", "/must-not-follow")
                self.end_headers()
                return
            if self.path == "/unauthorized":
                self.send_response(401)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(ENVELOPE).encode())
    server = ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        base = f"http://127.0.0.1:{server.server_port}"
        config = dict(provider="ollama", endpoint=base + "/v1/chat/completions", body=BODY)
        invoke(config)
        assert records[-1][2] is None
        assert records[-1][1]["max_tokens"] == 64
        invoke(dict(config, completion_token_limit=True))
        assert records[-1][1]["max_completion_tokens"] == 64 and "max_tokens" not in records[-1][1]
        invoke(dict(config, **{"async": True}))
        invoke(dict(config, key="private-test-key"))
        assert records[-1][2] == "Bearer private-test-key"
        assert "401" in invoke(dict(config, endpoint=base + "/unauthorized"), success=False).stderr
        previous = len(records)
        invoke(dict(config, endpoint=base + "/redirect"), success=False)
        assert len(records) == previous + 1
        invoke(dict(config, key="bad\nkey"), success=False)
        invoke(dict(config, body=pictured), success=False)
        invoke(dict(config, body=pictured, images=True))
        assert records[-1][1]["messages"][0]["content"][1] == image
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("PASS: CLI adapters, image bytes, login failures, cancellation, deadlines, cleanup, output bounds, loopback HTTP, optional keys, redirects and capability checks")

if __name__ == "__main__":
    main()
