#include "tuiide/json_rpc_framer.hpp"

#include <algorithm>
#include <charconv>
#include <optional>

namespace tuiide {
namespace {
auto contentLength(std::string_view header) -> std::optional<std::size_t> {
  std::optional<std::size_t> length;
  while (!header.empty()) {
    const auto end = header.find("\r\n");
    auto line = header.substr(0, end);
    header = end == std::string_view::npos ? std::string_view{} : header.substr(end + 2);
    constexpr std::string_view name = "Content-Length:";
    if (!line.starts_with(name)) continue;
    if (length) return std::nullopt;
    line.remove_prefix(name.size());
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos) return std::nullopt;
    line.remove_prefix(first);
    const auto last = line.find_last_not_of(" \t");
    line = line.substr(0, last + 1);
    std::size_t parsed_length{};
    const auto parsed = std::from_chars(line.data(), line.data() + line.size(), parsed_length);
    if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size()
        || parsed_length > JsonRpcFramer::maxPayloadBytes) return std::nullopt;
    length = parsed_length;
  }
  return length;
}
}  // namespace

auto JsonRpcFramer::feed(std::string_view bytes) -> JsonRpcFrameBatch {
  JsonRpcFrameBatch batch;
  // Не копируем большой вход целиком: после каждого блока разбираем готовые фреймы.
  while (!bytes.empty()) {
    const auto count = std::min<std::size_t>(bytes.size(), 64 * 1024);
    buffer_.append(bytes.substr(0, count));
    bytes.remove_prefix(count);
    extract(batch);
  }
  return batch;
}

void JsonRpcFramer::extract(JsonRpcFrameBatch& batch) {
  for (;;) {
    const auto header_end = buffer_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      if (buffer_.size() > maxHeaderBytes) { buffer_.clear(); ++batch.invalid_headers; }
      return;
    }
    if (header_end > maxHeaderBytes) {
      buffer_.erase(0, header_end + 4); ++batch.invalid_headers; continue;
    }
    const auto length = contentLength(std::string_view(buffer_).substr(0, header_end));
    if (!length) {
      buffer_.erase(0, header_end + 4); ++batch.invalid_headers; continue;
    }
    if (buffer_.size() - header_end - 4 < *length) return;
    batch.payloads.push_back(buffer_.substr(header_end + 4, *length));
    buffer_.erase(0, header_end + 4 + *length);
  }
}

auto JsonRpcFramer::pendingBytes() const -> std::size_t { return buffer_.size(); }
void JsonRpcFramer::clear() { buffer_.clear(); }

}  // namespace tuiide
