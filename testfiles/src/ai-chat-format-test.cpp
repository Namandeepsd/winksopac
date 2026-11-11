// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <stdexcept>
#include <pango/pango.h>
#include "ui/dialog/ai-chat-format.h"
int main() {
    try {
        for (auto text : {"# Heading\n**Bold** and *italic* with `code`.",
            "- First\n- Second", "```xml\n<svg onload='x'>& test</svg>\n```\nAfter",
            "A <span foreground='red'>literal</span> & text", "Unclosed **bold and `code",
            "नमस्ते · 你好 · 🎨", "```\n<unclosed & block"}) {
            auto markup=Inkscape::UI::Dialog::AIChat::markup(text);
            g_autoptr(GError) error=nullptr;
            g_autofree char *plain=nullptr;
            if(!pango_parse_markup(markup.c_str(),-1,0,nullptr,&plain,nullptr,&error)) throw std::runtime_error(error->message);
        }
        auto literal=Inkscape::UI::Dialog::AIChat::markup("<a href='file:///tmp'>test</a>");
        if(literal.find("<a ")!=std::string::npos) throw std::runtime_error("Untrusted markup was interpreted");
        auto block=Inkscape::UI::Dialog::AIChat::markup("```\n<svg>\n```\nAfter");
        if(block.find("</tt>\nAfter")==std::string::npos) throw std::runtime_error("Code block merged into following paragraph");
        std::cout<<"PASS: Markdown subset, Unicode, escaped markup and incomplete formatting\n";
    } catch(std::exception const &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
