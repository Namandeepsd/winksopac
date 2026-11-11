// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/dialog/ai-design-assets.h"
#include "ai/svg-validation.h"
#include "document.h"
#include "object/sp-root.h"
#include "inkscape.h"
#include "inkgc/gc-core.h"
#include "util/statics.h"
#include "xml/repr.h"
#include "document-undo.h"
#include "display/cairo-utils.h"
#include "helper/pixbuf-ops.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glibmm/init.h>
#include <glibmm/i18n.h>
#include <giomm/init.h>
#include <iostream>
#include <fstream>
using namespace Inkscape::AI;
using namespace Inkscape::UI::Dialog;
namespace {
void check(bool condition, char const *message) { if (!condition) throw std::runtime_error(message); }
template <typename F> void rejects(F action) { bool rejected = false; try { action(); } catch (...) { rejected = true; } check(rejected, "Unsafe or incomplete composition accepted"); }
}
int main()
{
    Inkscape::Util::Statics statics;
    Inkscape::GC::init(); Glib::init(); Gio::init(); Inkscape::Application::create(false);
    try {
        auto bus = std::make_shared<MessageBus>(); bus->register_tasks({"test"});
        std::stop_source stop;
        ExecutionContext execution("test", 1, bus, stop.get_token(), Clock::now() + std::chrono::seconds(30));
        Json metadata = {{"title", "Test & shapes"}, {"source", "https://commons.wikimedia.org/wiki/File:Test.svg"}, {"license", "CC0"}, {"attribution", "Test author"},
            {"download", "https://upload.wikimedia.org/wikipedia/commons/a/ab/Test.svg"}, {"thumbnail", "https://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/Test.svg/800px-Test.svg.png"}, {"mime", "image/svg+xml"}};
        std::string source = "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='50' viewBox='0 0 100 50'><rect width='100' height='50' style='fill:#ff0000;stroke:none'/></svg>";
        auto vector = prepare_design_asset(metadata, "asset-1", [&](auto const &, auto &) { return source; }, execution);
        check(vector.available() && !vector.svg.empty() && vector.width == 100 && vector.height == 50, "Vector download/thumbnail/style conversion failed");
        auto raster_meta = metadata; raster_meta["mime"] = "image/png";
        auto raster = prepare_design_asset(raster_meta, "asset-2", [&](auto const &, auto &) { return vector.png; }, execution);
        check(raster.available() && raster.svg.empty(), "PNG normalization failed");
        unsigned fetches = 0;
        auto fallback = prepare_design_asset(metadata, "asset-3", [&](auto const &, auto &) {
            return ++fetches == 1 ? std::string("<svg xmlns='http://www.w3.org/2000/svg'><script>alert(1)</script></svg>") : vector.png;
        }, execution);
        check(fallback.available() && fallback.svg.empty() && fetches == 2, "Unsafe SVG did not use safe PNG fallback");
        auto broken = prepare_design_asset(raster_meta, "asset-4", [](auto const &, auto &) { return "not an image"; }, execution);
        check(!broken.available() && !broken.error.empty(), "Invalid image accepted");
        for (auto url : {"file:///etc/passwd", "https://upload.wikimedia.org.evil/wikipedia/commons/x", "https://user@upload.wikimedia.org/wikipedia/commons/x", "https://upload.wikimedia.org:443/wikipedia/commons/x"})
            check(!commons_download_url(url), "Untrusted download URL accepted");
        check(commons_download_url("https://thumb.wikimedia.org/wikipedia/commons/thumb/a/ab/Test.svg/960px-Test.svg.png?utm_source=commons.wikimedia.org"), "Commons thumbnail CDN/tracking URL rejected");
        std::string layout = "<svg xmlns='http://www.w3.org/2000/svg' width='300' height='200'><rect id='asset-1' x='20' y='40' width='100' height='100'/><rect id='asset-2' x='150' y='40' width='100' height='100'/></svg>";
        auto composed = compose_design_assets(layout, {vector, raster});
        check(composed.find("data:image/png;base64,") != std::string::npos && composed.find("Test author") != std::string::npos, "Embedded pixels or attribution missing");
        check(composed.find("matrix(1 0 0 1 20 65)") != std::string::npos, "Vector aspect ratio/centering incorrect");
        rejects([&] { validate_generated_svg(composed); }); // The generated-model route stays restricted.
        rejects([&] { compose_design_assets("<svg xmlns='http://www.w3.org/2000/svg'/>", {vector}); });
        rejects([&] { compose_design_assets("<svg xmlns='http://www.w3.org/2000/svg'><image href='https://evil.test/a.png'/></svg>", {}); });
        rejects([&] { compose_design_assets("<svg xmlns='http://www.w3.org/2000/svg'><rect id='asset-1'/><rect id='asset-1'/></svg>", {vector}); });
        auto doc = SPDocument::createNewDocFromMem(std::span<char const>(composed.data(), composed.size()));
        check(bool(doc), "Composed SVG could not open in Inkscape");
        doc->ensureUpToDate();
        auto bounds = doc->getRoot()->documentVisualBounds();
        check(bounds && bounds->width() > 200 && bounds->height() >= 50, "Assets have missing/incorrect rendered bounds");
        auto saved = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto reloaded = SPDocument::createNewDocFromMem(std::span<char const>(saved.data(), saved.size()));
        reloaded->ensureUpToDate();
        check(reloaded->getRoot()->documentVisualBounds().has_value(), "Embedded assets did not survive save/reload");
        auto assert_pixels = [](SPDocument &drawing) {
            std::unique_ptr<Inkscape::Pixbuf> bitmap(sp_generate_internal_bitmap(&drawing, Geom::Rect(Geom::Point(0, 0), Geom::Point(300, 200)), 96));
            check(bool(bitmap), "Asset rendering failed");
            auto pb = bitmap->getPixbufRaw();
            for (auto x : {40, 170}) {
                auto pixel = gdk_pixbuf_get_pixels(pb) + 80 * gdk_pixbuf_get_rowstride(pb) + x * gdk_pixbuf_get_n_channels(pb);
                check(pixel[0] > 250 && pixel[1] < 5 && pixel[2] < 5, "Actual vector or raster pixels missing at expected position");
            }
        };
        assert_pixels(*reloaded);
        std::string empty = "<svg xmlns='http://www.w3.org/2000/svg' width='300' height='200'/>";
        auto destination = SPDocument::createNewDocFromMem(std::span<char const>(empty.data(), empty.size()));
        Inkscape::DocumentUndo::setUndoSensitive(destination.get(), true);
        destination->import(*reloaded, destination->getReprRoot(), nullptr, Geom::identity(), nullptr, SPDocument::ImportRoot::AlwaysGroup);
        destination->ensureUpToDate();
        Inkscape::DocumentUndo::done(destination.get(), RC_("Undo", "Insert asset composition"), "ai-assistant");
        assert_pixels(*destination);
        Inkscape::DocumentUndo::undo(destination.get());
        destination->ensureUpToDate();
        check(!destination->getRoot()->documentVisualBounds(), "Undo left imported assets behind");
        Inkscape::DocumentUndo::redo(destination.get());
        destination->ensureUpToDate();
        assert_pixels(*destination);
        std::ofstream("/tmp/ai-design-assets-demo.svg") << saved;
        stop.request_stop();
        check(!prepare_design_asset(metadata, "cancel", [&](auto const &, auto &) { throw std::runtime_error("Fetch must not run"); return source; }, execution).available(), "Cancelled download accepted");
        std::cout << "PASS: download validation, SVG sanitization, PNG fallback, thumbnails, asset placement, provenance and Inkscape save/reload\n";
    } catch (std::exception const &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
