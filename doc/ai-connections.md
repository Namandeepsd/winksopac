# Model connections

Open **AI Assistant → Connection settings** (the gear). Choose a connector, set
its model, then click **Test connection**. The test sends a short text request;
it verifies an actual response, not just that an executable exists. It may use
provider credits. It does not verify image quality or image support.

The same connection is used by Create, Edit, Commands, Planner and Asset
Discovery. Changing providers does not require starting a new document.

## Available connectors

| Connector | Setup |
| --- | --- |
| Codex · terminal login | Install Codex, sign in with `codex login`, then select this connector. |
| Claude Code · terminal login | Install a recent Claude Code with `--safe-mode` support, sign in with `claude auth login`, then select this connector. |
| Kimi · API | Enter your Kimi API key and the exact model ID available to your account. The chat-completions address is prefilled. |
| Ollama · local server | Start Ollama, download a model, and enter its installed model ID. The local endpoint is prefilled; the key can remain empty. |
| LM Studio · local server | Load a model, start its local API server, and enter its model ID. Adjust the prefilled URL if the server uses a different port. |
| Custom · compatible API | Enter a complete chat-completions URL, model ID and an API key if the service requires one. |
| Custom · CLI bridge | Select a trusted executable implementing the JSON bridge contract below. This can integrate future CLIs without changing the assistant UI. |

CLI executables are discovered on PATH and in `~/.local/bin`. Set a full path if
your desktop launcher cannot find the tool. **Copy terminal sign-in command**
copies a command for you to run in your terminal; it does not launch a terminal,
install anything or sign you in automatically. After login, use **Test connection**.
WinkSpace does not read the CLI's saved authentication credentials. CLI inference
may still use an online provider; a terminal connector does not imply offline AI.

Each connector remembers its model, endpoint, executable and capability settings
across launches. API keys and bridge arguments remain in memory for the panel's
lifetime. Switching away and back retains those session values. Requests capture
an immutable connection snapshot, and controls cannot change during a request.

## Image input and API compatibility

For API models and custom bridges, enable **This model accepts image input** only
when both the selected model and endpoint support vision. The flag is saved per
connector, so update it when changing models. Artwork edits, Commands and image
attachments may send images. Unsupported requests stop before inference with an
explanation. Text-only models can still plan designs and create artwork from
text. Codex and Claude adapters support image transport directly.

**Use max_completion_tokens** selects that API output-limit field. Leave it off
for servers using `max_tokens`; enable it when required by your API/model. This
setting changes only HTTP requests. No automatic retry resends a request with
different parameters, so compatibility errors cannot silently double usage.

Hosted endpoints require HTTPS. Plain HTTP is accepted only for `localhost`,
`127.0.0.1` and `::1`, enabling local servers without transmitting keys over an
unencrypted remote connection. Redirects are disabled. Configure the final
endpoint URL directly. Authentication errors, unavailable servers, unsupported
models, rate limits and timeouts have separate messages. Stop cancels active
requests; closing the panel also cancels them.

## Custom CLI bridge contract

The bridge is an executable path or program name, plus optional arguments as a
JSON string array, for example `["--profile", "design"]`. Arguments are passed
literally; no shell expansion occurs. Authenticate with the underlying tool
separately if required. Native vendor CLIs do not automatically implement this
contract: write an adapter, or use their compatible API instead.

WinkSpace writes one UTF-8 JSON request to stdin and closes it:

```json
{
  "model": "your-model",
  "stream": false,
  "max_completion_tokens": 16384,
  "messages": [
    {"role": "system", "content": "Return the requested SVG or JSON."},
    {"role": "user", "content": "Draw a blue circle."}
  ]
}
```

The executable must exit successfully and write exactly one response envelope to
stdout. Diagnostic logging belongs on stderr. Example:

```json
{"choices":[{"message":{"content":"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><circle cx='50' cy='50' r='40' fill='blue'/></svg>"},"finish_reason":"stop"}]}
```

Vision requests use chat-completions `image_url` content blocks with inline data
URLs. The bridge must handle them when image input is enabled. The executable
runs in a private temporary working directory. It is trusted local code, not a
sandboxed plugin. Requests default to a 120-second deadline; planning uses the
remaining workflow task deadline. Stdout is bounded to 4 MiB and stderr to
256 KiB. Cancellation terminates the direct bridge process; a bridge spawning
other processes is responsible for their cleanup. Temporary workspaces are
removed after completion or failure.

## Implementation and verification

`AIConnection` is the shared configuration; `ConnectionPreset` declares connector
identity, transport, setup hints and default image capability. `run_connection`
and its asynchronous wrapper normalize every response to the envelope above.
The UI and design agents use these same functions. To add a compatible service,
add a preset. A new native CLI protocol needs its own transport adapter; the
bridge contract avoids needing one for each third-party tool.

Claude requests use stream-JSON image/text input and consume the final result
event from stream-JSON output. Safe mode, disabled tools/MCP, disabled hooks and
no session persistence keep the request focused on inference. Codex retains its
existing isolated transport and image-file handling. SVG and document-edit
validation remain mandatory after inference.

Build and run the offline tests:

```sh
cmake --build build --target test_ai_connection test_ai_design_services -j2
python3 testfiles/src/ai-connection-test.py build/bin/test_ai_connection
build/bin/test_ai_design_services
```

The connector test uses fake CLIs and a loopback HTTP fixture, with no external
model calls. It covers prompt/model/image transport, literal arguments,
capability rejection, synchronous/asynchronous completion, authentication errors,
cancellation, deadlines, response bounds, cleanup, optional API keys and redirects.
A native UI smoke test also checks connector switching and settings retention.
Real service accounts and installed CLI versions must still be verified using
**Test connection**.

Protocol references: [Claude Code CLI](https://code.claude.com/docs/en/cli-reference),
[Ollama compatibility](https://docs.ollama.com/api/openai-compatibility), and
[Kimi chat completions](https://platform.kimi.ai/docs/api/chat).
