// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_AI_CONVERSATION_H
#define INKSCAPE_UI_DIALOG_AI_CONVERSATION_H
#include <string>
#include <utility>
#include <vector>
namespace Inkscape::UI::Dialog {
// Credentials and provider configuration deliberately never enter this model.
struct AIConversation {
    struct Message { std::string role, text, mode; bool user = false; };
    struct Artwork { std::string prompt, svg, status; };
    std::vector<Artwork> artwork;
    std::string brief, draft;
    std::string mode = "create";
    std::vector<Message> messages;
    std::vector<std::pair<std::string, std::string>> accepted;
    bool writable = true;
    std::string error;
    void load(std::string const &document);
    void save(std::string const &document);
    static std::string history_path(std::string const &document);
};
}
#endif
