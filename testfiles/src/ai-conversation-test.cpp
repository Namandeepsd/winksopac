// SPDX-License-Identifier: GPL-2.0-or-later
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <gtest/gtest.h>
#include "ui/dialog/ai-conversation.h"

using Inkscape::UI::Dialog::AIConversation;

TEST(AIConversation, StorageRoundTripAndRecovery)
{
    auto *directory = g_dir_make_tmp("inkscape-ai-history-test-XXXXXX", nullptr);
    ASSERT_NE(directory, nullptr);
    std::string root(directory);
    g_free(directory);
    g_setenv("XDG_DATA_HOME", root.c_str(), true);
    // Refuse to touch a real profile if another test initialized GLib's cache.
    ASSERT_EQ(std::string(g_get_user_data_dir()), root);
    auto first = root + "/first.svg";
    auto second = root + "/second.svg";
    AIConversation conversation;
    conversation.brief = "Navy and gold";
    conversation.draft = "Keep the shape";
    conversation.messages.push_back({"You", "Make a badge", "create", true});
    conversation.accepted.emplace_back("Make a badge", "<svg/>");
    conversation.artwork.push_back({"Make a badge", "<svg/>", "inserted"});
    conversation.save(first);
    ASSERT_TRUE(conversation.error.empty()) << conversation.error;

    AIConversation restored;
    restored.load(first);
    EXPECT_EQ(restored.brief, conversation.brief);
    EXPECT_EQ(restored.draft, conversation.draft);
    ASSERT_EQ(restored.messages.size(), 1u);
    EXPECT_TRUE(restored.messages.front().user);
    EXPECT_EQ(restored.accepted, conversation.accepted);
    ASSERT_EQ(restored.artwork.size(), 1u);
    EXPECT_EQ(restored.artwork.front().status, "inserted");
    EXPECT_EQ(AIConversation::history_path(first), AIConversation::history_path(root + "/./first.svg"));

    conversation.brief = "Separate Save As conversation";
    conversation.save(second);
    restored.load(first);
    EXPECT_EQ(restored.brief, "Navy and gold");
    restored.load(second);
    EXPECT_EQ(restored.brief, conversation.brief);

    auto path = AIConversation::history_path(first);
    { std::ofstream output(path); output << "corrupt history"; }
    restored.load(first);
    EXPECT_FALSE(restored.writable);
    EXPECT_FALSE(restored.error.empty());
    restored.save(first);
    { std::ifstream input(path); std::string text; std::getline(input, text); EXPECT_EQ(text, "corrupt history"); }
    restored = AIConversation{};
    restored.save(first);
    restored.load(first);
    EXPECT_TRUE(restored.writable);

    conversation.brief = std::string(8001, 'x');
    conversation.save(second);
    EXPECT_FALSE(conversation.error.empty());
    restored.load(second);
    EXPECT_EQ(restored.brief, "Separate Save As conversation");
    std::filesystem::remove_all(root);
}
