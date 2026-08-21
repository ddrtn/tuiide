#include "tuiide/event_log.hpp"

#include <utility>

namespace tuiide {

void EventLog::publish(EventChannel channel, EventSource source, EventSeverity severity,
    std::string message) {
  auto& destination = stream(channel);
  destination.text += message;
  destination.events.push_back({next_sequence_++, channel, source, severity, std::move(message)});
  ++destination.revision;
  if (channel == EventChannel::Build) trim(destination, 300000, 240000);
  else trim(destination, 150000, 120000);
}

void EventLog::clear(EventChannel channel) {
  auto& destination = stream(channel);
  if (destination.text.empty() && destination.events.empty()) return;
  destination.text.clear();
  destination.events.clear();
  ++destination.revision;
}

void EventLog::clear() {
  clear(EventChannel::Output);
  clear(EventChannel::Build);
}

auto EventLog::text(EventChannel channel) const noexcept -> const std::string& {
  return stream(channel).text;
}

auto EventLog::events(EventChannel channel) const noexcept -> const std::deque<IdeEvent>& {
  return stream(channel).events;
}

auto EventLog::revision(EventChannel channel) const noexcept -> std::uint64_t {
  return stream(channel).revision;
}

auto EventLog::stream(EventChannel channel) noexcept -> Stream& {
  return channel == EventChannel::Build ? build_ : output_;
}

auto EventLog::stream(EventChannel channel) const noexcept -> const Stream& {
  return channel == EventChannel::Build ? build_ : output_;
}

void EventLog::trim(Stream& destination, std::size_t maximum_text,
    std::size_t retained_text) {
  if (destination.text.size() <= maximum_text) return;
  const auto removed = destination.text.size() - retained_text;
  destination.text.erase(0, removed);
  std::size_t consumed{};
  while (!destination.events.empty()
      && consumed + destination.events.front().message.size() <= removed) {
    consumed += destination.events.front().message.size();
    destination.events.pop_front();
  }
  if (!destination.events.empty() && consumed < removed) {
    auto& first = destination.events.front().message;
    first.erase(0, removed - consumed);
  }
}

}  // namespace tuiide
