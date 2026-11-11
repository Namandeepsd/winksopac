# Local Codex connection

The AI Assistant can launch the installed Codex CLI using its saved login.
Tested with Codex CLI 0.166.0. Codex inference still uses its online service.

1. Install Codex and sign in with `codex login` if needed.
2. Open AI Assistant → the **Connection settings** gear in the header.
3. Choose **Codex · terminal login** in the connector picker. It is selected
   initially when Codex is found on PATH or at `~/.local/bin/codex`.
4. Leave Model blank for the CLI default, or enter an available Codex model ID.
   URL and API key fields are unused in this mode.
5. Select artwork, describe the edit, and review the preview before Apply.

Each request sends the prompt and attached artwork images to `codex exec`.
Inkscape never reads the saved authentication credentials. Requests run in a
private temporary directory with an ephemeral session and read-only sandbox;
user CLI configuration, shell tools, apps, and multiple agents are disabled.
Temporary images and the final-response file are removed after completion or
cancellation. Existing SVG and geometry validators still check every proposal.

If a request fails, check `codex login status`. The panel reports CLI failures.
Choose another connector to use Claude Code, Kimi, a local model server or a
custom compatible endpoint. See [Model connections](ai-connections.md) for setup.

## Transport regression tests

These tests use a fake CLI and do not contact a model:

```sh
c++ -std=c++20 -Isrc testfiles/src/ai-codex-test.cpp src/ui/dialog/ai-codex.cpp -o /tmp/ai-codex-test -pthread $(pkg-config --cflags --libs gio-2.0)
python3 testfiles/src/ai-codex-test.py /tmp/ai-codex-test
```

The integration uses the documented non-interactive CLI interface:
https://learn.chatgpt.com/docs/non-interactive-mode
