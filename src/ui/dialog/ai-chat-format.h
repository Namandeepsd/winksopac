// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_AI_CHAT_FORMAT_H
#define INKSCAPE_UI_DIALOG_AI_CHAT_FORMAT_H
#include <glib.h>
#include <string>
#include <string_view>
#include <sstream>
namespace Inkscape::UI::Dialog::AIChat {
inline std::string escape(std::string_view text)
{
    g_autofree char *escaped = g_markup_escape_text(text.data(), text.size());
    return escaped ? escaped : "";
}
// Deliberately small Markdown subset. All model/user text is escaped before
// becoming Pango markup; HTML and executable/external content remain plain text.
inline std::string inline_markup(std::string_view text)
{
    std::string output;
    for (std::size_t i = 0; i < text.size();) {
        std::string_view marker, open, close;
        if (text.substr(i, 2) == "**") { marker = "**"; open = "<b>"; close = "</b>"; }
        else if (text[i] == '`') { marker = "`"; open = "<tt>"; close = "</tt>"; }
        else if (text[i] == '*') { marker = "*"; open = "<i>"; close = "</i>"; }
        if (!marker.empty()) {
            auto end = text.find(marker, i + marker.size());
            if (end != std::string_view::npos && end > i + marker.size()) {
                output += open;
                output += escape(text.substr(i + marker.size(), end - i - marker.size()));
                output += close;
                i = end + marker.size();
                continue;
            }
        }
        auto next = text.find_first_of("*`", i + 1);
        if (next == std::string_view::npos) next = text.size();
        output += escape(text.substr(i, next - i));
        i = next;
    }
    return output;
}
inline std::string markup(std::string const &text)
{
    std::istringstream input(text);
    std::string line, output;
    bool code = false, first = true;
    while (std::getline(input, line)) {
        if (line.starts_with("```")) {
            if (code) output += "</tt>\n";
            else { if (!first) output += "\n"; output += "<tt>"; }
            code = !code;
            first = true;
            continue;
        }
        if (!first) output += "\n";
        first = false;
        if (code) { output += escape(line); continue; }
        auto start = line.find_first_not_of('#');
        if (start > 0 && start <= 3 && start < line.size() && line[start] == ' ')
            output += "<b>" + inline_markup(std::string_view(line).substr(start + 1)) + "</b>";
        else if (line.starts_with("- ") || line.starts_with("* "))
            output += "• " + inline_markup(std::string_view(line).substr(2));
        else output += inline_markup(line);
    }
    if (code) output += "</tt>";
    return output;
}
}
#endif
