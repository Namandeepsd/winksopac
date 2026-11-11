# AI Assistant sidebar

The conversation occupies the flexible middle of the sidebar. The header holds
the design brief, new conversation and connection settings. Those settings open
in popovers so they do not compete with messages for vertical space.

The bottom composer grows from a compact two-line input to a capped, scrollable
editor. Enter sends; Shift+Enter inserts a newline. Ctrl+Enter remains supported.
Input-method composition is handled before sending. The plus menu accepts image
files or clipboard images; Ctrl+V still pastes an image when the clipboard offers
one. Attachment reference/tracing controls appear only when relevant.

The operation menu contains **Create**, **Edit**, **Plan** and **Commands**.
Edit selects the existing workflow from the canvas context: captured vector
regions use geometry editing, captured image regions use region editing, selected
nodes use geometry editing, and ordinary selections use whole-object editing.
The context line shows which route is active. Capture a region using the existing
canvas right-click action; the detailed implementation modes are no longer menu
choices. Old region/geometry conversations reopen in Edit.

The **+** menu contains Add image, Copy prompt, Paste and Fit page to drawing.
Copy prompt copies selected text or the whole prompt; Paste inserts clipboard
text or, where supported, an image. Standard Ctrl+C/Ctrl+V and text context menus
remain available. Plan and Commands still allow text paste while disabling image
attachments. The send button becomes Stop while a request runs.

See [AI workflows](ai-workflows.md) for Plan's image selection and preview flow.

User messages have a subtle accent and a reuse-prompt button. Assistant feedback
is inline, with one updated status per request instead of repeated cards. The
formatter supports headings, bullets, bold, italic, inline code and fenced code;
it escapes untrusted markup and does not load external content. Response source
remains available in a collapsed expander. Previews retain Insert/Apply/Discard,
and the footer exposes the document's native Undo/Redo actions.

New prompts scroll to their latest feedback. While reading earlier messages,
updates preserve the scroll position; **Latest** returns to the newest content.
History and drafts keep using the existing conversation persistence layer.

All styling is scoped to the assistant and uses GTK theme colours, including
light and dark palettes. The collapsed operation and attachment menus ellipsize
at narrow widths; their popup lists retain complete labels.

## Verification

`testfiles/src/ai-chat-format-test.cpp` checks escaped Pango markup, incomplete
Markdown, Unicode and fenced code. It can run without a display or GoogleTest:

```sh
c++ -std=c++20 -Isrc testfiles/src/ai-chat-format-test.cpp -o /tmp/ai-chat-format-test $(pkg-config --cflags --libs pango)
/tmp/ai-chat-format-test
```

`testfiles/src/ai-assistant-ui-test.cpp` is an explicit, display-dependent GTK
smoke test linked against `inkscape_base`. It opens a temporary window, uses a
synthetic landscape, checks layout, status coalescing, placeholder, keyboard,
scroll retention, attachments, busy controls, and completed workflow plans with
source links, downloaded image thumbnails and inclusion checkboxes. It saves only
panel images.
It never sends a model request or opens a user drawing. Arguments are width,
height, `dark`/`light`, and output PNG. Use an isolated `INKSCAPE_PROFILE_DIR` and
set `GTK_THEME=Adwaita` or `GTK_THEME=Adwaita:dark` when checking both palettes.
The UI test is an opt-in executable, not part of unattended CTest runs.

Existing `ai-region-test.cpp` covers immutable previews, insertion/edit geometry,
native commands, page fitting and Undo/Redo independently of the widget layout.
