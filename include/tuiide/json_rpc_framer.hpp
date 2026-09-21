#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

struct JsonRpcFrameBatch {
  std::vector<std::string> payloads;
  std::size_t invalid_headers{};
};

/** Собирает LSP/JSON-RPC фреймы из произвольно разбитого потока байтов. */
class JsonRpcFramer {
 public:
  static constexpr std::size_t maxHeaderBytes = 8192;
  static constexpr std::size_t maxPayloadBytes = 16 * 1024 * 1024;

  [[nodiscard]] auto feed(std::string_view bytes) -> JsonRpcFrameBatch;
  [[nodiscard]] auto pendingBytes() const -> std::size_t;
  void clear();

 private:
  void extract(JsonRpcFrameBatch& batch);
  std::string buffer_;
};

}  // namespace tuiide
