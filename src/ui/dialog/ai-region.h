// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_AI_REGION_H
#define INKSCAPE_UI_DIALOG_AI_REGION_H

#include <string>
#include <vector>
#include <2geom/rect.h>

class SPDocument;
class SPItem;
class SPShape;

namespace Inkscape::UI::Dialog {

// All region geometry is in document pixels, independently of layer transforms.
// Callers own the undo transaction and must reject stale document snapshots.
class AIRegion
{
public:
    AIRegion(SPItem *target, SPShape *outline);
    static bool is_outline(SPShape const *outline);
    // Topmost visible, unlocked sibling beneath the boundary that overlaps it.
    static SPItem *target_below(SPShape *outline);
    // Capture the boundary as session geometry; caller owns the undo step.
    void detach_outline(SPDocument &document);
    bool detached() const { return _detached; }
    std::string const &target_id() const { return _target_id; }
    std::vector<SPItem *> node_targets(SPDocument &document) const;
    bool accepts_selection(SPDocument &document, std::vector<SPItem *> const &items) const;
    // patch must already have passed the assistant's generated-SVG validator.
    // nullptr constructs a clipped reference instead of a replacement.
    SPItem *compose(SPDocument &document, SPDocument *patch, std::string const &dark_color = {}) const;
    std::string reference_png(SPDocument &document, bool full_artwork = false) const;
    std::string const &boundary_path() const { return _path; }
    Geom::Rect const &bounds() const { return _bounds; }
    std::string const &outline_id() const { return _outline_id; }

private:
    std::string _target_id, _outline_id, _path;
    Geom::Rect _bounds;
    bool _detached = false;
};

}
#endif
