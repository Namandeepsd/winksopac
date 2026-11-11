// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai-region.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <memory>
#include <stdexcept>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <2geom/pathvector.h>
#include "document.h"
#include "display/cairo-utils.h"
#include "helper/pixbuf-ops.h"
#include "object/sp-defs.h"
#include "object/sp-shape.h"
#include "object/sp-path.h"
#include "object/sp-item-group.h"
#include "object/sp-root.h"
#include "svg/svg.h"
#include "xml/document.h"
#include "xml/node.h"

namespace Inkscape::UI::Dialog {
namespace {
using Node = Inkscape::XML::Node;

Node *add(SPDocument &doc, Node *parent, char const *name)
{
    auto node = doc.getReprDoc()->createElement(name);
    parent->appendChild(node);
    Inkscape::GC::release(node);
    return node;
}

void rectangle(Node *node, Geom::Rect const &bounds)
{
    node->setAttributeSvgDouble("x", bounds.left());
    node->setAttributeSvgDouble("y", bounds.top());
    node->setAttributeSvgDouble("width", bounds.width());
    node->setAttributeSvgDouble("height", bounds.height());
}

void check_bounds(Geom::OptRect const &bounds)
{
    if (!bounds || bounds->width() <= 0 || bounds->height() <= 0) {
        throw std::runtime_error("The region must have a non-zero width and height.");
    }
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (!std::isfinite(bounds->min()[axis]) || !std::isfinite(bounds->max()[axis]) ||
            std::abs(bounds->min()[axis]) > 1e7 || std::abs(bounds->max()[axis]) > 1e7) {
            throw std::runtime_error("The region has invalid or excessively large bounds.");
        }
    }
}
}

bool AIRegion::is_outline(SPShape const *outline)
{
    auto curve = outline ? outline->curve() : nullptr;
    return curve && curve->size() == 1 && curve->front().closed();
}

SPItem *AIRegion::target_below(SPShape *outline)
{
    if (!outline) return nullptr;
    outline->document->ensureUpToDate();
    if (!is_outline(outline) || !outline->parent || outline->isHidden() || outline->isLocked()) return nullptr;
    auto bounds = outline->documentGeometricBounds();
    if (!bounds) return nullptr;
    SPItem *target = nullptr;
    for (auto &sibling : outline->parent->children) {
        if (&sibling == outline) break;
        auto item = dynamic_cast<SPItem *>(&sibling);
        if (!item || item->isHidden() || item->isLocked()) continue;
        auto item_bounds = item->documentVisualBounds();
        if (item_bounds && bounds->intersects(*item_bounds)) target = item;
    }
    return target;
}

AIRegion::AIRegion(SPItem *target, SPShape *outline)
{
    if (!target || !outline || target == outline || !target->getId() || !outline->getId() ||
        target->parent != outline->parent) {
        throw std::runtime_error("Select artwork and a closed path above it in the same group or layer.");
    }
    if (outline->isLocked() || outline->isHidden() || outline->isReferenced()) {
        throw std::runtime_error("The region outline must be visible, unlocked and unreferenced.");
    }
    auto curve = outline->curve();
    if (!is_outline(outline)) {
        throw std::runtime_error("Draw one closed loop with the Pencil or Bezier tool for the region outline.");
    }
    auto path = *curve * outline->i2doc_affine();
    auto bounds = path.boundsExact();
    check_bounds(bounds);
    auto target_bounds = target->documentVisualBounds();
    check_bounds(target_bounds);
    if (!bounds->intersects(*target_bounds)) {
        throw std::runtime_error("The region outline does not overlap the selected artwork.");
    }
    _bounds = *bounds;
    _path = sp_svg_write_path(path);
    _target_id = target->getId();
    _outline_id = outline->getId();
}

std::vector<SPItem *> AIRegion::node_targets(SPDocument &doc) const
{
    std::vector<SPItem *> paths;
    std::function<void(SPItem *)> collect = [&](SPItem *item) {
        if (!item || item->isHidden() || item->isLocked()) return;
        if (dynamic_cast<SPPath *>(item)) {
            paths.push_back(item);
        } else if (dynamic_cast<SPGroup *>(item)) {
            for (auto &child : item->children) {
                if (auto child_item = dynamic_cast<SPItem *>(&child)) collect(child_item);
            }
        }
    };
    collect(dynamic_cast<SPItem *>(doc.getObjectById(_target_id.c_str())));
    return paths;
}

bool AIRegion::accepts_selection(SPDocument &doc, std::vector<SPItem *> const &items) const
{
    auto target = doc.getObjectById(_target_id.c_str());
    return target && !items.empty() && std::all_of(items.begin(), items.end(), [&](SPItem *item) {
        return item && item->document == &doc && (item == target || target->isAncestorOf(item));
    });
}

void AIRegion::detach_outline(SPDocument &doc)
{
    if (_detached) throw std::runtime_error("This region boundary has already been captured.");
    auto outline = dynamic_cast<SPShape *>(doc.getObjectById(_outline_id.c_str()));
    auto target = dynamic_cast<SPItem *>(doc.getObjectById(_target_id.c_str()));
    if (!outline || !target || outline->parent != target->parent || outline->isReferenced()) {
        throw std::runtime_error("The region artwork or boundary is no longer available.");
    }
    outline->deleteObject(true);
    _detached = true;
    doc.ensureUpToDate();
}

SPItem *AIRegion::compose(SPDocument &doc, SPDocument *patch, std::string const &dark_color) const
{
    bool recolor = !dark_color.empty();
    if (recolor && (dark_color.size() != 7 || dark_color[0] != '#' ||
        !std::all_of(dark_color.begin() + 1, dark_color.end(), [](unsigned char c) { return std::isxdigit(c); }))) {
        throw std::runtime_error("Recoloring requires a six-digit hexadecimal color.");
    }
    if (recolor && patch) throw std::runtime_error("A region edit cannot both replace and recolor artwork.");
    bool replacement_requested = patch || recolor;
    doc.ensureUpToDate();
    auto original = dynamic_cast<SPItem *>(doc.getObjectById(_target_id.c_str()));
    auto outline = dynamic_cast<SPShape *>(doc.getObjectById(_outline_id.c_str()));
    if (!original || (!_detached && (!outline || original->parent != outline->parent))) {
        throw std::runtime_error("The region's original artwork or outline is no longer available.");
    }
    auto parent = original->getRepr()->parent();
    auto parent_item = dynamic_cast<SPItem *>(original->parent);
    auto parent_transform = parent_item ? parent_item->i2doc_affine() : doc.getRoot()->i2doc_affine();
    auto original_transform = original->i2doc_affine();
    if (parent_transform.isSingular()) throw std::runtime_error("The artwork's parent has a singular transform.");
    auto original_bounds = original->documentVisualBounds();
    check_bounds(original_bounds);

    // Locally constructed clipping/masking never comes from the model. Keep
    // the original object (and its IDs, image data, paths and effects) intact.
    g_autofree char *uuid = g_uuid_string_random();
    std::string clip_id = std::string("ai-region-clip-") + uuid;
    std::string mask_id = std::string("ai-region-mask-") + uuid;
    auto defs = doc.getDefs()->getRepr();
    auto clip = add(doc, defs, "svg:clipPath");
    clip->setAttribute("id", clip_id);
    clip->setAttribute("clipPathUnits", "userSpaceOnUse");
    auto shape = add(doc, clip, "svg:path");
    shape->setAttribute("d", _path);
    shape->setAttribute("clip-rule", "nonzero");

    auto wrapper = doc.getReprDoc()->createElement("svg:g");
    parent->addChild(wrapper, original->getRepr());
    Inkscape::GC::release(wrapper);
    wrapper->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(parent_transform.inverse()));
    wrapper->setAttribute("inkscape:label", replacement_requested ? "AI region edit" : "AI region reference");
    auto preserved = add(doc, wrapper, "svg:g");

    if (replacement_requested) {
        auto mask = add(doc, defs, "svg:mask");
        mask->setAttribute("id", mask_id);
        mask->setAttribute("maskUnits", "userSpaceOnUse");
        mask->setAttribute("maskContentUnits", "userSpaceOnUse");
        auto coverage = *original_bounds;
        coverage.expandBy(1);
        rectangle(mask, coverage);
        auto white = add(doc, mask, "svg:rect");
        rectangle(white, coverage);
        white->setAttribute("fill", "white");
        auto hole = add(doc, mask, "svg:path");
        hole->setAttribute("d", _path);
        hole->setAttribute("fill", "black");
        hole->setAttribute("fill-rule", "nonzero");
        preserved->setAttribute("mask", "url(#" + mask_id + ")");
    } else {
        preserved->setAttribute("clip-path", "url(#" + clip_id + ")");
    }

    auto original_repr = original->getRepr();
    Inkscape::GC::anchor(original_repr);
    parent->removeChild(original_repr);
    original_repr->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(original_transform));
    preserved->appendChild(original_repr);
    Inkscape::GC::release(original_repr);

    if (recolor) {
        auto inside = add(doc, wrapper, "svg:g");
        inside->setAttribute("clip-path", "url(#" + clip_id + ")");
        auto colored = add(doc, inside, "svg:g");
        std::string filter_id = std::string("ai-region-color-") + uuid;
        auto filter = add(doc, defs, "svg:filter");
        filter->setAttribute("id", filter_id);
        filter->setAttribute("filterUnits", "userSpaceOnUse");
        rectangle(filter, _bounds);
        filter->setAttribute("color-interpolation-filters", "sRGB");
        auto transfer = add(doc, filter, "svg:feComponentTransfer");
        transfer->setAttribute("style", "color-interpolation-filters:sRGB");
        char const *channels[] = {"svg:feFuncR", "svg:feFuncG", "svg:feFuncB"};
        for (unsigned c = 0; c < 3; ++c) {
            double value = std::stoi(dark_color.substr(1 + 2 * c, 2), nullptr, 16) / 255.0;
            auto channel = add(doc, transfer, channels[c]);
            channel->setAttribute("type", "linear");
            channel->setAttributeSvgDouble("slope", 1 - value);
            channel->setAttributeSvgDouble("intercept", value);
        }
        colored->setAttribute("filter", "url(#" + filter_id + ")");
        // Copy the exact source geometry, not a model's reconstruction. The
        // assistant rejects referenced source objects before creating an edit;
        // shared definitions keep their original IDs outside this subtree.
        auto copy = original_repr->duplicate(doc.getReprDoc());
        std::function<void(Node *)> strip_ids = [&](Node *node) {
            node->removeAttribute("id");
            for (auto child = node->firstChild(); child; child = child->next()) strip_ids(child);
        };
        strip_ids(copy);
        colored->appendChild(copy);
        Inkscape::GC::release(copy);
    }

    if (patch) {
        auto inside = add(doc, wrapper, "svg:g");
        inside->setAttribute("clip-path", "url(#" + clip_id + ")");
        // Flatten the SVG viewport through the normal importer. Nested SVG
        // viewports are not rendered consistently by the editor.
        auto replacement = patch->copy();
        auto viewport = replacement->getReprRoot();
        viewport->setAttributeSvgDouble("width", _bounds.width());
        viewport->setAttributeSvgDouble("height", _bounds.height());
        viewport->setAttribute("preserveAspectRatio", "none");
        replacement->ensureUpToDate();
        doc.import(*replacement, inside, nullptr, Geom::Translate(_bounds.min()), nullptr,
                   SPDocument::ImportRoot::AlwaysGroup);
    }
    if (!_detached) outline->deleteObject(true);
    doc.ensureUpToDate();
    auto composed = dynamic_cast<SPItem *>(doc.getObjectByRepr(wrapper));
    if (!composed) throw std::runtime_error("Could not construct the region edit.");
    return composed;
}

std::string AIRegion::reference_png(SPDocument &doc, bool full_artwork) const
{
    auto reference = doc.copy();
    auto item = full_artwork ? dynamic_cast<SPItem *>(reference->getObjectById(_target_id.c_str()))
                             : compose(*reference, nullptr);
    if (!item) throw std::runtime_error("The original artwork is no longer available.");
    auto area = full_artwork ? item->documentVisualBounds() : Geom::OptRect(_bounds);
    check_bounds(area);
    // Small vector details such as eyelids need enough pixels for the vision
    // model to distinguish outlines from the white interior of the eye.
    double dpi = 96 * std::min(full_artwork ? 8.0 : 64.0,
                              (full_artwork ? 1536.0 : 1024.0) / std::max(area->width(), area->height()));
    auto pixels = std::unique_ptr<Inkscape::Pixbuf>(
        sp_generate_internal_bitmap(reference.get(), *area, dpi, {item}));
    if (!pixels) throw std::runtime_error("Could not render the selected region.");
    g_autofree char *png = nullptr;
    g_autoptr(GError) error = nullptr;
    gsize size = 0;
    if (!gdk_pixbuf_save_to_buffer(pixels->getPixbufRaw(), &png, &size, "png", &error, nullptr)) {
        throw std::runtime_error("Could not encode the region reference image.");
    }
    if (size > 8 * 1024 * 1024) throw std::runtime_error("The region reference image is too large.");
    g_autofree char *encoded = g_base64_encode(reinterpret_cast<guchar const *>(png), size);
    return std::string("data:image/png;base64,") + encoded;
}
}
