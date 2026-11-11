// SPDX-License-Identifier: GPL-2.0-or-later
#include "ai-conversation.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <glib.h>
#include <glib/gstdio.h>
#include <nlohmann/json.hpp>
namespace Inkscape::UI::Dialog {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_file = 2 * 1024 * 1024;
std::string identity(std::string const &path) {
    return std::filesystem::weakly_canonical(std::filesystem::absolute(path)).string();
}
std::string field(Json const &j, char const *name, std::size_t limit) {
    auto s = j.at(name).get<std::string>();
    if (s.size() > limit || s.find('\0') != std::string::npos || !g_utf8_validate(s.data(), s.size(), nullptr))
        throw std::runtime_error("Invalid history text");
    return s;
}
}
std::string AIConversation::history_path(std::string const &document) {
    auto canonical = identity(document);
    auto hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, canonical.c_str(), canonical.size());
    auto path = std::filesystem::path(g_get_user_data_dir()) / "inkscape" / "ai-history" / (std::string(hash) + ".json");
    g_free(hash);
    return path.string();
}
void AIConversation::load(std::string const &document) {
    *this = AIConversation{};
    if (document.empty()) return;
    try {
        auto path = history_path(document);
        if (!std::filesystem::exists(path)) return;
        if (std::filesystem::file_size(path) > max_file) throw std::runtime_error("History file too large");
        std::ifstream input(path);
        auto j = Json::parse(input);
        if (j.at("version") != 1 || field(j, "document", 32768) != identity(document))
            throw std::runtime_error("Unsupported history version or document");
        AIConversation next;
        next.brief = field(j, "brief", 8000);
        next.draft = field(j, "draft", 16000);
        next.mode = j.value("mode", std::string("create"));
        if (next.mode != "create" && next.mode != "edit" && next.mode != "region" && next.mode != "geometry" && next.mode != "commands" && next.mode != "workflow") throw std::runtime_error("Invalid conversation mode");
        auto const &messages = j.at("messages");
        auto const &accepted = j.at("accepted");
        if (!messages.is_array() || messages.size() > 40 || !accepted.is_array() || accepted.size() > 3)
            throw std::runtime_error("Invalid history entries");
        for (auto const &m : messages)
            next.messages.push_back({field(m,"role",256),field(m,"text",64000),field(m,"mode",16),m.at("user").get<bool>()});
        std::size_t bytes = 0;
        for (auto const &a : accepted) {
            auto prompt = field(a,"prompt",24000), svg = field(a,"svg",24000);
            bytes += prompt.size() + svg.size();
            if (bytes > 24000) throw std::runtime_error("Context exceeds budget");
            next.accepted.emplace_back(std::move(prompt),std::move(svg));
        }
        if (j.contains("artwork")) {
            if (!j["artwork"].is_array() || j["artwork"].size() > 3) throw std::runtime_error("Invalid artwork history");
            for (auto const &a : j["artwork"])
                next.artwork.push_back({field(a,"prompt",16000), field(a,"svg",512*1024), field(a,"status",32)});
        }
        *this = std::move(next);
    } catch (std::exception const &e) {
        writable = false; // Preserve damaged/unknown files until explicit New conversation.
        error = std::string("History could not be loaded; original file preserved: ") + e.what();
    }
}
void AIConversation::save(std::string const &document) {
    if (document.empty() || !writable) return;
    try {
        if (brief.size() > 8000 || draft.size() > 16000) throw std::runtime_error("Shorten brief/message to save history");
        Json j = {{"version",1},{"document",identity(document)},{"brief",brief},{"draft",draft},{"mode",mode},
                  {"artwork",Json::array()},{"updated",g_get_real_time()},{"messages",Json::array()},{"accepted",Json::array()}};
        for (auto const &m : messages) j["messages"].push_back({{"role",m.role},{"text",m.text},{"mode",m.mode},{"user",m.user}});
        for (auto const &[p,s] : accepted) j["accepted"].push_back({{"prompt",p},{"svg",s}});
        for (auto const &a : artwork) j["artwork"].push_back({{"prompt",a.prompt},{"svg",a.svg},{"status",a.status}});
        auto data = j.dump();
        if (data.size() > max_file) throw std::runtime_error("History exceeds storage limit");
        auto path = history_path(document);
        if (g_mkdir_with_parents(std::filesystem::path(path).parent_path().c_str(),0700) != 0)
            throw std::runtime_error("Cannot create history directory");
        GError *failure = nullptr;
        if (!g_file_set_contents_full(path.c_str(),data.data(),data.size(),
            static_cast<GFileSetContentsFlags>(G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE),0600,&failure)) {
            std::string reason = failure->message;
            g_error_free(failure);
            throw std::runtime_error(reason);
        }
        error.clear();
    } catch (std::exception const &e) { error = std::string("History not saved: ") + e.what(); }
}
}
