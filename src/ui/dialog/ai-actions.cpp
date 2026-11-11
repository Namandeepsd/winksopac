// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai-actions.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <memory>
#include <regex>
#include <stdexcept>
#include "document.h"
#include "page-manager.h"
#include "object/object-set.h"
#include "object/sp-root.h"
#include "object/sp-path.h"
#include "display/cairo-utils.h"
#include "helper/pixbuf-ops.h"
#include "svg/svg.h"
#include "xml/node.h"
#include "xml/repr.h"
namespace Inkscape::UI::Dialog {
namespace {
using Json = nlohmann::json;
void keys(Json const &j, std::set<std::string> const &allowed) {
    if (!j.is_object()) throw std::runtime_error("Expected a command object.");
    for (auto const &[k,v] : j.items()) if (!allowed.contains(k)) throw std::runtime_error("Unsupported command field: " + k);
}
double number(Json const &j, char const *key, double min, double max) {
    if (!j.contains(key) || !j.at(key).is_number()) throw std::runtime_error(std::string("Missing number: ") + key);
    double v = j.at(key);
    if (!std::isfinite(v) || v < min || v > max) throw std::runtime_error(std::string("Value out of range: ") + key);
    return v;
}
bool color(std::string const &s) {
    return s == "none" || (s.size() == 7 && s[0] == '#' &&
        std::all_of(s.begin()+1,s.end(),[](unsigned char c){ return std::isxdigit(c); }));
}
}
AIActions::AIActions(SPDocument &doc, std::vector<SPItem *> const &selection) {
    doc.ensureUpToDate();
    _page = doc.getPageManager().getSelectedPageIndex();
    auto page = doc.getPageManager().getSelectedPageRect();
    _catalog = {{"page_index",_page},{"page_bounds",{page.left(),page.top(),page.width(),page.height()}},{"objects",Json::array()}};
    std::size_t path_bytes = 0;
    std::function<void(SPObject *)> visit = [&](SPObject *object) {
        for (auto &child : object->children) {
            if (auto item = dynamic_cast<SPItem *>(&child)) {
                if (_ids.size() >= 1000) throw std::runtime_error("Document exceeds 1000 objects. Use geometry editing on a smaller selection.");
                if (item->getId()) {
                    _ids.insert(item->getId());
                    Json entry = {{"id",item->getId()},{"type",item->getRepr()->name()},
                        {"locked",item->isLocked()},{"hidden",item->isHidden()},{"referenced",item->isReferenced()}};
                    if (auto label = item->getRepr()->attribute("inkscape:label")) entry["label"] = label;
                    if (auto b = item->desktopVisualBounds()) entry["bounds"] = {b->left(),b->top(),b->width(),b->height()};
                    if (auto style = item->getRepr()->attribute("style"); style && std::char_traits<char>::length(style) <= 1024) entry["style"] = style;
                    if (auto path = dynamic_cast<SPPath *>(item); path && !path->hasPathEffect()) {
                        if (auto d = item->getRepr()->attribute("d")) {
                            auto size = std::char_traits<char>::length(d);
                            if (size <= 8192 && path_bytes + size <= 128*1024) {
                                entry["d"] = d;
                                path_bytes += size;
                                _path_ids.insert(item->getId());
                            }
                        }
                    }
                    entry["has_fill"] = !item->style->fill.isNone();
                    entry["has_stroke"] = !item->style->stroke.isNone();
                    _catalog["objects"].push_back(entry);
                }
                visit(item);
            }
        }
    };
    visit(doc.getRoot());
    for (auto item : selection) if (item->getId() && _ids.contains(item->getId())) _selection.emplace_back(item->getId());
    _catalog["selection"] = _selection;
}
std::string AIActions::catalog() const { return _catalog.dump(); }
std::optional<std::string> AIActions::page_fit_request(std::string const &request) {
    if (request.size() > 180) return {};
    std::string text;
    for (unsigned char c : request) {
        if (std::isspace(c)) {
            if (!text.empty() && text.back() != ' ') text += ' ';
        } else {
            text += static_cast<char>(std::tolower(c));
        }
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '.' || text.back() == '!' || text.back() == '?')) text.pop_back();
    static std::regex const pattern(
        R"(^(?:(?:can you|could you) )?(?:please )?(?:fit|resize|crop) (?:the )?page(?: (?:size|dimensions))? (?:to|around) (?:the )?(?:(?:whole|entire|full) )?(drawing|artwork|art|scenery|scene|content|selection|selected objects)(?: please)?$)");
    std::smatch match;
    if (std::regex_match(text, match, pattern))
        return match[1] == "selection" || match[1] == "selected objects" ? "selection" : "drawing";
    if (text == "fit page" || text == "fit the page" || text == "fit to page") return "drawing";
    return {};
}
void AIActions::fit_page(SPDocument &doc, std::vector<SPItem *> const &selection, bool drawing) {
    doc.ensureUpToDate();
    auto &pages = doc.getPageManager();
    if (drawing) {
        auto bounds = doc.getRoot()->documentVisualBounds();
        if (!bounds) throw std::runtime_error("There is no artwork on the canvas. Insert the generated artwork before fitting the page.");
        pages.fitToRect(bounds, pages.getSelected(), false);
    } else {
        if (selection.empty()) throw std::runtime_error("Select the objects to fit the page around.");
        Inkscape::ObjectSet objects(&doc);
        objects.setList(selection);
        if (!objects.documentPreferredBounds()) throw std::runtime_error("The selection has no visible bounds.");
        pages.fitToSelection(&objects, false);
    }
    doc.ensureUpToDate();
}
std::string AIActions::instructions() {
    return R"AI(You operate Inkscape using its built-in document operations. Inspect the supplied object catalog, current selection, page, and artwork image.
Return ONLY {"commands":[...],"reason":"Describe the result and any assumptions"}.
Commands:
{"tool":"fit_page","scope":"drawing"} fits the selected page to ALL drawing content, ignoring selection.
{"tool":"fit_page","scope":"selection"} fits to the captured selection.
{"tool":"fit_page","scope":"objects","targets":["existing-id"]} fits to specified objects.
{"tool":"resize_page","width":800,"height":600} sets page size in document pixels.
Object commands use "targets":["existing-id",...] or "targets":"selection":
{"tool":"move","targets":"selection","x":10,"y":0}
{"tool":"scale","targets":"selection","x":1.2,"y":1.2}
{"tool":"rotate","targets":"selection","degrees":15}
{"tool":"group","targets":["id1","id2"]}; {"tool":"ungroup","targets":"selection"}
{"tool":"raise","targets":"selection"}; {"tool":"lower","targets":"selection"} move one stacking step.
{"tool":"align","targets":["id1","id2"],"axis":"x","edge":"center"} aligns within combined bounds. axis x or y; edge min, center or max.
{"tool":"set_attributes","targets":"selection","attributes":{"fill":"#FF69B4","stroke":"none","opacity":1,"stroke-width":2}}
Only fill/stroke (#RRGGBB or none), opacity (0..1), stroke-width (0..10000) are allowed. These affect the named objects; target leaf objects to override their own paint.
{"tool":"set_path","targets":["path-id"],"d":"M0,0..."} edits an existing path's local geometry; use only when ordinary commands cannot express the edit. Use set_path only when the catalog includes the original d attribute. Preserve unaffected contours. Never invent path coordinates for a feature you cannot locate.
Move and alignment coordinates are desktop pixels as in the catalog. Scale and rotation use the combined center.
After group/ungroup, targets:"selection" refers to the resulting objects. Other IDs must come from the catalog.
Use fit_page scope drawing for "fit page to scenery/artwork" unless a particular subset is explicitly requested; a small selected object is not necessarily the scenery.
At most 16 commands. Prefer native commands over XML attribute/path changes. Do not output SVG or invent unsupported tools, files, shell commands, or menu names.
For unsupported operations or genuinely unclear targets return {"clarification":"A concise question or limitation"}.
)AI";
}
AIActions::Plan AIActions::parse(std::string const &response) const try {
    if (response.size() > 128*1024) throw std::runtime_error("Command response is too large.");
    auto first = response.find_first_not_of(" \r\n\t"), last = response.find_last_not_of(" \r\n\t");
    auto text = first == std::string::npos ? std::string{} : response.substr(first,last-first+1);
    if (text.starts_with("```") && text.ends_with("```")) {
        auto line = text.find('\n');
        if (line != std::string::npos) text = text.substr(line+1,text.size()-line-4);
    }
    auto j = Json::parse(text);
    Plan plan;
    if (j.contains("clarification")) {
        keys(j,{"clarification"}); plan.clarification = j.at("clarification").get<std::string>();
        if (plan.clarification.empty() || plan.clarification.size()>500) throw std::runtime_error("Invalid clarification.");
        return plan;
    }
    keys(j,{"commands","reason"});
    plan.reason = j.value("reason",std::string{});
    plan.commands = j.at("commands");
    if (plan.reason.size()>500 || !plan.commands.is_array() || plan.commands.empty() || plan.commands.size()>16)
        throw std::runtime_error("Expected 1–16 commands and a short explanation.");
    for (auto const &c : plan.commands) {
        auto tool = c.at("tool").get<std::string>();
        if (tool=="fit_page") {
            keys(c,{"tool","scope","targets"});
            auto scope=c.at("scope").get<std::string>();
            if (scope!="drawing" && scope!="selection" && scope!="objects") throw std::runtime_error("Unknown page-fit scope.");
            if (scope!="objects") { if(c.contains("targets")) throw std::runtime_error("Use objects scope with targets."); continue; }
        } else if(tool=="resize_page") {
            keys(c,{"tool","width","height"}); number(c,"width",1,1e6); number(c,"height",1,1e6); continue;
        } else if(tool=="move" || tool=="scale") {
            keys(c,{"tool","targets","x","y"});
            number(c,"x",tool=="scale"?.01:-1e6,tool=="scale"?100:1e6);
            number(c,"y",tool=="scale"?.01:-1e6,tool=="scale"?100:1e6);
        } else if(tool=="rotate") {
            keys(c,{"tool","targets","degrees"}); number(c,"degrees",-360,360);
        } else if(tool=="group" || tool=="ungroup" || tool=="raise" || tool=="lower") keys(c,{"tool","targets"});
        else if(tool=="align") {
            keys(c,{"tool","targets","axis","edge"});
            auto axis=c.at("axis").get<std::string>(), edge=c.at("edge").get<std::string>();
            if ((axis!="x" && axis!="y") || (edge!="min" && edge!="center" && edge!="max")) throw std::runtime_error("Invalid alignment.");
        } else if(tool=="set_attributes") {
            keys(c,{"tool","targets","attributes"});
            auto const &attrs=c.at("attributes"); keys(attrs,{"fill","stroke","opacity","stroke-width"});
            if(attrs.empty()) throw std::runtime_error("No attributes supplied.");
            for(auto const &[key,value]:attrs.items()) {
                if(key=="fill" || key=="stroke") { if(!value.is_string() || !color(value.get<std::string>())) throw std::runtime_error("Use a hex color or none."); }
                else number(attrs,key.c_str(),0,key=="opacity"?1:10000);
            }
        } else if(tool=="set_path") {
            keys(c,{"tool","targets","d"});
            auto d=c.at("d").get<std::string>();
            if(d.size()>65536) throw std::runtime_error("Path edit is too large.");
            auto path=sp_svg_read_pathv(d.c_str()); auto b=path.boundsExact();
            if(!b || !std::isfinite(b->left()) || !std::isfinite(b->top()) || !std::isfinite(b->right()) || !std::isfinite(b->bottom()) ||
                std::max({std::abs(b->left()),std::abs(b->top()),std::abs(b->right()),std::abs(b->bottom())})>1e7)
                throw std::runtime_error("Invalid path geometry.");
        } else throw std::runtime_error("Unsupported Inkscape command: "+tool);
        auto const &targets=c.at("targets");
        if(targets.is_string() && targets=="selection") {
            if(tool=="set_path") throw std::runtime_error("Use explicit catalog path IDs for XML geometry edits.");
            continue;
        }
        if(!targets.is_array() || targets.empty() || targets.size()>1000) throw std::runtime_error("Expected object IDs or selection.");
        std::set<std::string> seen;
        for(auto const &id:targets) {
            auto value=id.get<std::string>();
            if(!_ids.contains(value) || !seen.insert(value).second) throw std::runtime_error("Unknown or duplicate object ID.");
            if(tool=="set_path" && !_path_ids.contains(value)) throw std::runtime_error("Path geometry was not available in the catalog. Use geometry editing instead.");
        }
    }
    return plan;
} catch (Json::exception const &) { throw std::runtime_error("The command response has missing or invalid fields."); }

std::vector<std::string> AIActions::apply(SPDocument &doc, Plan const &plan) const {
    if (!plan.clarification.empty() || plan.commands.empty()) throw std::runtime_error("No executable commands.");
    auto &pages=doc.getPageManager();
    if(_page>=0) pages.selectPage(_page);
    auto current=_selection;
    for(auto const &c:plan.commands) {
        doc.ensureUpToDate();
        std::string tool=c.at("tool");
        if(tool=="resize_page") { pages.resizePage(c.at("width"),c.at("height")); continue; }
        if(tool=="fit_page" && c.at("scope")=="drawing") {
            fit_page(doc, {}, true); continue;
        }
        std::vector<std::string> ids=current;
        if(c.contains("targets") && c.at("targets").is_array()) ids=c.at("targets").get<std::vector<std::string>>();
        std::vector<SPItem *> items;
        for(auto const &id:ids) {
            auto item=dynamic_cast<SPItem *>(doc.getObjectById(id.c_str()));
            if(!item || item->isLocked() || item->isHidden() || item->isReferenced())
                throw std::runtime_error("An object is missing, locked, hidden or referenced: "+id);
            items.push_back(item);
        }
        if(items.empty()) throw std::runtime_error("This command needs a selection or explicit object IDs.");
        for(auto a:items) for(auto b:items) if(a!=b && a->isAncestorOf(b)) throw std::runtime_error("Do not target both a group and its child.");
        Inkscape::ObjectSet set(&doc); set.setList(items);
        if(tool=="fit_page") { fit_page(doc, items, false); continue; }
        auto bounds=set.visualBounds();
        if(!bounds) throw std::runtime_error("The objects have no visible bounds.");
        if(tool=="move") set.moveRelative(Geom::Point(c.at("x"),c.at("y")));
        else if(tool=="scale") set.scaleRelative(bounds->midpoint(),Geom::Scale(c.at("x"),c.at("y")));
        else if(tool=="rotate") set.rotateRelative(bounds->midpoint(),c.at("degrees"));
        else if(tool=="group") {
            for(auto item:items) if(item->parent!=items.front()->parent) throw std::runtime_error("Group objects within the same parent.");
            set.group();
        } else if(tool=="ungroup") {
            for(auto item:items) if(!dynamic_cast<SPGroup *>(item)) throw std::runtime_error("Ungroup requires groups.");
            set.ungroup(true);
        } else if(tool=="raise") set.stackUp(true);
        else if(tool=="lower") set.stackDown(true);
        else if(tool=="align") {
            int axis=c.at("axis")=="x"?0:1;
            std::string edge=c.at("edge");
            auto coordinate=[&](Geom::Rect const &r){ return edge=="min"?r.min()[axis]:edge=="max"?r.max()[axis]:r.midpoint()[axis]; };
            for(auto item:items) {
                auto b=item->desktopVisualBounds(); if(!b) continue;
                Geom::Point delta(0,0); delta[axis]=coordinate(*bounds)-coordinate(*b);
                Inkscape::ObjectSet one(&doc); one.set(item); one.moveRelative(delta);
            }
        } else if(tool=="set_attributes") {
            auto css=sp_repr_css_attr_new();
            for(auto const &[key,value]:c.at("attributes").items())
                sp_repr_css_set_property(css,key.c_str(),value.is_string()?value.get<std::string>().c_str():value.dump().c_str());
            for(auto item:items) sp_repr_css_change(item->getRepr(),css,"style");
            sp_repr_css_attr_unref(css);
        } else if(tool=="set_path") {
            for(auto item:items) {
                auto path=dynamic_cast<SPPath *>(item);
                if(!path || path->hasPathEffect()) throw std::runtime_error("Path editing requires ordinary paths without live effects.");
                item->getRepr()->setAttribute("d",sp_svg_write_path(sp_svg_read_pathv(c.at("d").get<std::string>().c_str())));
            }
        }
        current.clear();
        for(auto item:set.items()) if(item->getId()) current.emplace_back(item->getId());
    }
    doc.ensureUpToDate();
    return current;
}
std::string AIActions::summary(Plan const &plan) const {
    return std::to_string(plan.commands.size())+" Inkscape command(s) ready. "+plan.reason;
}
std::string AIActions::reference_png(SPDocument &doc) const {
    auto area=doc.getRoot()->documentVisualBounds();
    if(!area) area=doc.getPageManager().getSelectedPageRect()*doc.dt2doc();
    if(!area || area->width()<=0 || area->height()<=0) throw std::runtime_error("Cannot render the document.");
    auto box=*area; box.expandBy(std::max(box.width(),box.height())*.03);
    std::unique_ptr<Inkscape::Pixbuf> pixels(sp_generate_internal_bitmap(&doc,box,96*std::min(8.0,1600/std::max(box.width(),box.height()))));
    if(!pixels) throw std::runtime_error("Cannot render the document.");
    auto raw=pixels->getPixbufRaw();
    g_autoptr(GdkPixbuf) opaque=gdk_pixbuf_composite_color_simple(raw,gdk_pixbuf_get_width(raw),gdk_pixbuf_get_height(raw),GDK_INTERP_NEAREST,255,16,0xffffff,0xffffff);
    g_autofree char *png=nullptr; gsize length=0;
    if(!opaque || !gdk_pixbuf_save_to_buffer(opaque,&png,&length,"png",nullptr,nullptr) || length>8*1024*1024) throw std::runtime_error("Cannot encode document reference.");
    g_autofree char *encoded=g_base64_encode(reinterpret_cast<guchar *>(png),length);
    return std::string("data:image/png;base64,")+encoded;
}
}
