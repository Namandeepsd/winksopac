// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai-design-assets.h"
#include "ai/svg-validation.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <cmath>
#include <set>
#include <memory>
#include <sstream>
#include <array>
#include <locale>

namespace Inkscape::UI::Dialog {
namespace {
using Doc = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
Doc parse(std::string const &text)
{
    return Doc(xmlReadMemory(text.data(), text.size(), "asset.svg", "UTF-8",
        XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc);
}
std::string prop(xmlNode *node, char const *name)
{
    auto value = xmlGetProp(node, BAD_CAST name);
    std::string result = value ? reinterpret_cast<char const *>(value) : "";
    xmlFree(value);
    return result;
}
std::string serialize(xmlDoc *doc)
{
    xmlChar *buffer = nullptr;
    int size = 0;
    xmlDocDumpMemoryEnc(doc, &buffer, &size, "UTF-8");
    std::string result = buffer ? std::string(reinterpret_cast<char *>(buffer), size) : "";
    xmlFree(buffer);
    if (result.empty() || result.size() > 12 * 1024 * 1024) throw std::runtime_error("Asset composition exceeds 12 MB.");
    return result;
}
double number(std::string const &text, bool positive = false)
{
    char *end = nullptr;
    double value = g_ascii_strtod(text.c_str(), &end);
    if (end == text.c_str() || *end || !std::isfinite(value) || std::abs(value) > 1e6 || (positive && value <= 0))
        throw std::runtime_error("Asset positions and sizes must be finite SVG numbers.");
    return value;
}
std::array<double, 4> viewbox(xmlNode *root)
{
    auto text = prop(root, "viewBox");
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    std::array<double, 4> box;
    for (auto &value : box) {
        if (!(input >> value) || !std::isfinite(value) || std::abs(value) > 1e6) throw std::runtime_error("Invalid asset viewBox.");
    }
    std::string extra;
    if (input >> extra || box[2] <= 0 || box[3] <= 0) throw std::runtime_error("Invalid asset viewBox.");
    return box;
}
// Flatten safe inline presentation styles before applying the existing strict
// validator. Complex SVG features fall back to Commons' raster rendition.
std::string safe_vector(std::string const &bytes)
{
    if (bytes.size() > 512 * 1024 || bytes.find("<!DOCTYPE") != std::string::npos || bytes.find("<!ENTITY") != std::string::npos)
        throw std::runtime_error("SVG needs a raster rendition.");
    auto doc = parse(bytes);
    if (!doc) throw std::runtime_error("Invalid SVG asset.");
    std::function<void(xmlNode *)> clean = [&](xmlNode *node) {
        for (; node; node = node->next) {
            if (node->type != XML_ELEMENT_NODE) continue;
            if (xmlStrEqual(node->name, BAD_CAST "svg") && node != xmlDocGetRootElement(doc.get()))
                throw std::runtime_error("Nested SVG requires a raster rendition.");
            auto style = prop(node, "style");
            if (!style.empty()) {
                std::istringstream declarations(style);
                std::string declaration;
                while (std::getline(declarations, declaration, ';')) {
                    if (declaration.find_first_not_of(" \t\r\n") == std::string::npos) continue;
                    auto colon = declaration.find(':');
                    if (colon == std::string::npos) throw std::runtime_error("Unsupported SVG style.");
                    auto key = declaration.substr(0, colon), value = declaration.substr(colon + 1);
                    auto trim = [](std::string text) {
                        auto first = text.find_first_not_of(" \t\r\n"), last = text.find_last_not_of(" \t\r\n");
                        return first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
                    };
                    key = trim(key); value = trim(value);
                    static std::set<std::string> const allowed = {"fill", "fill-rule", "fill-opacity", "stroke", "stroke-width", "stroke-opacity",
                        "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset", "opacity",
                        "font-family", "font-size", "font-weight", "font-style", "text-anchor"};
                    if (!allowed.contains(key)) throw std::runtime_error("Unsupported SVG style.");
                    xmlSetProp(node, BAD_CAST key.c_str(), BAD_CAST value.c_str());
                }
                xmlUnsetProp(node, BAD_CAST "style");
            }
            clean(node->children);
        }
    };
    clean(xmlDocGetRootElement(doc.get()));
    auto validated = Inkscape::AI::validate_generated_svg(serialize(doc.get()));
    doc = parse(validated);
    auto root = xmlDocGetRootElement(doc.get());
    if (prop(root, "viewBox").empty()) {
        auto width = prop(root, "width"), height = prop(root, "height");
        if (width.ends_with("px")) width.resize(width.size() - 2);
        if (height.ends_with("px")) height.resize(height.size() - 2);
        number(width, true); number(height, true);
        auto box = "0 0 " + width + " " + height;
        xmlSetProp(root, BAD_CAST "viewBox", BAD_CAST box.c_str());
    }
    viewbox(root);
    return serialize(doc.get());
}
void decode(DesignAsset &asset, std::string const &bytes, bool vector)
{
    if (bytes.empty() || bytes.size() > 4 * 1024 * 1024) throw std::runtime_error("Asset download is empty or too large.");
    char const *type = vector ? "svg" : bytes.starts_with("\x89PNG\r\n\x1a\n") ? "png" :
        (bytes.size() > 2 && static_cast<unsigned char>(bytes[0]) == 255 && static_cast<unsigned char>(bytes[1]) == 216) ? "jpeg" : nullptr;
    if (!type) throw std::runtime_error("Only SVG, PNG and JPEG assets are supported.");
    g_autoptr(GError) error = nullptr;
    g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new_with_type(type, &error);
    if (!loader) throw std::runtime_error("The image decoder is unavailable.");
    bool oversized = false;
    g_signal_connect(loader, "size-prepared", G_CALLBACK(+[](GdkPixbufLoader *loader, int width, int height, gpointer data) {
        auto &oversized = *static_cast<bool *>(data);
        oversized = width <= 0 || height <= 0 || width > 16000 || height > 16000 || int64_t(width) * height > 16000000;
        double scale = std::min(1.0, 800.0 / std::max(width, height));
        gdk_pixbuf_loader_set_size(loader, oversized ? 1 : std::max(1, int(width * scale)), oversized ? 1 : std::max(1, int(height * scale)));
    }), &oversized);
    bool valid = gdk_pixbuf_loader_write(loader, reinterpret_cast<guchar const *>(bytes.data()), bytes.size(), &error);
    if (!valid) g_clear_error(&error);
    valid = gdk_pixbuf_loader_close(loader, &error) && valid;
    if (!valid || oversized) throw std::runtime_error("Asset image is invalid or exceeds the pixel limit.");
    auto pixels = gdk_pixbuf_loader_get_pixbuf(loader);
    if (!pixels) throw std::runtime_error("Asset has no image pixels.");
    gchar *png = nullptr;
    gsize size = 0;
    if (!gdk_pixbuf_save_to_buffer(pixels, &png, &size, "png", &error, nullptr)) throw std::runtime_error("Cannot prepare the asset preview.");
    asset.png.assign(png, size);
    g_free(png);
    if (size > 1024 * 1024) throw std::runtime_error("Prepared image exceeds 1 MB.");
    asset.width = gdk_pixbuf_get_width(pixels); asset.height = gdk_pixbuf_get_height(pixels);
}
}
bool commons_download_url(std::string const &url)
{
    if (url.size() > 4096) return false;
    g_autoptr(GUri) uri = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, nullptr);
    return uri && g_strcmp0(g_uri_get_scheme(uri), "https") == 0 &&
        (g_strcmp0(g_uri_get_host(uri), "upload.wikimedia.org") == 0 || g_strcmp0(g_uri_get_host(uri), "thumb.wikimedia.org") == 0) &&
        !g_uri_get_userinfo(uri) && g_uri_get_port(uri) == -1 && !g_uri_get_fragment(uri) &&
        std::string(g_uri_get_path(uri)).starts_with("/wikipedia/commons/");
}
DesignAsset prepare_design_asset(Inkscape::AI::Json const &metadata, std::string id, AssetDownload const &download, Inkscape::AI::ExecutionContext &execution)
{
    DesignAsset asset;
    asset.id = std::move(id); asset.title = metadata.at("title"); asset.source = metadata.at("source");
    asset.license = metadata.at("license"); asset.attribution = metadata.at("attribution");
    auto fetch = [&](std::string const &url) {
        execution.checkpoint();
        if (!commons_download_url(url)) throw std::runtime_error("No supported Wikimedia download URL is available.");
        // Imageinfo appends analytics parameters; the static file needs none.
        auto bytes = download(url.substr(0, url.find('?')), execution);
        execution.checkpoint();
        return bytes;
    };
    try {
        auto url = metadata.value("download", std::string{}), thumbnail = metadata.value("thumbnail", std::string{});
        if (metadata.value("mime", std::string{}) == "image/svg+xml") {
            try {
                asset.svg = safe_vector(fetch(url));
                decode(asset, asset.svg, true);
            } catch (std::exception const &) {
                execution.checkpoint();
                asset.svg.clear();
                decode(asset, fetch(thumbnail), false);
            }
        } else decode(asset, fetch(thumbnail.empty() ? url : thumbnail), false);
    } catch (std::exception const &error) { asset.svg.clear(); asset.png.clear(); asset.error = error.what(); }
    return asset;
}
std::string asset_layout_prompt(std::vector<DesignAsset> const &assets)
{
    Inkscape::AI::Json catalog = Inkscape::AI::Json::array();
    for (auto const &asset : assets) catalog.push_back({{"id", asset.id}, {"title", asset.title}, {"width", asset.width}, {"height", asset.height}});
    return "\nREAL ASSETS TO PLACE: " + catalog.dump() +
        "\nFor EACH asset, include exactly one empty <rect id=\"asset-N\" x=\"...\" y=\"...\" width=\"...\" height=\"...\"/> using its exact catalog ID. "
        "Use positive unitless sizes and unitless positions in the outer SVG coordinate system; do not nest these rectangles in transformed groups. "
        "The application replaces these rectangles with the actual asset, preserving its aspect ratio. Do not redraw these images or emit image elements, URLs or base64. "
        "Arrange text and original vectors around the asset rectangles. Return only the complete SVG layout.";
}
std::string compose_design_assets(std::string const &layout, std::vector<DesignAsset> const &assets)
{
    auto doc = parse(Inkscape::AI::validate_generated_svg(layout));
    if (assets.size() > 9) throw std::runtime_error("Too many assets in this composition.");
    for (auto const &asset : assets) {
        if (!asset.available()) throw std::runtime_error("A selected asset is unavailable.");
        xmlNode *slot = nullptr;
        std::function<void(xmlNode *)> find = [&](xmlNode *node) {
            for (; node; node = node->next) {
                if (node->type == XML_ELEMENT_NODE && prop(node, "id") == asset.id) {
                    if (slot) throw std::runtime_error("The layout repeats asset " + asset.id);
                    slot = node;
                }
                find(node->children);
            }
        };
        find(xmlDocGetRootElement(doc.get()));
        if (!slot || !xmlStrEqual(slot->name, BAD_CAST "rect")) throw std::runtime_error("The layout is missing the image placeholder " + asset.id + ". Create preview again.");
        auto x = prop(slot, "x"), y = prop(slot, "y"), width = prop(slot, "width"), height = prop(slot, "height");
        if (x.empty()) x = "0";
        if (y.empty()) y = "0";
        number(x); number(y); number(width, true); number(height, true);
        auto group = xmlNewNode(slot->ns, BAD_CAST "g");
        xmlSetProp(group, BAD_CAST "id", BAD_CAST asset.id.c_str());
        auto transform = prop(slot, "transform");
        if (!transform.empty()) xmlSetProp(group, BAD_CAST "transform", BAD_CAST transform.c_str());
        auto credit = asset.title + "\nSource: " + asset.source + "\nLicense: " + asset.license + "\nAttribution: " + asset.attribution;
        xmlNewTextChild(group, slot->ns, BAD_CAST "desc", BAD_CAST credit.c_str());
        xmlNode *image = nullptr;
        if (!asset.svg.empty()) {
            auto source = parse(Inkscape::AI::validate_generated_svg(asset.svg));
            image = xmlDocCopyNode(xmlDocGetRootElement(source.get()), doc.get(), 1);
            auto box = viewbox(image);
            double w = number(width, true), h = number(height, true);
            double scale = std::min(w / box[2], h / box[3]);
            double tx = number(x) + (w - box[2] * scale) / 2 - box[0] * scale;
            double ty = number(y) + (h - box[3] * scale) / 2 - box[1] * scale;
            std::ostringstream matrix;
            matrix.imbue(std::locale::classic());
            matrix.precision(17);
            matrix << "matrix(" << scale << " 0 0 " << scale << ' ' << tx << ' ' << ty << ')';
            auto placement = xmlNewChild(group, slot->ns, BAD_CAST "g", nullptr);
            xmlSetProp(placement, BAD_CAST "transform", BAD_CAST matrix.str().c_str());
            xmlNodeSetName(image, BAD_CAST "g");
            for (auto name : {"viewBox", "preserveAspectRatio", "width", "height", "x", "y", "version"}) xmlUnsetProp(image, BAD_CAST name);
            std::function<void(xmlNode *)> strip_ids = [&](xmlNode *node) {
                for (; node; node = node->next) { xmlUnsetProp(node, BAD_CAST "id"); strip_ids(node->children); }
            };
            strip_ids(image);
            xmlAddChild(placement, image);
        } else {
            image = xmlNewNode(slot->ns, BAD_CAST "image");
            g_autofree char *encoded = g_base64_encode(reinterpret_cast<guchar const *>(asset.png.data()), asset.png.size());
            auto uri = std::string("data:image/png;base64,") + encoded;
            auto ns = xmlNewNs(image, BAD_CAST "http://www.w3.org/1999/xlink", BAD_CAST "xlink");
            xmlSetNsProp(image, ns, BAD_CAST "href", BAD_CAST uri.c_str());
            xmlSetProp(image, BAD_CAST "x", BAD_CAST x.c_str()); xmlSetProp(image, BAD_CAST "y", BAD_CAST y.c_str());
            xmlSetProp(image, BAD_CAST "width", BAD_CAST width.c_str()); xmlSetProp(image, BAD_CAST "height", BAD_CAST height.c_str());
            xmlSetProp(image, BAD_CAST "preserveAspectRatio", BAD_CAST "xMidYMid meet");
            xmlAddChild(group, image);
        }
        xmlReplaceNode(slot, group); xmlFreeNode(slot);
    }
    xmlReconciliateNs(doc.get(), xmlDocGetRootElement(doc.get()));
    return serialize(doc.get());
}
}
