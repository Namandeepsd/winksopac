# WinkSpace before the GTK3 migration

Baseline: Inkscape 1.5-dev, upstream commit 64a5e0e65b. The complete working
implementation is preserved on `winkspace/development-1.5` before porting.

## Feature and dependency inventory

| Layer | Files | Responsibilities and dependencies |
| --- | --- | --- |
| Workflow core | `src/ai/*` | C++20, nlohmann JSON, GLib, libxml2, threads; task DAG, factory-created agents, scoped revisioned context, messages, immutable outputs, provenance, cancellation, persistence; no GTK dependency |
| Design agents | `src/ai/design-agents.*` | Model-backed planning and Commons candidate selection; bounded schema validation and host-controlled task decomposition |
| Layout search | `src/ai/layout-search.*` | Backtracking, MRV, forward checking and normalized section boxes; no UI dependency |
| Connectors | `ai-connection`, `ai-codex` | GIO subprocesses, libsoup3 HTTP, saved CLI login, compatible APIs, Claude stream-JSON images, custom bridges, cancellation, deadlines and capability checks |
| Retrieval/assets | `ai-design-services`, `ai-design-assets` | Commons discovery/download, licence provenance, bounded decoding/sanitization, vector/raster composition and background ownership |
| Editor operations | `ai-actions`, `ai-region`, `ai-vector-edit` | ObjectSet, page manager, XML, drawing/raster snapshots, selected-node/region edits, native commands, freshness validation and geometry checks |
| Assistant UI | `ai-assistant*` | Create/Edit/Plan/Commands, previews, Insert/Apply/Discard, Undo/Redo, image attachments, context brief, connection settings and incremental status |
| History | `ai-conversation` | Per-document chat, drafts, briefs, accepted artwork; bounded versioned atomic private files |
| Integration | dialog registry, context menu, command toolbar, canvas | Discoverable sidebar, region capture, standalone preview handling |

All generated SVG is validated before preview/import. Existing-document edits
operate on snapshots and reject stale proposals. Document application uses native
Undo transactions. Agents receive value context rather than live GTK widgets or
SPDocument pointers. User acceptance remains the document mutation boundary.

## Compatibility audit

- GTK4/gtkmm4: Texture, Picture, FileDialog, CSS classes/providers, Box append,
  widget children, popover/menu APIs, clipboard/image async APIs, key controllers,
  event propagation, snapshot rendering, visibility and native icon methods.
- Stable Inkscape 1.4.4 uses GTK3/gtkmm3. Verify editor APIs against the actual
  stable source, especially SVG previews, ObjectSet/PageManager, XML attribute
  iteration, drawing snapshots, node selection, context menus and dialog creation.
- The async library exists in stable; verify signatures rather than copying the
  GTK4 host wholesale. Keep workflow/model services independent of GTK.
- C++20 is needed by stop tokens, jthread, contains and comparison operators.
  Inspect stable compiler options and require C++20 for WinkSpace if necessary.
- Python is used by regression fixtures and inherited Inkscape extensions, not
  the core AI runtime. Connector fixtures use the Python standard library.
  Stable extension/submodule revisions must match the stable release.
- Reuse core algorithms, JSON transport, validation and persistence unchanged
  wherever possible. Put UI compatibility primitives in an explicit GTK3 adapter.

## Acceptance gate

A build or successful launch is not migration completion. The stable build must
exercise actual assistant generation and editing requests through the connector,
preview, user application, Undo/Redo, save/reload and export. Include agent
planning/discovery, image inputs, unavailable providers, cancellation, malformed
SVG, repeated requests, large documents and UI responsiveness. Deterministic
provider fixtures prove host workflows; live model tests must be reported
separately, with provider/model and results recorded. Any missing gate keeps the
migration incomplete and must be disclosed.
