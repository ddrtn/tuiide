#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace tuiide {

enum class EventChannel { Output, Build };
enum class EventSource { Ide, Project, Editor, Build, Run, Debug, Lsp, System };
enum class EventSeverity { Information, Success, Warning, Error };

struct IdeEvent {
  std::uint64_t sequence{};
  EventChannel channel{EventChannel::Output};
  EventSource source{EventSource::Ide};
  EventSeverity severity{EventSeverity::Information};
  std::string message;
};

class EventLog {
 public:
  auto openFile(const std::filesystem::path& path, std::string& error) -> bool;
  void publish(EventChannel channel, EventSource source, EventSeverity severity,
    std::string message);
  void clear(EventChannel channel);
  void clear();

  [[nodiscard]] auto text(EventChannel channel) const noexcept -> const std::string&;
  [[nodiscard]] auto events(EventChannel channel) const noexcept -> const std::deque<IdeEvent>&;
  [[nodiscard]] auto revision(EventChannel channel) const noexcept -> std::uint64_t;
  [[nodiscard]] auto filePath() const noexcept -> const std::filesystem::path&;

 private:
  struct Stream {
    std::string text;
    std::deque<IdeEvent> events;
    std::uint64_t revision{};
  };

  [[nodiscard]] auto stream(EventChannel channel) noexcept -> Stream&;
  [[nodiscard]] auto stream(EventChannel channel) const noexcept -> const Stream&;
  void trim(Stream& stream, std::size_t maximum_text, std::size_t retained_text);

  Stream output_;
  Stream build_;
  std::uint64_t next_sequence_{1};
  std::filesystem::path file_path_;
  std::ofstream file_;
};

[[nodiscard]] auto eventChannelName(EventChannel channel) noexcept -> std::string_view;
[[nodiscard]] auto eventSourceName(EventSource source) noexcept -> std::string_view;
[[nodiscard]] auto eventSeverityName(EventSeverity severity) noexcept -> std::string_view;

}  // namespace tuiide
