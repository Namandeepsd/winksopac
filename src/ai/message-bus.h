// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_AI_MESSAGE_BUS_H
#define INKSCAPE_AI_MESSAGE_BUS_H
#include "workflow-types.h"
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace Inkscape::AI {
// One bounded bus per workflow. Sender identity comes from the task channel,
// not from agent/model payload. Retries have distinct correlation IDs.
class MessageBus {
public:
    void register_tasks(std::vector<std::string> const &tasks);
    void send(std::string const &from, std::string const &to, MessageKind kind,
              Json payload, std::string correlation);
    std::vector<Message> receive(std::string const &task, std::uint64_t after,
                                 Clock::time_point until, std::stop_token stop);
    std::vector<Message> snapshot() const;
private:
    mutable std::mutex _mutex;
    std::condition_variable_any _ready;
    std::set<std::string> _tasks;
    std::vector<Message> _messages;
};
} // namespace Inkscape::AI
#endif
