// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone integration test: deliberately needs no GUI or GoogleTest.
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <giomm/init.h>
#include <glibmm/i18n.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "document.h"
#include "document-undo.h"
#include "page-manager.h"
#include "object/object-set.h"
#include "display/cairo-utils.h"
#include "helper/pixbuf-ops.h"
#include "inkgc/gc-core.h"
#include "inkscape.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "ui/dialog/ai-region.h"
#include "ui/dialog/ai-vector-edit.h"
#include "ui/dialog/ai-actions.h"
#include "svg/svg.h"
#include <nlohmann/json.hpp>
#include "util/statics.h"
#include "util/units.h"
#include "xml/repr.h"

using Inkscape::UI::Dialog::AIRegion;
using namespace std::string_literals;

static void require(bool condition, char const *message)
{
    if (!condition) throw std::runtime_error(message);
}

static std::unique_ptr<SPDocument> parse(std::string const &svg)
{
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(), svg.size()));
    require(bool(doc), "SVG fixture failed to parse");
    doc->ensureUpToDate();
    return doc;
}

static std::array<unsigned char, 4> pixel(Inkscape::Pixbuf &image, int x, int y)
{
    auto pb = image.getPixbufRaw();
    auto p = gdk_pixbuf_get_pixels(pb) + y * gdk_pixbuf_get_rowstride(pb) + x * gdk_pixbuf_get_n_channels(pb);
    return {p[0], p[1], p[2], gdk_pixbuf_get_has_alpha(pb) ? p[3] : static_cast<unsigned char>(255)};
}

static std::unique_ptr<Inkscape::Pixbuf> render(SPDocument &doc)
{
    auto image = std::unique_ptr<Inkscape::Pixbuf>(sp_generate_internal_bitmap(&doc, Geom::Rect(Geom::Point(0, 0), Geom::Point(200, 200)), 96));
    require(bool(image), "Rendering failed");
    return image;
}

static void check_case(std::string const &target, std::string const &transform, bool scaled_root, bool capture_boundary = false)
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' width='200' height='200' viewBox='0 0 "s +
        (scaled_root ? "100 100" : "200 200") + "'><g transform='" + transform + "'>" + target +
        "<path id='outline' fill='none' d='M20,20 L80,20 L50,80 Z'/></g></svg>");
    auto original_xml = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto item = dynamic_cast<SPItem *>(doc->getObjectById("target"));
    auto outline = dynamic_cast<SPPath *>(doc->getObjectById("outline"));
    auto affine = item->i2doc_affine();
    AIRegion region(item, outline);
    auto png = region.reference_png(*doc);
    require(png.starts_with("data:image/png;base64,"), "Missing region reference PNG");
    require(sp_repr_save_buf(doc->getReprDoc()).raw() == original_xml, "Reference changed the original document");
    gsize png_size = 0;
    g_autofree guchar *png_data = g_base64_decode(png.substr(22).c_str(), &png_size);
    g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new();
    require(gdk_pixbuf_loader_write(loader, png_data, png_size, nullptr) &&
            gdk_pixbuf_loader_close(loader, nullptr), "Invalid reference PNG");
    auto reference = gdk_pixbuf_loader_get_pixbuf(loader);
    require(gdk_pixbuf_get_has_alpha(reference), "Region reference lacks transparency");
    auto corner_pixel = gdk_pixbuf_get_pixels(reference) + (gdk_pixbuf_get_height(reference) - 1) * gdk_pixbuf_get_rowstride(reference);
    require(corner_pixel[3] == 0, "Reference includes pixels outside the loop");
    auto before = render(*doc);
    if (capture_boundary) {
        Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
        region.detach_outline(*doc);
        Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Capture region boundary"), "ai-assistant");
        require(region.detached() && !doc->getObjectById("outline"), "Captured outline remains in the drawing");
        require(region.reference_png(*doc) == png, "Removing the outline changed the captured reference");
        original_xml = sp_repr_save_buf(doc->getReprDoc()).raw();
    }
    auto proposal = parse("<svg xmlns='http://www.w3.org/2000/svg' width='60' height='60' viewBox='0 0 60 60'><rect id='target' x='-100' y='-100' width='300' height='300' fill='blue'/></svg>");
    auto preview = doc->copy();
    require(region.compose(*preview, proposal.get()), "No preview composition");
    require(sp_repr_save_buf(doc->getReprDoc()).raw() == original_xml, "Preview changed the original document");
    Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
    auto composed = region.compose(*doc, proposal.get());
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Test region edit"), "ai-assistant");
    require(composed, "No applied composition");
    require(doc->getObjectById("target"), "Original object ID was lost");
    require(!doc->getObjectById("outline"), "Outline was not consumed");
    auto after = render(*doc);
    auto expected = render(*preview);
    auto inside = Geom::Point(50, 40) * affine;
    auto outside = Geom::Point(10, 40) * affine;
    // A point inside the loop's bounding box, but outside the triangle.
    auto corner = Geom::Point(22, 70) * affine;
    auto sample = pixel(*after, inside.x(), inside.y());
    if (sample != std::array<unsigned char, 4>{0, 0, 255, 255}) {
        std::cerr << "At " << inside << " got " << int(sample[0]) << "," << int(sample[1]) << "," << int(sample[2]) << "," << int(sample[3]) << '\n';
        std::cerr << sp_repr_save_buf(doc->getReprDoc()).raw() << '\n';
    }
    require(pixel(*after, inside.x(), inside.y()) == std::array<unsigned char, 4>{0, 0, 255, 255}, "Replacement is not blue inside the loop");
    for (auto point : {outside, corner}) {
        require(pixel(*before, point.x(), point.y()) == pixel(*after, point.x(), point.y()), "Artwork outside the loop changed");
    }
    for (int y = 0; y < 200; ++y) for (int x = 0; x < 200; ++x) {
        require(pixel(*after, x, y) == pixel(*expected, x, y), "Preview and apply differ");
    }
    auto reloaded = parse(sp_repr_save_buf(doc->getReprDoc()).raw());
    auto saved = render(*reloaded);
    require(pixel(*saved, inside.x(), inside.y()) == pixel(*after, inside.x(), inside.y()), "Save/reload changed the patch");
    require(Inkscape::DocumentUndo::undo(doc.get()), "Region edit could not be undone");
    doc->ensureUpToDate();
    require(bool(doc->getObjectById("outline")) == !capture_boundary, "Edit undo restored the wrong boundary state");
    auto undone = render(*doc);
    for (int y = 0; y < 200; ++y) for (int x = 0; x < 200; ++x) {
        require(pixel(*undone, x, y) == pixel(*before, x, y), "Undo changed original pixels");
    }
    require(Inkscape::DocumentUndo::redo(doc.get()), "Region edit could not be redone");
    auto redone = render(*doc);
    require(pixel(*redone, inside.x(), inside.y()) == pixel(*after, inside.x(), inside.y()), "Redo failed to restore the patch");
    if (capture_boundary) {
        require(Inkscape::DocumentUndo::undo(doc.get()), "Could not undo patch a second time");
        require(Inkscape::DocumentUndo::undo(doc.get()), "Could not undo boundary capture");
        require(doc->getObjectById("outline"), "Capture undo did not restore the outline");
        require(Inkscape::DocumentUndo::redo(doc.get()), "Could not redo boundary capture");
        require(!doc->getObjectById("outline"), "Capture redo did not remove the outline");
    }
}

static void check_recolor()
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200'>"
        "<g transform='translate(10,15)'><g id='target'><path id='ink' d='M0,0H100V100H0Z' fill='black'/>"
        "<rect x='40' y='40' width='20' height='20' fill='white'/></g>"
        "<path id='loop' d='M20,20H80V80H20Z' fill='none'/></g></svg>");
    AIRegion region(dynamic_cast<SPItem *>(doc->getObjectById("target")),
                    dynamic_cast<SPShape *>(doc->getObjectById("loop")));
    auto original = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto context_png = region.reference_png(*doc, true);
    require(context_png != region.reference_png(*doc), "Context image must show more than the crop");
    require(sp_repr_save_buf(doc->getReprDoc()).raw() == original, "Context rendering changed the original");
    auto preview = doc->copy();
    region.compose(*preview, nullptr, "#ffc0cb");
    auto image = render(*preview);
    auto recolored = pixel(*image, 40, 65);
    if (recolored != std::array<unsigned char, 4>{255,192,203,255}) {
        std::cerr << "Recolor pixel: " << int(recolored[0]) << ',' << int(recolored[1]) << ',' << int(recolored[2]) << ',' << int(recolored[3]) << '\n';
        std::cerr << sp_repr_save_buf(preview->getReprDoc()).raw() << '\n';
    }
    require(pixel(*image, 40, 65) == std::array<unsigned char, 4>{255,192,203,255}, "Original dark outline was not recolored pink");
    require(pixel(*image, 60, 65) == std::array<unsigned char, 4>{255,255,255,255}, "White eye gap changed color");
    require(pixel(*image, 20, 65) == std::array<unsigned char, 4>{0,0,0,255}, "Recolor escaped the selected region");
    require(std::string(preview->getObjectById("ink")->getRepr()->attribute("d")) == "M0,0H100V100H0Z", "Source path geometry changed");
    require(sp_repr_save_buf(doc->getReprDoc()).raw() == original, "Recolor preview mutated the source");
    bool rejected = false;
    try { region.compose(*doc, nullptr, "url(https://invalid)"); }
    catch (std::runtime_error const &) { rejected = true; }
    require(rejected && sp_repr_save_buf(doc->getReprDoc()).raw() == original, "Invalid paint was not rejected before mutation");
    Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
    region.compose(*doc, nullptr, "#ffc0cb");
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Test region recolor"), "ai-assistant");
    auto applied = render(*doc);
    for (int y = 0; y < 200; ++y) for (int x = 0; x < 200; ++x)
        require(pixel(*applied, x, y) == pixel(*image, x, y), "Recolor preview and apply differ");
    auto saved = parse(sp_repr_save_buf(doc->getReprDoc()).raw());
    auto reloaded = render(*saved);
    require(pixel(*reloaded, 40, 65) == pixel(*image, 40, 65), "Recolor did not survive save/reload");
    require(Inkscape::DocumentUndo::undo(doc.get()), "Could not undo recolor");
    auto undone = render(*doc);
    require(pixel(*undone, 40, 65) == std::array<unsigned char, 4>{0,0,0,255}, "Undo did not restore black outline");
}

static void check_partial_contour_recolor(char const *paint)
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
        "<path id='shape' fill-rule='evenodd' d='M10,10H50V50H10Z M20,20H40V40H20Z M60,10H90V50H60Z'/></svg>");
    Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Fixture"), "ai-assistant");
    Inkscape::DocumentUndo::clearUndo(doc.get());
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    Inkscape::UI::Dialog::AIVectorEdit editor(*doc, dynamic_cast<SPItem *>(doc->getObjectById("shape")));
    editor.apply(*doc, editor.parse(nlohmann::json({{"targets", {"p0"}}, {"operation", {{"type", "recolor"}, {"color", "#ff0088"}, {"paint", paint}}}}).dump()));
    std::unique_ptr<Inkscape::Pixbuf> rendered(sp_generate_internal_bitmap(doc.get(), Geom::Rect(Geom::Point(0,0), Geom::Point(100,100)), 96));
    require(bool(rendered), "Partial recolor render failed");
    require(pixel(*rendered, 15, 15) == std::array<unsigned char,4>{255,0,136,255}, "Selected contour was not pink");
    require(pixel(*rendered, 30, 30)[3] == 0, "Partial recolor filled a compound path hole");
    require(pixel(*rendered, 75, 30) == std::array<unsigned char,4>{0,0,0,255}, "Partial recolor changed another contour");
    require(doc->getObjectById("shape"), "Partial recolor lost the scope ID");
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Partial recolor"), "ai-assistant");
    require(Inkscape::DocumentUndo::undo(doc.get()), "Partial recolor undo failed");
    require(sp_repr_save_buf(doc->getReprDoc()).raw() == before, "Partial recolor undo changed the original");
}

static void check_partial_stroke_recolor()
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
        "<g transform='translate(5,5)' fill='#ffff00' stroke='black' stroke-width='4'>"
        "<path id='shape' d='M10,10H40V40H10Z M60,10H90V40H60Z'/></g></svg>");
    Inkscape::UI::Dialog::AIVectorEdit editor(*doc, dynamic_cast<SPItem *>(doc->getObjectById("shape")));
    auto catalog = nlohmann::json::parse(editor.catalog());
    require(catalog["candidates"][0]["has_stroke"] == true, "Inherited stroke missing from catalog");
    editor.apply(*doc, editor.parse(R"({"targets":["p0"],"operation":{"type":"recolor","color":"#ff0088","paint":"stroke"}})"));
    std::unique_ptr<Inkscape::Pixbuf> rendered(sp_generate_internal_bitmap(doc.get(), Geom::Rect(Geom::Point(0,0), Geom::Point(100,100)), 96));
    require(bool(rendered), "Partial stroke render failed");
    require(pixel(*rendered, 15, 25) == std::array<unsigned char,4>{255,0,136,255}, "Selected stroke was not pink");
    require(pixel(*rendered, 30, 30) == std::array<unsigned char,4>{255,255,0,255}, "Stroke recoloring changed the fill");
    require(pixel(*rendered, 65, 25) == std::array<unsigned char,4>{0,0,0,255}, "Stroke recoloring changed another contour");
}

static void check_missing_paint_preview()
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
        "<path id='shape' d='M10,20h5v5h-5Z M20,20h5v5h-5Z M30,20h5v5h-5Z M40,20h5v5h-5Z M50,20h5v5h-5Z M60,20h5v5h-5Z M70,20h5v5h-5Z'/></svg>");
    Inkscape::UI::Dialog::AIVectorEdit editor(*doc, dynamic_cast<SPItem *>(doc->getObjectById("shape")));
    // Reproduce the response shown in the user's screenshot: no paint field.
    auto plan = editor.parse(R"({"targets":["p4","p5","p6"],"operation":{"type":"recolor","color":"#FF69B4"},"reason":"Selected eye features"})");
    require(plan.paint == "auto", "Missing paint did not default to existing paint");
    auto candidate = doc->copy();
    editor.apply(*candidate, plan);
    auto rendered = render(*candidate);
    for (int x : {52,62,72})
        require(pixel(*rendered, x, 22) == std::array<unsigned char,4>{255,105,180,255}, "Missing-paint response did not render pink");
    require(pixel(*rendered, 12, 22) == std::array<unsigned char,4>{0,0,0,255}, "Missing-paint response recolored an unselected part");
    editor.fit_preview(*candidate, plan);
    auto review = editor.selection_preview(*doc, plan);
    require(candidate->getWidth().value("px") < 40 && candidate->getHeight().value("px") < 20, "Preview did not zoom to the selected parts");
    require(std::abs(candidate->getWidth().value("px") - review->getWidth().value("px")) < .001, "Before/after preview scales differ");
    // Page-fit translation must leave the visible result in the preview viewport.
    auto zoomed = render(*candidate);
    require(pixel(*zoomed, 7, 7) == std::array<unsigned char,4>{255,105,180,255}, "Zoomed preview lost the pink geometry");
    for (auto const *bad : {
        R"({"targets":["p0"],"operation":{"type":"recolor"}})",
        R"({"targets":["p0"],"operation":{"type":"recolor","color":"#ff0088","paint":5}})",
        R"({"targets":["p0"],"operation":{}})"
    }) {
        bool rejected = false;
        try { editor.parse(bad); } catch (std::exception const &e) {
            rejected = true;
            require(std::string(e.what()).find("json.exception") == std::string::npos, "Raw JSON exception leaked into the UI");
        }
        require(rejected, "Malformed geometry command was accepted");
    }
}

static void check_clarification_and_multicolor()
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
        "<path id='shape' fill-rule='evenodd' d='M10,10H90V90H10Z M20,20H80V80H20Z M40,40H60V60H40Z M92,10H98V20H92Z'/></svg>");
    Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Fixture"), "ai-assistant");
    Inkscape::DocumentUndo::clearUndo(doc.get());
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    Inkscape::UI::Dialog::AIVectorEdit editor(*doc, dynamic_cast<SPItem *>(doc->getObjectById("shape")));
    auto question = editor.parse(R"({"clarification":"Should the outline be red and the pupil blue?"})");
    require(!question.clarification.empty() && question.targets.empty(), "Clarification is not a distinct non-edit response");
    bool rejected = false;
    try { editor.apply(*doc, question); } catch (std::exception const &) { rejected = true; }
    require(rejected && sp_repr_save_buf(doc->getReprDoc()).raw() == before, "Clarification modified the document");
    auto plan = editor.parse(R"({"edits":[{"targets":["p0"],"operation":{"type":"recolor","color":"#FF0000"}},{"targets":["p2"],"operation":{"type":"recolor","color":"#0000FF"}}],"reason":"Outline red and inner feature blue"})");
    auto preview = doc->copy();
    editor.apply(*preview, plan);
    require(sp_repr_save_buf(doc->getReprDoc()).raw() == before, "Multi-color preview changed the original");
    editor.apply(*doc, plan);
    auto image = render(*doc);
    require(pixel(*image, 15, 15) == std::array<unsigned char,4>{255,0,0,255}, "Outer contour was not red");
    require(pixel(*image, 50, 50) == std::array<unsigned char,4>{0,0,255,255}, "Outer contour covered the blue inner feature");
    require(pixel(*image, 30, 30)[3] == 0, "Multi-color recolor filled a hole");
    require(pixel(*image, 95, 15) == std::array<unsigned char,4>{0,0,0,255}, "Multi-color recolor changed an unselected feature");
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Multi-color recolor"), "ai-assistant");
    require(Inkscape::DocumentUndo::undo(doc.get()), "Multi-color undo failed");
    require(sp_repr_save_buf(doc->getReprDoc()).raw() == before, "Multi-color undo was not exact");
    require(Inkscape::DocumentUndo::redo(doc.get()), "Multi-color redo failed");
    for (auto const *bad : {
        R"({"edits":[{"targets":["p0"],"operation":{"type":"recolor","color":"#FF0000"}},{"targets":["p0"],"operation":{"type":"recolor","color":"#0000FF"}}]})",
        R"({"edits":[{"targets":["p0"],"operation":{"type":"move","x":1,"y":1}}]})",
        R"({"edits":[]})",
        R"({"edits":[{"edits":[]}]})"
    }) {
        rejected = false;
        try { editor.parse(bad); } catch (std::exception const &) { rejected = true; }
        require(rejected, "Invalid multi-color proposal was accepted");
    }
    auto stroked = parse("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
        "<path id='stroke' fill='#ffff00' stroke='black' stroke-width='4' d='M10,10H40V40H10Z M60,10H90V40H60Z'/></svg>");
    Inkscape::UI::Dialog::AIVectorEdit stroke_editor(*stroked, dynamic_cast<SPItem *>(stroked->getObjectById("stroke")));
    auto strokes = stroke_editor.parse(R"({"edits":[{"targets":["p0"],"operation":{"type":"recolor","color":"#FF0000","paint":"stroke"}},{"targets":["p1"],"operation":{"type":"recolor","color":"#0000FF","paint":"stroke"}}]})");
    stroke_editor.apply(*stroked, strokes);
    image = render(*stroked);
    require(pixel(*image, 10, 25) == std::array<unsigned char,4>{255,0,0,255}, "First stroke was not red");
    require(pixel(*image, 60, 25) == std::array<unsigned char,4>{0,0,255,255}, "Second stroke was not blue");
    require(pixel(*image, 25, 25) == std::array<unsigned char,4>{255,255,0,255}, "Multi-color strokes changed the fill");
}

static void check_direct_page_fit()
{
    using Inkscape::UI::Dialog::AIActions;
    for (auto text : {"fit the page dimensions to the scenery", "Fit page to drawing",
                      "Please fit the page to the entire artwork!", "Can you fit the page to the scenery?",
                      "fit to page", "resize page to content"}) {
        require(AIActions::page_fit_request(text) == std::optional<std::string>("drawing"), "Direct drawing-fit request was not recognized");
    }
    require(AIActions::page_fit_request("fit page to selection") == std::optional<std::string>("selection"), "Selection fit request was lost");
    for (auto text : {"do not fit page to drawing", "fit the scenery to the page", "fit page to drawing and recolor the sky",
                      "how do I fit the page to the artwork", "draw a page with text fit page to drawing"}) {
        require(!AIActions::page_fit_request(text), "Unrelated or compound request triggered immediate page fitting");
    }
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='500' height='500'>"
        "<rect id='scenery' x='20' y='30' width='300' height='200'/>"
        "<circle id='sun' cx='100' cy='80' r='10'/></svg>");
    Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Fixture"), "ai-assistant");
    Inkscape::DocumentUndo::clearUndo(doc.get());
    auto original = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto shortcut = doc->copy();
    shortcut->ensureUpToDate();
    Inkscape::ObjectSet empty(shortcut.get());
    shortcut->getPageManager().fitToSelection(&empty);
    shortcut->ensureUpToDate();
    // A selected sun must not shrink the page to the sun when fitting scenery.
    AIActions::fit_page(*doc, {dynamic_cast<SPItem *>(doc->getObjectById("sun"))}, true);
    require(std::abs(doc->getWidth().value("px") - 300) < .001 && std::abs(doc->getHeight().value("px") - 200) < .001,
            "Immediate page fit used the small selection or failed to resize");
    require(std::abs(doc->getWidth().value("px") - shortcut->getWidth().value("px")) < .001 &&
            std::abs(doc->getHeight().value("px") - shortcut->getHeight().value("px")) < .001,
            "Immediate fit differs from Ctrl+Shift+R with no selection");
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Fit page immediately"), "ai-assistant");
    require(Inkscape::DocumentUndo::undo(doc.get()), "Immediate page fit could not be undone");
    require(sp_repr_save_buf(doc->getReprDoc()).raw() == original, "Immediate page fit undo did not restore the drawing");
    AIActions::fit_page(*doc, {dynamic_cast<SPItem *>(doc->getObjectById("sun"))}, false);
    require(std::abs(doc->getWidth().value("px") - 20) < .001, "Explicit selection fitting did not use the selected sun");
    auto blank = parse("<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200'/>");
    auto blank_xml = sp_repr_save_buf(blank->getReprDoc()).raw();
    bool rejected = false;
    try { AIActions::fit_page(*blank, {}, true); } catch (std::exception const &) { rejected = true; }
    require(rejected && blank_xml == sp_repr_save_buf(blank->getReprDoc()).raw(), "Empty-document fit did not fail without mutation");
}

static void check_builtin_commands()
{
    using Inkscape::UI::Dialog::AIActions;
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200'>"
        "<rect id='a' x='20' y='30' width='20' height='10'/><rect id='b' x='70' y='50' width='10' height='20'/></svg>");
    Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Fixture"), "ai-assistant");
    Inkscape::DocumentUndo::clearUndo(doc.get());
    auto before=sp_repr_save_buf(doc->getReprDoc()).raw();
    AIActions tools(*doc,{dynamic_cast<SPItem *>(doc->getObjectById("a"))});
    auto catalog=nlohmann::json::parse(tools.catalog());
    require(catalog["selection"] == nlohmann::json::array({"a"}), "Command catalog lost selection");
    auto fit=tools.parse(R"({"commands":[{"tool":"fit_page","scope":"drawing"}]})");
    auto preview=doc->copy(); tools.apply(*preview,fit);
    require(sp_repr_save_buf(doc->getReprDoc()).raw()==before,"Command preview changed source document");
    require(std::abs(preview->getWidth().value("px")-60)<.001 && std::abs(preview->getHeight().value("px")-40)<.001,
        "Fit page used the selected object instead of the whole drawing");
    tools.apply(*doc,fit);
    Inkscape::DocumentUndo::done(doc.get(),RC_("Undo","Command fit page"),"ai-assistant");
    require(Inkscape::DocumentUndo::undo(doc.get()),"Page command undo failed");
    require(sp_repr_save_buf(doc->getReprDoc()).raw()==before,"Page command undo was not exact");
    auto grouping=tools.parse(R"({"commands":[{"tool":"group","targets":["a","b"]},{"tool":"move","targets":"selection","x":10,"y":5},{"tool":"ungroup","targets":"selection"},{"tool":"align","targets":"selection","axis":"x","edge":"center"}]})");
    auto changed=doc->copy();
    auto selected=tools.apply(*changed,grouping);
    require(selected.size()==2,"Group/ungroup did not retain the result selection");
    auto a=dynamic_cast<SPItem *>(changed->getObjectById("a"));
    auto b=dynamic_cast<SPItem *>(changed->getObjectById("b"));
    require(a && b,"Built-in grouping lost object IDs");
    require(std::abs(a->desktopVisualBounds()->midpoint().x()-b->desktopVisualBounds()->midpoint().x())<.001,"Built-in alignment failed");
    tools.apply(*doc,grouping);
    Inkscape::DocumentUndo::done(doc.get(),RC_("Undo","Combined native commands"),"ai-assistant");
    require(Inkscape::DocumentUndo::undo(doc.get()),"Combined command undo failed");
    require(sp_repr_save_buf(doc->getReprDoc()).raw()==before,"Combined command undo was not exact");
    auto transformed=doc->copy();
    tools.apply(*transformed,tools.parse(R"({"commands":[{"tool":"scale","targets":["a"],"x":2,"y":1},{"tool":"rotate","targets":["a"],"degrees":90},{"tool":"raise","targets":["a"]}]})"));
    auto transformed_a=dynamic_cast<SPItem *>(transformed->getObjectById("a"));
    require(std::abs(transformed_a->desktopVisualBounds()->width()-10)<.001 && std::abs(transformed_a->desktopVisualBounds()->height()-40)<.001,"Native scale/rotate failed");
    require(transformed->getObjectById("a")->getRepr()->next()==nullptr,"Native raise failed");
    tools.apply(*transformed,tools.parse(R"({"commands":[{"tool":"lower","targets":["a"]}]})"));
    require(transformed->getObjectById("a")->getRepr()->next()==transformed->getObjectById("b")->getRepr(),"Native lower failed");
    auto styled=doc->copy();
    tools.apply(*styled,tools.parse(R"({"commands":[{"tool":"set_attributes","targets":["a"],"attributes":{"fill":"#FF69B4","opacity":0.5}},{"tool":"resize_page","width":100,"height":80}]})"));
    require(std::abs(styled->getWidth().value("px")-100)<.001 && std::abs(styled->getHeight().value("px")-80)<.001,"Built-in resize page failed");
    require(std::string(styled->getObjectById("a")->getRepr()->attribute("style")).find("#ff69b4")!=std::string::npos ||
        std::string(styled->getObjectById("a")->getRepr()->attribute("style")).find("#FF69B4")!=std::string::npos,"Attribute edit did not set paint");
    for(auto bad:{
        R"({"commands":[{"tool":"run_shell","targets":["a"]}]})",
        R"({"commands":[{"tool":"move","targets":["missing"],"x":1,"y":0}]})",
        R"({"commands":[{"tool":"resize_page","width":-1,"height":80}]})",
        R"({"commands":[{"tool":"set_attributes","targets":["a"],"attributes":{"onclick":"bad"}}]})",
        R"BAD({"commands":[{"tool":"set_attributes","targets":["a"],"attributes":{"fill":"url(https://example.com)"}}]})BAD"
    }) {
        bool rejected=false; try { tools.parse(bad); } catch(std::exception const &) { rejected=true; }
        require(rejected,"Unsafe/invalid command accepted");
    }
    auto question=tools.parse(R"({"clarification":"Which objects should be aligned?"})");
    require(!question.clarification.empty(),"Command clarification was not preserved");
    auto paths=parse("<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200'><path id='p' d='M10,10H20V20H10Z'/></svg>");
    AIActions path_tools(*paths,{});
    path_tools.apply(*paths,path_tools.parse(R"({"commands":[{"tool":"set_path","targets":["p"],"d":"M10,10H40V20H10Z"}]})"));
    require(std::abs(dynamic_cast<SPItem *>(paths->getObjectById("p"))->documentVisualBounds()->width()-30)<.001,"Targeted path XML edit failed");
}

static void check_geometry_commands()
{
    using Inkscape::UI::Dialog::AIVectorEdit;
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' width='200' height='200' viewBox='0 0 100 100'>"
        "<g id='scope' transform='translate(5,7) scale(1.2,.8)'>"
        "<path id='compound' d='M10,10H20V15H10Z M50,50H60V60H50Z'/>"
        "<rect id='box' x='70' y='20' width='10' height='10'/>"
        "<path id='locked' sodipodi:insensitive='true' d='M0,0H10V10Z'/></g>"
        "<path id='unrelated' d='M80,80H90V90Z'/></svg>");
    Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Initialize geometry fixture"), "ai-assistant");
    Inkscape::DocumentUndo::clearUndo(doc.get());
    auto root = dynamic_cast<SPItem *>(doc->getObjectById("scope"));
    Geom::Rect hint(Geom::Point(35,30), Geom::Point(45,35));
    AIVectorEdit editor(*doc, root, hint, "M35,30H45V35H35Z");
    auto catalog = nlohmann::json::parse(editor.catalog());
    require(catalog.at("candidates").size() == 3, "Locked geometry entered the candidate catalog");
    auto original_xml = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto path = dynamic_cast<SPPath *>(doc->getObjectById("compound"));
    auto original_curve = *path->curve();
    auto original_first = Geom::PathVector{original_curve[0]} * path->i2doc_affine();
    auto untouched = sp_svg_write_path(Geom::PathVector{original_curve[1]});
    auto plan = editor.parse(R"({"targets":["p0"],"operation":{"type":"scale","x":1.5,"y":1},"reason":"Complete feature, including its portion outside the lasso"})");
    auto fenced = editor.parse("```json\n{\"targets\":[\"p0\"],\"operation\":{\"type\":\"scale\",\"x\":1.5,\"y\":1}}\n```");
    require(fenced.targets == plan.targets, "Fenced geometry JSON was not understood");
    require(editor.summary(plan).find("1 extend beyond") != std::string::npos, "Lasso expansion was not disclosed");
    auto target_preview = editor.selection_preview(*doc, plan);
    require(bool(target_preview), "Missing selection review");
    auto original_image = editor.reference_png(*doc, false);
    auto numbered_image = editor.reference_png(*doc, true);
    require(editor.reference_png(*doc, true, true) != numbered_image, "Lasso detail map was not enlarged");
    if (auto preview_path = g_getenv("AI_GEOMETRY_MAP_OUTPUT")) {
        gsize size = 0;
        g_autofree guchar *data = g_base64_decode(numbered_image.substr(22).c_str(), &size);
        require(g_file_set_contents(preview_path, reinterpret_cast<char const *>(data), size, nullptr), "Could not write map diagnostic");
    }
    require(original_image != numbered_image && numbered_image.starts_with("data:image/png;base64,"), "Missing numbered candidate map");
    require(sp_repr_save_buf(doc->getReprDoc()).raw() == original_xml, "Candidate previews changed the original");
    Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
    editor.apply(*doc, plan);
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Test AI geometry scale"), "ai-assistant");
    auto changed = dynamic_cast<SPPath *>(doc->getObjectById("compound"));
    auto changed_first = Geom::PathVector{(*changed->curve())[0]} * changed->i2doc_affine();
    auto a = original_first.boundsExact(), b = changed_first.boundsExact();
    require(std::abs(b->width() - a->width() * 1.5) < .0001 && std::abs(b->height() - a->height()) < .0001,
            "Document-space width scaling failed under transformed parent/viewBox");
    require(sp_svg_write_path(Geom::PathVector{(*changed->curve())[1]}) == untouched, "Unselected subpath was changed");
    require(doc->getObjectById("compound") && doc->getObjectById("unrelated"), "Geometry editing replaced object identities");
    require(Inkscape::DocumentUndo::undo(doc.get()), "Geometry edit could not be undone");
    doc->ensureUpToDate();
    if (sp_repr_save_buf(doc->getReprDoc()).raw() != original_xml) {
        g_file_set_contents("/tmp/ai-geometry-before.svg", original_xml.c_str(), -1, nullptr);
        auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
        g_file_set_contents("/tmp/ai-geometry-undone.svg", after.c_str(), -1, nullptr);
    }
    require(sp_repr_save_buf(doc->getReprDoc()).raw() == original_xml, "Geometry undo did not restore the exact document");
    require(Inkscape::DocumentUndo::redo(doc.get()), "Geometry edit could not be redone");
    require(Inkscape::DocumentUndo::undo(doc.get()), "Geometry edit second undo failed");
    doc->ensureUpToDate();
    auto moving = doc->copy();
    moving->ensureUpToDate();
    auto box = dynamic_cast<SPItem *>(moving->getObjectById("box"));
    auto box_before = box->documentGeometricBounds();
    editor.apply(*moving, editor.parse(R"({"targets":["p2"],"operation":{"type":"move","x":12,"y":-8}})"));
    auto box_after = box->documentGeometricBounds();
    if (std::abs(box_after->left() - box_before->left() - 12) > .0001 ||
        std::abs(box_after->top() - box_before->top() + 8) > .0001) {
        std::cerr << "Move before " << *box_before << " after " << *box_after << '\n';
    }
    require(std::abs(box_after->left() - box_before->left() - 12) < .0001 &&
            std::abs(box_after->top() - box_before->top() + 8) < .0001, "Whole-object move used wrong coordinate system");
    auto rotating = doc->copy();
    editor.apply(*rotating, editor.parse(R"({"targets":["p0"],"operation":{"type":"rotate","degrees":90}})"));
    auto rotated_path = dynamic_cast<SPPath *>(rotating->getObjectById("compound"));
    auto rotated = (Geom::PathVector{(*rotated_path->curve())[0]} * rotated_path->i2doc_affine()).boundsExact();
    require(std::abs(rotated->width() - a->height()) < .0001, "Rotation failed to preserve geometry");
    auto recoloring = doc->copy();
    editor.apply(*recoloring, editor.parse(R"({"targets":["p0","p1"],"operation":{"type":"recolor","color":"#ff0088","paint":"fill"}})"));
    require(std::string(recoloring->getObjectById("compound")->getRepr()->attribute("style")).find("#ff0088") != std::string::npos,
            "Geometry recolor did not set the actual paint");
    for (auto const *bad : {
        R"({"targets":["made-up"],"operation":{"type":"scale","x":2,"y":1}})",
        R"({"targets":["p0","p0"],"operation":{"type":"move","x":1,"y":1}})",
        R"({"targets":["p0"],"operation":{"type":"scale","x":100000,"y":1}})",
        R"BAD({"targets":["p2"],"operation":{"type":"recolor","color":"url(x)","paint":"fill"}})BAD",
        R"({"targets":["p0"],"operation":{"type":"generate_svg"}})"}) {
        bool rejected = false;
        try { editor.parse(bad); } catch (std::exception const &) { rejected = true; }
        require(rejected, "Invalid geometry command was accepted");
    }
    require(sp_repr_save_buf(doc->getReprDoc()).raw() == original_xml, "Rejected commands changed the drawing");
    auto stale = doc->copy();
    stale->getObjectById("compound")->getRepr()->setAttribute("d", "M1,1H9V9Z");
    stale->ensureUpToDate();
    auto stale_xml = sp_repr_save_buf(stale->getReprDoc()).raw();
    bool rejected = false;
    try { editor.apply(*stale, plan); } catch (std::exception const &) { rejected = true; }
    require(rejected && sp_repr_save_buf(stale->getReprDoc()).raw() == stale_xml, "Stale target was modified");
}

int main()
{
    Inkscape::Util::Statics statics;
    Gio::init();
    Inkscape::GC::init();
    Inkscape::Application::create(false);
    try {
        check_geometry_commands();
        check_partial_contour_recolor("fill");
        check_partial_contour_recolor("stroke");
        check_partial_stroke_recolor();
        check_missing_paint_preview();
        check_clarification_and_multicolor();
        check_builtin_commands();
        check_direct_page_fit();
        check_recolor();
        auto picking = parse("<svg xmlns='http://www.w3.org/2000/svg' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' width='100' height='100'>"
            "<rect id='background' width='100' height='100'/>"
            "<g id='artwork'><path d='M0,0H100V100H0Z' fill='red'/></g>"
            "<rect id='away' x='200' width='10' height='10'/>"
            "<rect id='hidden' width='100' height='100' style='display:none'/>"
            "<rect id='locked' width='100' height='100' sodipodi:insensitive='true'/>"
            "<ellipse id='outline' cx='50' cy='50' rx='20' ry='20' fill='none' stroke='black'/>"
            "<rect id='above' width='100' height='100'/></svg>");
        auto ellipse = dynamic_cast<SPShape *>(picking->getObjectById("outline"));
        require(AIRegion::is_outline(ellipse), "Ellipse boundary was rejected");
        auto picked = AIRegion::target_below(ellipse);
        require(picked == picking->getObjectById("artwork"), "Wrong underlying artwork selected");
        AIRegion picked_region(picked, ellipse);
        require(picked_region.reference_png(*picking).starts_with("data:image/png;base64,"), "Ellipse reference failed");
        auto grouped = parse("<svg xmlns='http://www.w3.org/2000/svg' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'>"
            "<g id='art'><path id='p1' d='M0,0H100V100H0Z'/><g><path id='p2' d='M5,5H50V50Z'/></g>"
            "<path id='hidden-path' style='display:none' d='M0,0H100V100Z'/>"
            "<g sodipodi:insensitive='true'><path id='locked-path' d='M0,0H100V100Z'/></g></g>"
            "<ellipse id='loop' cx='25' cy='25' rx='20' ry='20'/><path id='other' d='M0,0H100V100Z'/></svg>");
        AIRegion group_region(dynamic_cast<SPItem *>(grouped->getObjectById("art")),
                              dynamic_cast<SPShape *>(grouped->getObjectById("loop")));
        auto paths = group_region.node_targets(*grouped);
        require(paths.size() == 2 && paths[0]->getId() == "p1"s && paths[1]->getId() == "p2"s,
                "Node targets must be unlocked visible descendant paths");
        require(group_region.accepts_selection(*grouped, paths), "Descendant path selection was rejected");
        require(!group_region.accepts_selection(*grouped, {}), "Empty selection was accepted");
        paths.push_back(dynamic_cast<SPItem *>(grouped->getObjectById("other")));
        require(!group_region.accepts_selection(*grouped, paths), "Unrelated artwork was accepted");
        auto path = "<path id='target' fill='red' d='M0,0 H100 V100 H0 Z'/>"s;
        check_case(path, "translate(10,15)", false);
        check_case(path, "translate(25,5) rotate(10) scale(1.2,.8)", false);
        check_case(path, "translate(0,0)", true);
        check_case(path, "translate(25,5) rotate(10) scale(1.2,.8)", false, true);
        // A real embedded raster target: solid red PNG.
        g_autoptr(GdkPixbuf) pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, false, 8, 4, 4);
        gdk_pixbuf_fill(pb, 0xff0000ff);
        g_autofree char *data = nullptr;
        gsize size = 0;
        require(gdk_pixbuf_save_to_buffer(pb, &data, &size, "png", nullptr, nullptr), "PNG fixture failed");
        g_autofree char *encoded = g_base64_encode(reinterpret_cast<guchar *>(data), size);
        check_case("<image id='target' width='100' height='100' xlink:href='data:image/png;base64,"s + encoded + "'/>", "translate(15,15)", false);
        auto invalid = parse("<svg xmlns='http://www.w3.org/2000/svg'><path id='target' d='M0,0H100V100H0Z'/><path id='outline' d='M10,10L20,20'/></svg>");
        bool rejected = false;
        try { AIRegion region(dynamic_cast<SPItem *>(invalid->getObjectById("target")), dynamic_cast<SPPath *>(invalid->getObjectById("outline"))); }
        catch (std::runtime_error const &) { rejected = true; }
        require(rejected, "An open outline was accepted");
        std::cout << "PASS: paths, embedded raster, transformed parents, scaled viewBox, clipping, immutable preview, ID preservation, save/reload, undo/redo, cropped reference, open-loop rejection, underlying artwork selection, ellipse boundaries, captured boundaries with undo/redo, exact dark recolor and white-gap preservation, geometry candidate selection and transforms, partial contour recolor preserving holes, clarification responses and multi-color edits, native Inkscape commands, immediate page fitting and shortcut equivalence\n";
        return 0;
    } catch (std::exception const &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
