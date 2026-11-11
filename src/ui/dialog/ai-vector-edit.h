// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_AI_VECTOR_EDIT_H
#define INKSCAPE_UI_DIALOG_AI_VECTOR_EDIT_H
#include <memory>
#include <map>
#include <string>
#include <vector>
#include <2geom/pathvector.h>
#include <2geom/rect.h>
class SPDocument;
class SPItem;
namespace Inkscape::UI::Dialog {
// A snapshot-scoped vocabulary of existing geometry. Callers validate document
// freshness and own the undo transaction. Provider output never becomes SVG.
class AIVectorEdit {
public:
    struct Plan {
        std::vector<std::size_t> targets;
        std::string type, color, paint, reason, clarification;
        std::map<std::size_t, std::string> colors;
        double x = 1, y = 1, angle = 0;
    };
    AIVectorEdit(SPDocument &document, SPItem *scope, Geom::OptRect hint = {}, std::string hint_path = {});
    std::string catalog() const;
    std::string reference_png(SPDocument &document, bool numbered, bool closeup = false) const;
    Plan parse(std::string const &response) const;
    void apply(SPDocument &document, Plan const &plan) const;
    std::unique_ptr<SPDocument> selection_preview(SPDocument &document, Plan const &plan) const;
    std::string summary(Plan const &plan) const;
    // Fit a preview copy to the affected area in document coordinates.
    void fit_preview(SPDocument &document, Plan const &plan) const;
    std::string const &scope_id() const { return _scope_id; }
private:
    struct Candidate {
        std::string id, object_id;
        int subpath = -1;
        bool has_fill = false, has_stroke = false;
        Geom::PathVector geometry;
        Geom::Rect bounds;
        Geom::Affine local_to_doc;
        std::string source_path;
    };
    std::vector<Candidate> _candidates;
    std::string _scope_id, _hint_path;
    Geom::Rect _bounds;
    Geom::OptRect _hint;
    std::unique_ptr<SPDocument> marked_document(SPDocument &document, std::vector<std::size_t> const &targets, bool numbered) const;
};
}
#endif
