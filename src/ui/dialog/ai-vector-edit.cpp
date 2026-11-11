// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai-vector-edit.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "document.h"
#include "display/cairo-utils.h"
#include "helper/pixbuf-ops.h"
#include "object/sp-path.h"
#include "object/sp-defs.h"
#include "object/sp-item-group.h"
#include "object/sp-root.h"
#include "svg/svg.h"
#include "xml/document.h"
#include "xml/attribute-record.h"
#include "xml/node.h"
#include "xml/repr.h"
namespace Inkscape::UI::Dialog {
namespace {
using Json = nlohmann::json;
using Node = Inkscape::XML::Node;
Node *add(SPDocument &doc, Node *parent, char const *tag) {
    auto node = doc.getReprDoc()->createElement(tag);
    parent->appendChild(node);
    Inkscape::GC::release(node);
    return node;
}
Json rect_json(Geom::Rect const &r) { return {r.left(), r.top(), r.width(), r.height()}; }
bool contained(Geom::Rect const &a, Geom::Rect const &b) {
    return a.left() <= b.left() && a.top() <= b.top() && a.right() >= b.right() && a.bottom() >= b.bottom();
}
bool hex_color(std::string const &c) {
    return c.size() == 7 && c[0] == '#' && std::all_of(c.begin()+1, c.end(), [](unsigned char v) { return std::isxdigit(v); });
}
void keys(Json const &object, std::set<std::string> const &allowed) {
    if (!object.is_object()) throw std::runtime_error("Expected a geometry command object.");
    for (auto const &[key, value] : object.items()) if (!allowed.contains(key))
        throw std::runtime_error("Unknown geometry command field: " + key);
}
double number(Json const &j, char const *key, double low, double high) {
    if (!j.contains(key) || !j.at(key).is_number()) throw std::runtime_error(std::string("Missing numeric field: ") + key);
    double v = j.at(key).get<double>();
    if (!std::isfinite(v) || v < low || v > high) throw std::runtime_error(std::string("Geometry value is out of range: ") + key);
    return v;
}
}
AIVectorEdit::AIVectorEdit(SPDocument &doc, SPItem *scope, Geom::OptRect hint, std::string hint_path) : _hint_path(std::move(hint_path)), _hint(hint) {
    doc.ensureUpToDate();
    if (!scope || scope->document != &doc || !scope->getId()) throw std::runtime_error("Select an object or group first.");
    auto bounds = scope->documentVisualBounds();
    if (!bounds || !std::isfinite(bounds->width()) || !std::isfinite(bounds->height()) ||
        bounds->width() > 1e7 || bounds->height() > 1e7 || bounds->width() <= 0 || bounds->height() <= 0) throw std::runtime_error("The selected artwork has no visible area.");
    _scope_id = scope->getId();
    _bounds = *bounds;
    std::function<void(SPItem *)> visit = [&](SPItem *item) {
        if (item->isHidden() || item->isLocked()) return;
        if (auto lpe = dynamic_cast<SPLPEItem *>(item); lpe && lpe->hasPathEffect()) return;
        if (auto group = dynamic_cast<SPGroup *>(item)) {
            for (auto &child : group->children) if (auto leaf = dynamic_cast<SPItem *>(&child)) visit(leaf);
            return;
        }
        if (!item->getId() || item->isReferenced()) return;
        if (auto lpe = dynamic_cast<SPLPEItem *>(item); lpe && lpe->hasPathEffect()) return;
        auto transform = item->i2doc_affine();
        if (transform.isSingular()) return;
        auto append = [&](int subpath, Geom::PathVector geometry, Geom::Rect const &b) {
            if (_candidates.size() >= 256) throw std::runtime_error("More than 256 geometry candidates. Select a smaller object or group.");
            Candidate candidate;
            candidate.id = "p" + std::to_string(_candidates.size());
            candidate.object_id = item->getId();
            candidate.subpath = subpath;
            candidate.has_fill = !item->style->fill.isNone();
            candidate.has_stroke = !item->style->stroke.isNone();
            candidate.geometry = std::move(geometry);
            candidate.bounds = b;
            candidate.local_to_doc = transform;
            if (auto d = item->getRepr()->attribute("d")) candidate.source_path = d;
            _candidates.push_back(std::move(candidate));
        };
        if (auto path = dynamic_cast<SPPath *>(item); path && path->curve()) {
            for (std::size_t i = 0; i < path->curve()->size(); ++i) {
                Geom::PathVector geometry;
                geometry.push_back((*path->curve())[i]);
                geometry *= transform;
                if (auto b = geometry.boundsExact()) append(i, std::move(geometry), *b);
            }
        } else if (auto b = item->documentVisualBounds()) {
            append(-1, {}, *b);
        }
    };
    visit(scope);
    if (_candidates.empty()) throw std::runtime_error("No editable geometry found. Locked, hidden, referenced objects and live path effects are excluded.");
}
std::string AIVectorEdit::catalog() const {
    Json result = {{"scope_bounds", rect_json(_bounds)}, {"candidates", Json::array()}};
    if (_hint) result["lasso_hint_bounds"] = rect_json(*_hint);
    for (auto const &c : _candidates) {
        result["candidates"].push_back({{"id", c.id}, {"object", c.object_id}, {"subpath", c.subpath},
            {"bounds", rect_json(c.bounds)}, {"has_fill", c.has_fill}, {"has_stroke", c.has_stroke}, {"overlaps_hint", _hint && _hint->intersects(c.bounds)}});
    }
    return result.dump();
}
AIVectorEdit::Plan AIVectorEdit::parse(std::string const &response) const try {
    if (response.size() > 32000) throw std::runtime_error("Geometry command is too large.");
    auto first = response.find_first_not_of(" \t\r\n");
    auto last = response.find_last_not_of(" \t\r\n");
    auto input = first == std::string::npos ? std::string{} : response.substr(first, last - first + 1);
    if ((input.starts_with("```json\n") || input.starts_with("```\n")) && input.ends_with("```")) {
        auto newline = input.find('\n');
        input = input.substr(newline + 1, input.size() - newline - 4);
    }
    auto j = Json::parse(input, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error("The model did not return a valid geometry command. Try again; no artwork was changed.");
    if (j.is_object() && j.contains("clarification")) {
        keys(j, {"clarification"});
        auto message = j.at("clarification").get<std::string>();
        if (message.empty() || message.size() > 500) throw std::runtime_error("Invalid clarification response.");
        Plan plan;
        plan.clarification = std::move(message);
        return plan;
    }
    if (j.is_object() && j.contains("edits")) {
        keys(j, {"edits", "reason"});
        auto const &edits = j.at("edits");
        if (!edits.is_array() || edits.empty() || edits.size() > 32)
            throw std::runtime_error("Use between 1 and 32 recolor edits.");
        Plan combined;
        combined.type = "recolor";
        combined.reason = j.value("reason", std::string{});
        if (combined.reason.size() > 500) throw std::runtime_error("Selection explanation is too long.");
        for (auto const &entry : edits) {
            keys(entry, {"targets", "operation", "reason"});
            auto part = parse(entry.dump());
            if (part.type != "recolor") throw std::runtime_error("Multiple operations currently support recoloring only.");
            if (combined.targets.empty()) combined.paint = part.paint;
            if (combined.paint != part.paint) throw std::runtime_error("Use the same paint channel for a multi-color proposal.");
            for (auto index : part.targets) {
                if (!combined.colors.emplace(index, part.color).second)
                    throw std::runtime_error("The AI assigned more than one color to the same target.");
                combined.targets.push_back(index);
            }
        }
        return combined;
    }
    keys(j, {"targets", "operation", "reason"});
    auto const &targets = j.at("targets");
    if (!targets.is_array() || targets.empty() || targets.size() > _candidates.size()) throw std::runtime_error("Select at least one known geometry candidate.");
    Plan plan;
    std::set<std::size_t> seen;
    for (auto const &id : targets) {
        if (!id.is_string()) throw std::runtime_error("Target IDs must be strings.");
        auto found = std::find_if(_candidates.begin(), _candidates.end(), [&](auto const &c) { return c.id == id.get<std::string>(); });
        if (found == _candidates.end()) throw std::runtime_error("The AI selected an unknown target.");
        auto index = std::size_t(found - _candidates.begin());
        if (!seen.insert(index).second) throw std::runtime_error("Duplicate geometry target.");
        plan.targets.push_back(index);
    }
    plan.reason = j.value("reason", std::string{});
    if (plan.reason.size() > 500) throw std::runtime_error("Selection explanation is too long.");
    auto const &op = j.at("operation");
    plan.type = op.at("type").get<std::string>();
    if (plan.type == "scale") {
        keys(op, {"type", "x", "y"});
        plan.x = number(op, "x", .1, 5); plan.y = number(op, "y", .1, 5);
    } else if (plan.type == "move") {
        keys(op, {"type", "x", "y"});
        plan.x = number(op, "x", -2 * _bounds.width(), 2 * _bounds.width());
        plan.y = number(op, "y", -2 * _bounds.height(), 2 * _bounds.height());
    } else if (plan.type == "rotate") {
        keys(op, {"type", "degrees"});
        plan.angle = number(op, "degrees", -180, 180);
    } else if (plan.type == "recolor") {
        keys(op, {"type", "color", "paint"});
        plan.color = op.at("color").get<std::string>();
        plan.paint = op.value("paint", std::string("auto"));
        if (!hex_color(plan.color) || (plan.paint != "fill" && plan.paint != "stroke" && plan.paint != "auto")) throw std::runtime_error("Recolor needs a hex color and paint set to fill, stroke or auto.");
    } else throw std::runtime_error("Unsupported geometry operation. Use scale, move, rotate or recolor.");
    return plan;
} catch (Json::exception const &) {
    throw std::runtime_error("The AI edit has a missing or incorrectly typed field. Recolor needs targets and a hex color; paint is optional. No artwork was changed.");
}
void AIVectorEdit::apply(SPDocument &doc, Plan const &plan) const {
    doc.ensureUpToDate();
    if (!plan.clarification.empty()) throw std::runtime_error("Answer the clarification before applying an edit.");
    if (plan.targets.empty()) throw std::runtime_error("No geometry targets.");
    Geom::OptRect selected_bounds;
    std::map<std::string, std::vector<std::size_t>> by_object;
    // Validate all targets before making any changes.
    for (auto index : plan.targets) {
        if (index >= _candidates.size()) throw std::runtime_error("Invalid target index.");
        auto const &c = _candidates[index];
        auto item = dynamic_cast<SPItem *>(doc.getObjectById(c.object_id.c_str()));
        if (!item || item->isLocked() || item->isHidden() || item->i2doc_affine() != c.local_to_doc)
            throw std::runtime_error("The selected geometry changed. Generate a new proposal.");
        if (c.subpath >= 0) {
            auto path = dynamic_cast<SPPath *>(item);
            if (!path || !path->curve() || std::size_t(c.subpath) >= path->curve()->size() ||
                std::string(item->getRepr()->attribute("d") ? item->getRepr()->attribute("d") : "") != c.source_path)
                throw std::runtime_error("The path changed. Generate a new proposal.");
        }
        selected_bounds.unionWith(c.bounds);
        by_object[c.object_id].push_back(index);
    }
    auto center = selected_bounds->midpoint();
    Geom::Affine edit;
    if (plan.type == "scale") edit = Geom::Translate(-center) * Geom::Scale(plan.x, plan.y) * Geom::Translate(center);
    else if (plan.type == "move") edit = Geom::Translate(plan.x, plan.y);
    else if (plan.type == "rotate") edit = Geom::Translate(-center) * Geom::Rotate(plan.angle * std::acos(-1.0) / 180) * Geom::Translate(center);
    for (auto const &[id, indices] : by_object) {
        auto item = dynamic_cast<SPItem *>(doc.getObjectById(id.c_str()));
        auto const &first = _candidates[indices.front()];
        if (plan.type == "recolor") {
            // Recolor existing paint: traced outlines are usually fills, not SVG strokes.
            auto paint = plan.paint == "auto" ? (first.has_fill ? "fill" : "stroke") : plan.paint;
            if (paint == "stroke" && !first.has_stroke && first.has_fill) paint = "fill";
            auto color_for = [&](std::size_t index) -> std::string const & {
                return plan.colors.empty() ? plan.color : plan.colors.at(index);
            };
            auto total = std::count_if(_candidates.begin(), _candidates.end(),
                [&](auto const &c) { return c.object_id == id; });
            if (first.subpath >= 0 && (indices.size() < static_cast<std::size_t>(total) || !plan.colors.empty())) {
                // Partition the original fill, rather than filling extracted hole contours.
                // The group keeps effects/opacity/transforms and the original public ID.
                auto path = dynamic_cast<SPPath *>(item);
                auto source = item->getRepr();
                auto parent = source->parent();
                auto wrapper = doc.getReprDoc()->createElement("svg:g");
                for (auto const &attr : source->attributeList()) {
                    std::string key = g_quark_to_string(attr.key);
                    if (key != "id" && key != "d")
                        wrapper->setAttribute(key.c_str(), attr.value);
                }
                parent->addChild(wrapper, source);
                Inkscape::GC::release(wrapper);
                if (paint == "stroke") {
                    // Keep the complete fill (including holes), partition only the strokes.
                    auto fill = add(doc, wrapper, "svg:path");
                    fill->setAttribute("d", first.source_path);
                    fill->setAttribute("style", "stroke:none");
                    std::set<int> selected;
                    for (auto index : indices) selected.insert(_candidates[index].subpath);
                    Geom::PathVector other;
                    for (std::size_t k = 0; k < path->curve()->size(); ++k)
                        if (!selected.contains(k)) other.push_back((*path->curve())[k]);
                    auto preserved = add(doc, wrapper, "svg:path");
                    preserved->setAttribute("d", sp_svg_write_path(other));
                    preserved->setAttribute("style", "fill:none");
                    std::map<std::string, Geom::PathVector> strokes;
                    for (auto index : indices)
                        strokes[color_for(index)].push_back((*path->curve())[_candidates[index].subpath]);
                    for (auto const &[color, geometry] : strokes) {
                        auto colored = add(doc, wrapper, "svg:path");
                        colored->setAttribute("d", sp_svg_write_path(geometry));
                        colored->setAttribute("style", "fill:none;stroke:" + color);
                    }
                    parent->removeChild(source);
                    wrapper->setAttribute("id", id);
                    continue;
                }
                g_autofree char *uuid = g_uuid_string_random();
                std::string clip_id = std::string("ai-contour-clip-") + uuid;
                std::string mask_id = std::string("ai-contour-mask-") + uuid;
                auto defs = doc.getDefs()->getRepr();
                std::map<std::string, std::pair<Node *, std::string>> clips;
                auto mask = add(doc, defs, "svg:mask");
                mask->setAttribute("id", mask_id);
                mask->setAttribute("maskUnits", "userSpaceOnUse");
                mask->setAttribute("maskContentUnits", "userSpaceOnUse");
                auto bounds = path->curve()->boundsExact();
                bounds->expandBy(1);
                for (auto node : {mask, add(doc, mask, "svg:rect")}) {
                    node->setAttributeSvgDouble("x", bounds->left());
                    node->setAttributeSvgDouble("y", bounds->top());
                    node->setAttributeSvgDouble("width", bounds->width());
                    node->setAttributeSvgDouble("height", bounds->height());
                    if (node != mask) node->setAttribute("fill", "white");
                }
                for (auto index : indices) {
                    Geom::PathVector contour;
                    contour.push_back((*path->curve())[_candidates[index].subpath]);
                    auto const &color = color_for(index);
                    if (!clips.contains(color)) {
                        auto clip = add(doc, defs, "svg:clipPath");
                        auto name = clip_id + "-" + std::to_string(clips.size());
                        clip->setAttribute("id", name);
                        clip->setAttribute("clipPathUnits", "userSpaceOnUse");
                        clips.emplace(color, std::make_pair(clip, name));
                    }
                    for (auto container : {clips.at(color).first, mask}) {
                        auto shape = add(doc, container, "svg:path");
                        shape->setAttribute("d", sp_svg_write_path(contour));
                        shape->setAttribute("fill", "black");
                    }
                }
                auto preserved = add(doc, wrapper, "svg:path");
                preserved->setAttribute("d", first.source_path);
                preserved->setAttribute("style", "stroke:none");
                preserved->setAttribute("mask", "url(#" + mask_id + ")");
                // Paint enclosing contours first so a separately selected inner feature
                // (for example a pupil) retains its own requested color.
                std::map<std::string, double> areas;
                for (auto index : indices) {
                    auto const &b = _candidates[index].bounds;
                    areas[color_for(index)] = std::max(areas[color_for(index)], b.width() * b.height());
                }
                std::vector<std::string> order;
                for (auto const &[color, clip] : clips) order.push_back(color);
                std::stable_sort(order.begin(), order.end(), [&](auto const &a, auto const &b) { return areas[a] > areas[b]; });
                for (auto const &color : order) {
                    auto const &clip = clips.at(color);
                    auto colored = add(doc, wrapper, "svg:path");
                    colored->setAttribute("d", first.source_path);
                    colored->setAttribute("style", "fill:" + color + ";stroke:none");
                    colored->setAttribute("clip-path", "url(#" + clip.second + ")");
                }
                auto stroke = add(doc, wrapper, "svg:path");
                stroke->setAttribute("d", first.source_path);
                stroke->setAttribute("style", "fill:none");
                // Reacquire by ID after applying: the path has become an editable group.
                parent->removeChild(source);
                wrapper->setAttribute("id", id);
                continue;
            }
            auto css = sp_repr_css_attr_new();
            sp_repr_css_set_property(css, paint.c_str(), color_for(indices.front()).c_str());
            sp_repr_css_change(item->getRepr(), css, "style");
            sp_repr_css_attr_unref(css);
        } else if (first.subpath >= 0) {
            auto path = dynamic_cast<SPPath *>(item);
            auto geometry = *path->curve();
            auto local_edit = first.local_to_doc * edit * first.local_to_doc.inverse();
            for (auto index : indices) geometry[_candidates[index].subpath] *= local_edit;
            item->getRepr()->setAttribute("d", sp_svg_write_path(geometry));
        } else {
            auto local_edit = first.local_to_doc * edit * first.local_to_doc.inverse();
            item->doWriteTransform(local_edit * item->transform);
        }
    }
    doc.ensureUpToDate();
}
std::unique_ptr<SPDocument> AIVectorEdit::marked_document(SPDocument &doc, std::vector<std::size_t> const &targets, bool numbered) const {
    auto copy = doc.copy();
    auto scope = copy->getObjectById(_scope_id.c_str());
    if (!scope) throw std::runtime_error("Original artwork is no longer available.");
    copy->getRoot()->cropToObjects({scope});
    copy->ensureUpToDate();
    auto overlay = add(*copy, copy->getReprRoot(), "svg:g");
    overlay->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(copy->getRoot()->i2doc_affine().inverse()));
    double unit = std::max(_bounds.width(), _bounds.height()) / 160;
    if (numbered && !_hint_path.empty()) {
        auto hint = add(*copy, overlay, "svg:path");
        hint->setAttribute("d", _hint_path);
        hint->setAttribute("fill", "none");
        hint->setAttribute("stroke", "#00aacc");
        hint->setAttributeSvgDouble("stroke-width", unit * .4);
        hint->setAttribute("stroke-dasharray", std::to_string(unit) + "," + std::to_string(unit));
    }
    for (auto index : targets) {
        auto const &c = _candidates.at(index);
        auto mark = add(*copy, overlay, c.subpath >= 0 ? "svg:path" : "svg:rect");
        if (c.subpath >= 0) mark->setAttribute("d", sp_svg_write_path(c.geometry));
        else {
            mark->setAttributeSvgDouble("x", c.bounds.left()); mark->setAttributeSvgDouble("y", c.bounds.top());
            mark->setAttributeSvgDouble("width", c.bounds.width()); mark->setAttributeSvgDouble("height", c.bounds.height());
        }
        mark->setAttribute("fill", "none"); mark->setAttribute("stroke", "#f000aa");
        mark->setAttributeSvgDouble("stroke-width", unit * .35);
        if (numbered) {
            auto label = add(*copy, overlay, "svg:text");
            label->setAttributeSvgDouble("x", c.bounds.midpoint().x());
            label->setAttributeSvgDouble("y", c.bounds.midpoint().y());
            label->setAttributeSvgDouble("font-size", unit * 1.5);
            label->setAttribute("fill", "#000000"); label->setAttribute("stroke", "white");
            label->setAttribute("paint-order", "stroke fill"); label->setAttributeSvgDouble("stroke-width", unit * .35);
            auto text = copy->getReprDoc()->createTextNode(c.id.c_str());
            label->appendChild(text); Inkscape::GC::release(text);
        }
    }
    copy->ensureUpToDate();
    return copy;
}
std::string AIVectorEdit::reference_png(SPDocument &doc, bool numbered, bool closeup) const {
    std::vector<std::size_t> all;
    if (numbered) for (std::size_t i = 0; i < _candidates.size(); ++i) all.push_back(i);
    auto reference = marked_document(doc, all, numbered);
    auto area = closeup && _hint ? *_hint : _bounds; area.expandBy(std::max(area.width(), area.height()) * (closeup ? .75 : .03));
    double dpi = 96 * std::min(closeup ? 32.0 : 8.0, 1800 / std::max(area.width(), area.height()));
    std::unique_ptr<Inkscape::Pixbuf> pixels(sp_generate_internal_bitmap(reference.get(), area, dpi));
    if (!pixels) throw std::runtime_error("Could not render geometry candidates.");
    auto source = pixels->getPixbufRaw();
    g_autoptr(GdkPixbuf) opaque = gdk_pixbuf_composite_color_simple(source,
        gdk_pixbuf_get_width(source), gdk_pixbuf_get_height(source), GDK_INTERP_NEAREST,
        255, 16, 0xffffff, 0xffffff);
    if (!opaque) throw std::runtime_error("Could not prepare the artwork reference.");
    g_autofree char *png = nullptr; gsize length = 0;
    if (!gdk_pixbuf_save_to_buffer(opaque, &png, &length, "png", nullptr, nullptr) || length > 8 * 1024 * 1024)
        throw std::runtime_error("Could not encode geometry candidates within the image limit.");
    g_autofree char *encoded = g_base64_encode(reinterpret_cast<guchar *>(png), length);
    return std::string("data:image/png;base64,") + encoded;
}
std::unique_ptr<SPDocument> AIVectorEdit::selection_preview(SPDocument &doc, Plan const &plan) const {
    auto result = marked_document(doc, plan.targets, true);
    fit_preview(*result, plan);
    return result;
}
void AIVectorEdit::fit_preview(SPDocument &doc, Plan const &plan) const {
    Geom::OptRect bounds;
    for (auto index : plan.targets) bounds.unionWith(_candidates.at(index).bounds);
    if (!bounds) throw std::runtime_error("No targets to preview.");
    auto center = bounds->midpoint();
    Geom::Affine edit;
    if (plan.type == "scale") edit = Geom::Translate(-center) * Geom::Scale(plan.x, plan.y) * Geom::Translate(center);
    else if (plan.type == "move") edit = Geom::Translate(plan.x, plan.y);
    else if (plan.type == "rotate") edit = Geom::Translate(-center) * Geom::Rotate(plan.angle * std::acos(-1.0) / 180) * Geom::Translate(center);
    auto changed = *bounds * edit;
    bounds.unionWith(changed);
    bounds->expandBy(std::max(2.0, std::max(bounds->width(), bounds->height()) * .2));
    doc.ensureUpToDate();
    doc.fitToRect(*bounds * doc.doc2dt());
    doc.ensureUpToDate();
}
std::string AIVectorEdit::summary(Plan const &plan) const {
    unsigned outside = 0;
    for (auto index : plan.targets) if (_hint && !contained(*_hint, _candidates[index].bounds)) ++outside;
    unsigned corrected = 0;
    for (auto index : plan.targets) {
        auto const &c = _candidates[index];
        if (plan.type == "recolor" && plan.paint == "stroke" && !c.has_stroke && c.has_fill) ++corrected;
    }
    return "AI selected " + std::to_string(plan.targets.size()) + " parts" +
        (_hint ? "; " + std::to_string(outside) + " extend beyond the lasso bounds" : "") +
        ". Operation: " + plan.type + ". " +
        (corrected ? "Used existing fill for " + std::to_string(corrected) + " filled outlines without strokes. " : "") + plan.reason;
}
}
