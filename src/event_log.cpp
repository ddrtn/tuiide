#include "tuiide/event_log.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>

namespace tuiide {
namespace {
auto escaped(std::string_view value) -> std::string {
  std::string result;
  for (const char character : value) {
    if (character == '\\') result += "\\\\";
    else if (character == '\n') result += "\\n";
    else if (character == '\r') result += "\\r";
    else if (character == '\t') result += "\\t";
    else result += character;
  }
  return result;
}
}  // namespace

auto EventLog::openFile(const std::filesystem::path& path, std::string& error) -> bool {
  error.clear();
  file_.close();
  file_path_.clear();
  if (path.empty()) return true;
  std::error_code filesystem_error;
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    error = "cannot create log directory: " + filesystem_error.message();
    return false;
  }
  file_.open(path, std::ios::app);
  if (!file_) { error = "cannot open log file: " + path.string(); return false; }
  file_path_ = path;
  return true;
}

void EventLog::publish(EventChannel channel, EventSource source, EventSeverity severity,
    std::string message) {
  auto& destination = stream(channel);
  destination.text += message;
  destination.events.push_back({next_sequence_++, channel, source, severity, std::move(message)});
  if (file_) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_r(&now, &local);
    const auto& event = destination.events.back();
    file_ << std::put_time(&local, "%Y-%m-%dT%H:%M:%S%z") << '\t'
      << event.sequence << '\t' << eventChannelName(channel) << '\t'
      << eventSourceName(source) << '\t' << eventSeverityName(severity) << '\t'
      << escaped(event.message) << '\n';
    file_.flush();
  }
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

auto EventLog::filePath() const noexcept -> const std::filesystem::path& { return file_path_; }

auto eventChannelName(EventChannel channel) noexcept -> std::string_view {
  return channel == EventChannel::Build ? "build" : "output";
}

auto eventSourceName(EventSource source) noexcept -> std::string_view {
  switch (source) {
    case EventSource::Ide: return "ide";
    case EventSource::Project: return "project";
    case EventSource::Editor: return "editor";
    case EventSource::Build: return "build";
    case EventSource::Test: return "test";
    case EventSource::Run: return "run";
    case EventSource::Debug: return "debug";
    case EventSource::Lsp: return "lsp";
    case EventSource::System: return "system";
  }
  return "unknown";
}

auto eventSeverityName(EventSeverity severity) noexcept -> std::string_view {
  switch (severity) {
    case EventSeverity::Information: return "information";
    case EventSeverity::Success: return "success";
    case EventSeverity::Warning: return "warning";
    case EventSeverity::Error: return "error";
  }
  return "unknown";
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
