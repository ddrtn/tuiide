#include "tuiide/cmake_model.hpp"
#include "tuiide/gdb_mi.hpp"
#include "tuiide/json_rpc_framer.hpp"
#include "tuiide/json_utils.hpp"
#include "tuiide/text_display.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t seed = 0x5455494944452026ULL;

struct Random {
  std::uint64_t state{seed};
  auto next() -> std::uint64_t {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }
  auto below(std::size_t limit) -> std::size_t {
    return static_cast<std::size_t>(next() % limit);
  }
};

void expect(bool condition, std::string_view domain, std::size_t case_number) {
  if (condition) return;
  std::cerr << "FAIL: " << domain << " case=" << case_number << " seed=" << seed << '\n';
  std::exit(1);
}

struct TemporaryDirectory {
  std::filesystem::path path;
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

auto randomJson(Random& random, int depth) -> nlohmann::json {
  switch (random.below(depth == 0 ? 4 : 6)) {
    case 0: return nullptr;
    case 1: return static_cast<int>(random.below(1000));
    case 2: return random.below(2) == 0;
    case 3: return random.below(2) == 0 ? "данные" : "index-x.json";
    case 4: return nlohmann::json::array({randomJson(random, depth - 1)});
    default: return nlohmann::json{{"reply", randomJson(random, depth - 1)},
      {"configurations", randomJson(random, depth - 1)}};
  }
}

void writeJson(const std::filesystem::path& path, const nlohmann::json& value) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << value.dump();
}

void checkJsonRpc(Random& random) {
  tuiide::JsonRpcFramer framer;
  for (std::size_t test{}; test < 400; ++test) {
    const auto payload = nlohmann::json{{"jsonrpc", "2.0"}, {"id", test},
      {"result", std::string("UTF-8: данные ") + std::to_string(random.next())}}.dump();
    const auto frame = "Content-Length: " + std::to_string(payload.size())
      + "\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n" + payload;
    std::vector<std::string> received;
    for (std::size_t offset{}; offset < frame.size();) {
      const auto count = std::min<std::size_t>(frame.size() - offset, 1 + random.below(19));
      auto batch = framer.feed(std::string_view(frame).substr(offset, count));
      expect(batch.invalid_headers == 0, "JSON-RPC valid header", test);
      for (auto& item : batch.payloads) received.push_back(std::move(item));
      offset += count;
    }
    expect(received.size() == 1 && received.front() == payload && framer.pendingBytes() == 0,
      "JSON-RPC fragmented round trip", test);
  }
  constexpr std::string_view bad_headers[] = {
    "Content-Length: -1\r\n\r\n", "Content-Length: 99999999999999999999\r\n\r\n",
    "Content-Length: 16777217\r\n\r\n", "Content-Length: nope\r\n\r\n",
    "Content-Length: 1\r\nContent-Length: 1\r\n\r\n", "Other: 2\r\n\r\n"
  };
  for (std::size_t test{}; test < std::size(bad_headers); ++test) {
    framer.clear();
    const auto good = std::string("Content-Length: 2\r\n\r\n{}");
    const auto batch = framer.feed(std::string(bad_headers[test]) + good);
    expect(batch.invalid_headers == 1 && batch.payloads.size() == 1
        && batch.payloads.front() == "{}", "JSON-RPC invalid header recovery", test);
  }
  framer.clear();
  const auto oversized = framer.feed(std::string(tuiide::JsonRpcFramer::maxHeaderBytes + 1, 'x'));
  expect(oversized.invalid_headers == 1 && framer.pendingBytes() == 0,
    "JSON-RPC oversized header bound", 0);
  for (std::size_t test{}; test < 600; ++test) {
    std::string bytes(1 + random.below(96), '\0');
    for (char& byte : bytes) byte = static_cast<char>(random.next() & 0xffU);
    (void)framer.feed(bytes);
    expect(framer.pendingBytes() <= tuiide::JsonRpcFramer::maxHeaderBytes,
      "JSON-RPC random header bound", test);
  }
}

void checkGdbMi(Random& random) {
  for (std::size_t test{}; test < 800; ++test) {
    const auto value = "value_" + std::to_string(random.next());
    const auto record = tuiide::parseMiRecord(std::to_string(test) + "^done,name=\"" + value + "\"");
    expect(record.valid() && record.token == static_cast<int>(test)
        && record.string("name") == value, "GDB/MI generated round trip", test);
    std::string bytes(1 + random.below(96), '\0');
    for (char& byte : bytes) byte = static_cast<char>(random.next() & 0xffU);
    const auto malformed = tuiide::parseMiRecord(bytes);
    expect(!malformed.valid() || std::string_view("^*+=~@&").find(malformed.prefix) != std::string_view::npos,
      "GDB/MI random record safety", test);
  }
}

void checkCMakeFileApi(Random& random) {
  TemporaryDirectory temporary{std::filesystem::temp_directory_path()
    / ("tuiide-property-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()))};
  const auto reply = temporary.path / ".cmake/api/v1/reply";
  std::filesystem::create_directories(reply);
  const auto index = reply / "index-property.json";
  const auto codemodel = reply / "codemodel.json";
  const auto target = reply / "target.json";
  const nlohmann::json valid_index = {{"reply", {{"codemodel-v2", {{"jsonFile", "codemodel.json"}}}}}};
  const nlohmann::json valid_model = {{"paths", {{"source", temporary.path.string()}}},
    {"configurations", nlohmann::json::array({{{"name", "Debug"},
      {"targets", nlohmann::json::array({{{"jsonFile", "target.json"}}})}}})}};
  const nlohmann::json valid_target = {{"name", "sample"}, {"type", "EXECUTABLE"},
    {"artifacts", nlohmann::json::array({{{"path", "bin/sample"}}})},
    {"sources", nlohmann::json::array({{{"path", "main.cpp"}}})}};
  writeJson(index, valid_index); writeJson(codemodel, valid_model); writeJson(target, valid_target);
  std::string error;
  const auto targets = tuiide::loadCMakeExecutableTargets(temporary.path, error);
  expect(error.empty() && targets.size() == 1 && targets.front().name == "sample"
      && targets.front().sources.size() == 1
      && targets.front().sources.front() == temporary.path / "main.cpp",
    "CMake File API valid model", 0);
  for (std::size_t test{}; test < 180; ++test) {
    writeJson(index, test % 3 == 0 ? randomJson(random, 2) : valid_index);
    writeJson(codemodel, test % 3 == 1 ? randomJson(random, 2) : valid_model);
    writeJson(target, test % 3 == 2 ? randomJson(random, 2) : valid_target);
    error.clear();
    try {
      const auto found = tuiide::loadCMakeExecutableTargets(temporary.path, error);
      expect(found.size() <= 1 && (error.empty() || found.empty()),
        "CMake File API malformed reply invariant", test);
    } catch (const std::exception&) {
      expect(false, "CMake File API parser threw", test);
    }
  }
}

void checkMalformedUtf8(Random& random) {
  for (std::size_t test{}; test < 1000; ++test) {
    std::string bytes(random.below(100), '\0');
    for (char& byte : bytes) byte = static_cast<char>(random.next() & 0xffU);
    const auto units = tuiide::displayUnits(bytes, 1 + random.below(8));
    std::size_t offset{};
    std::size_t column{};
    for (const auto& unit : units) {
      expect(unit.byte_begin == offset && unit.byte_end > offset
          && unit.byte_end <= bytes.size() && unit.byte_end - offset <= 4
          && unit.column_begin == column && unit.column_end >= column,
        "UTF-8 byte and column partition", test);
      offset = unit.byte_end; column = unit.column_end;
    }
    expect(offset == bytes.size(), "UTF-8 complete byte coverage", test);
    for (std::size_t position{}; position <= bytes.size(); ++position)
      expect(tuiide::displayColumn(bytes, position) <= tuiide::displayWidth(bytes),
        "UTF-8 bounded display column", test);
    for (std::size_t position{}; position <= column; ++position) {
      const auto byte = tuiide::byteColumnAtDisplay(bytes, position);
      expect(byte <= bytes.size() && (byte == bytes.size()
          || std::any_of(units.begin(), units.end(), [byte](const auto& unit) {
            return unit.byte_begin == byte;
          })), "UTF-8 display-to-byte boundary", test);
    }
  }
}

}  // namespace

int main() {
  Random random;
  checkJsonRpc(random);
  checkGdbMi(random);
  checkCMakeFileApi(random);
  checkMalformedUtf8(random);
  std::cout << "Property tests passed (seed=" << seed << ")\n";
}
