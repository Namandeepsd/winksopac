// SPDX-License-Identifier: GPL-2.0-or-later
#include "svg-validation.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <libxml/parser.h>
#include <libxml/tree.h>

namespace Inkscape::AI {
std::string validate_generated_svg(std::string svg)
{
    constexpr std::size_t MAX_BYTES = 512 * 1024;

    if (svg.empty() || svg.size() > MAX_BYTES) {
        throw std::runtime_error("SVG is empty or exceeds 512 KB.");
    }

    if (svg.find('\0') != std::string::npos) {
        throw std::runtime_error("SVG contains an invalid null character.");
    }

    // Some models return Markdown fences despite the instructions.
    auto first = svg.find_first_not_of(" \t\r\n");
    auto last = svg.find_last_not_of(" \t\r\n");

    if (first == std::string::npos) {
        throw std::runtime_error("The response is empty.");
    }

    svg = svg.substr(first, last - first + 1);

    if (svg.starts_with("```")) {
        auto newline = svg.find('\n');
        auto closing = svg.rfind("```");

        if (newline == std::string::npos || closing <= newline ||
            closing + 3 != svg.size()) {
            throw std::runtime_error("Incomplete Markdown code block.");
        }

        svg = svg.substr(newline + 1, closing - newline - 1);
    }

    // Do this before parsing. Generated artwork needs no DTD or entities.
    if (svg.find("<!DOCTYPE") != std::string::npos ||
        svg.find("<!ENTITY") != std::string::npos) {
        throw std::runtime_error("DOCTYPE and entity declarations are not allowed.");
    }

    using XmlDocument =
        std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

    XmlDocument parsed(
        xmlReadMemory(
            svg.data(),
            static_cast<int>(svg.size()),
            "generated.svg",
            "UTF-8",
            XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING
        ),
        &xmlFreeDoc
    );

    if (!parsed || parsed->intSubset || parsed->extSubset) {
        throw std::runtime_error("The response is not valid SVG XML.");
    }

    auto *root = xmlDocGetRootElement(parsed.get());

    if (!root || !xmlStrEqual(root->name, BAD_CAST "svg")) {
        throw std::runtime_error("The document must have an <svg> root.");
    }

    static const std::set<std::string> elements = {
        "svg", "g", "path", "rect", "circle", "ellipse",
        "line", "polyline", "polygon",
        "text", "tspan", "title", "desc"
    };

    static const std::set<std::string> attributes = {
        "id", "version", "viewBox", "preserveAspectRatio",
        "x", "y", "x1", "y1", "x2", "y2",
        "width", "height", "rx", "ry", "r", "cx", "cy",
        "d", "points", "transform",
        "fill", "fill-opacity", "fill-rule",
        "stroke", "stroke-width", "stroke-opacity",
        "stroke-linecap", "stroke-linejoin", "stroke-miterlimit",
        "stroke-dasharray", "stroke-dashoffset",
        "opacity", "vector-effect", "paint-order",
        "font-family", "font-size", "font-weight", "font-style",
        "text-anchor", "dominant-baseline",
        "dx", "dy", "rotate", "letter-spacing", "word-spacing",
        "textLength", "lengthAdjust"
    };

    std::size_t nodes = 0;

    std::function<void(xmlNode *, unsigned)> visit;
    visit = [&](xmlNode *node, unsigned depth) {
        if (depth > 64) {
            throw std::runtime_error("SVG nesting is too deep.");
        }

        for (; node; node = node->next) {
            if (++nodes > 5000) {
                throw std::runtime_error("SVG contains too many nodes.");
            }

            if (node->type == XML_COMMENT_NODE ||
                node->type == XML_TEXT_NODE ||
                node->type == XML_CDATA_SECTION_NODE) {
                continue;
            }

            if (node->type != XML_ELEMENT_NODE) {
                throw std::runtime_error(
                    "Unsupported XML instruction or entity."
                );
            }

            std::string tag(
                reinterpret_cast<char const *>(node->name)
            );

            if (!elements.contains(tag) ||
                !node->ns ||
                !xmlStrEqual(
                    node->ns->href,
                    BAD_CAST "http://www.w3.org/2000/svg"
                )) {
                throw std::runtime_error(
                    "Unsupported SVG element: " + tag
                );
            }

            for (auto *attr = node->properties; attr; attr = attr->next) {
                std::string name(
                    reinterpret_cast<char const *>(attr->name)
                );

                // Preserve whitespace in text when explicitly requested.
                bool xml_space =
                    attr->ns &&
                    xmlStrEqual(
                        attr->ns->href,
                        BAD_CAST "http://www.w3.org/XML/1998/namespace"
                    ) &&
                    name == "space";

                if (!xml_space &&
                    (attr->ns || !attributes.contains(name))) {
                    throw std::runtime_error(
                        "Unsupported SVG attribute: " + name
                    );
                }

                if (name == "fill" || name == "stroke") {
                    xmlChar *raw = xmlNodeListGetString(
                        parsed.get(), attr->children, 1
                    );

                    std::string value = raw
                        ? reinterpret_cast<char const *>(raw)
                        : "";

                    xmlFree(raw);

                    std::transform(
                        value.begin(), value.end(), value.begin(),
                        [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        }
                    );

                    // Paint servers and escaped CSS are outside this subset.
                    if (value.find("url") != std::string::npos ||
                        value.find('\\') != std::string::npos ||
                        value.find('/') != std::string::npos) {
                        throw std::runtime_error(
                            "Use solid fill and stroke colors for now."
                        );
                    }
                }
            }

            visit(node->children, depth + 1);
        }
    };

    visit(parsed->children, 0);

    // Import exactly the tree we checked, with a consistent encoding.
    xmlChar *serialized = nullptr;
    int length = 0;
    xmlDocDumpMemoryEnc(parsed.get(), &serialized, &length, "UTF-8");
    if (!serialized || length <= 0) {
        xmlFree(serialized);
        throw std::runtime_error("Could not serialize the validated SVG.");
    }
    std::string validated(reinterpret_cast<char const *>(serialized), length);
    xmlFree(serialized);
    return validated;
}

} // namespace Inkscape::AI
