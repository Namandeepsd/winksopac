# Inkscape commands in the AI Assistant

For page fitting, use the **Fit page to drawing** action in the composer’s **+** menu, or type
“fit the page to the scenery” and press Send in any mode. This standalone request
resizes the actual page immediately, without contacting a model or requiring
Apply. It fits the entire drawing even if a smaller object is selected. Use
“fit page to selection” to fit only the selected objects. Ctrl+Z undoes the resize.
Generated artwork must already be inserted on the canvas.

Choose **Commands** in the AI Assistant mode dropdown. This works
with the [configured connector](ai-connections.md), using a vision-capable model.

Examples:

- “Fit the page to the entire scenery.” No selection is required.
- “Resize this page to 800 by 600 pixels.”
- “Group the selected objects, then move the group 20 pixels right.”
- “Align the selected objects horizontally by their centers.”
- “Make the selected object twice as wide.”
- “Move the selected object one step forward.”

The assistant receives the document image, selected object IDs, page bounds,
object types, labels, bounds, paint information and bounded path/style data.
It returns a structured command sequence. Inkscape validates that sequence and
runs it on a document copy to produce the preview. **Apply** repeats the
validated sequence on the current document in one undo transaction. If the
document changed since the request, generate a fresh proposal.

The initial command set is:

| Command | Implementation |
| --- | --- |
| Fit/resize page | Inkscape PageManager |
| Move, scale, rotate | Inkscape ObjectSet |
| Group, ungroup | Inkscape ObjectSet |
| Raise/lower one step | Inkscape ObjectSet |
| Align to combined bounds | Inkscape object bounds and ObjectSet movement |
| Fill, stroke, opacity, stroke width | Restricted SVG style changes |
| Replace a path's geometry | Validated `d` attribute on an existing ordinary path |

This is an allowlisted interface, not unrestricted access to every menu action.
There is no arbitrary shell command, event handler, external URL, or XML-node
insertion operation. Unsupported requests produce a clarification. Complex
subpath/region changes use **Edit** with selected nodes or a captured region, while new
illustrations use **Create**.

Native object commands operate on complete objects or groups. Path geometry
edits require an explicit existing path ID and its original geometry in the
catalog. Referenced, locked and hidden objects cannot be modified through this
interface. At most 16 commands can be proposed together.

Page commands affect the page selected when the request was sent. “Fit to
drawing” includes the whole drawing; “fit to selection” uses the captured
selection. Group/ungroup update the selection for subsequent commands.
