// SPDX-License-Identifier: GPL-2.0-or-later
#include "message-bus.h"
#include <algorithm>
#include <stdexcept>

namespace Inkscape::AI {
void MessageBus::register_tasks(std::vector<std::string> const &tasks)
{
    std::lock_guard lock(_mutex);
    auto next = _tasks;
    for (auto const &id : tasks) {
        require_id(id);
        if (!next.insert(id).second) throw std::runtime_error("Duplicate message endpoint");
    }
    _tasks.swap(next);
}
void MessageBus::send(std::string const &from, std::string const &to, MessageKind kind,
                      Json payload, std::string correlation)
{
    require_json(payload, 8192);
    if (correlation.size() > 128 || kind < MessageKind::InformationRequest || kind > MessageKind::Failed)
        throw std::invalid_argument("Invalid workflow message");
    std::lock_guard lock(_mutex);
    if (!_tasks.contains(from) || (to != "host" && !_tasks.contains(to))) throw std::runtime_error("Unknown message endpoint");
    auto count = std::count_if(_messages.begin(), _messages.end(), [&](auto const &m) { return m.from == from; });
    if (_messages.size() >= 4096 || count >= 64) throw std::runtime_error("Workflow message budget exceeded");
    _messages.push_back({_messages.size() + 1, kind, from, to, std::move(correlation), std::move(payload)});
    _ready.notify_all();
}
std::vector<Message> MessageBus::receive(std::string const &task, std::uint64_t after,
                                        Clock::time_point until, std::stop_token stop)
{
    std::unique_lock lock(_mutex);
    if (!_tasks.contains(task)) throw std::runtime_error("Unknown message receiver");
    auto available = [&] {
        return std::any_of(_messages.begin(), _messages.end(), [&](auto const &m) { return m.to == task && m.sequence > after; });
    };
    _ready.wait_until(lock, stop, until, available);
    std::vector<Message> result;
    for (auto const &message : _messages) {
        if (message.to == task && message.sequence > after) result.push_back(message);
    }
    return result;
}
std::vector<Message> MessageBus::snapshot() const
{
    std::lock_guard lock(_mutex);
    return _messages;
}
} // namespace Inkscape::AI
