#pragma once

#include "tuiide/build_diagnostic.hpp"
#include "tuiide/build_output_collector.hpp"
#include "tuiide/build_workflow.hpp"
#include "tuiide/process.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

struct BuildFinishResult {
  std::size_t diagnostics_added{};
  double elapsed{};
  std::string operation;
  BuildContinuation continuation{BuildContinuation::None};
};

struct BuildDrainResult {
  std::vector<std::string> output;
  std::size_t diagnostics_added{};
};

struct BuildStageCompletion {
  int exit_code{};
  BuildStage stage{BuildStage::Idle};
  double elapsed{};
};

/** Данные одного poll: новый вывод и, при завершении, статус стадии. */
struct BuildPollResult : BuildDrainResult {
  std::optional<BuildStageCompletion> completion;
};

struct BuildCancelResult {
  std::vector<std::string> output;
  std::size_t diagnostics_added{};
  double elapsed{};
  std::string stage;
};

/**
 * Координирует процесс CMake, BuildWorkflow и BuildOutputCollector.
 * Владелец гарантирует финальный drain вывода при успехе, ошибке и отмене.
 */
class BuildSession {
 public:
  /** Очищает остатки предыдущей операции перед началом новой. */
  void prepare();
  void begin(BuildOperation operation, bool configured,
    BuildContinuation continuation = BuildContinuation::None);
  void enterStage(BuildStage stage) noexcept { workflow_.enterStage(stage); }
  [[nodiscard]] auto start(std::vector<std::string> arguments,
    const std::filesystem::path& working_directory,
    const std::map<std::string, std::string>& environment) -> bool;
  [[nodiscard]] auto drain(const std::filesystem::path& project_root) -> BuildDrainResult;
  [[nodiscard]] auto poll(const std::filesystem::path& project_root) -> BuildPollResult;
  [[nodiscard]] auto ingest(std::string_view chunk,
    const std::filesystem::path& project_root) -> BuildOutputUpdate;
  [[nodiscard]] auto finish(const std::filesystem::path& project_root) -> BuildFinishResult;
  /** Останавливает группу процессов и возвращает ещё не показанный вывод. */
  [[nodiscard]] auto cancel(const std::filesystem::path& project_root) -> BuildCancelResult;
  void reset();

  [[nodiscard]] auto nextStageAfterSuccess() -> BuildStage {
    return workflow_.nextStageAfterSuccess();
  }
  [[nodiscard]] auto stage() const noexcept -> BuildStage { return workflow_.stage(); }
  [[nodiscard]] auto running() const noexcept -> bool { return workflow_.running(); }
  [[nodiscard]] auto continuation() const noexcept -> BuildContinuation {
    return workflow_.continuation();
  }
  [[nodiscard]] auto progress() const noexcept -> std::optional<unsigned> {
    return workflow_.progress();
  }
  [[nodiscard]] auto operationElapsed() const noexcept -> double {
    return workflow_.operationElapsed();
  }
  [[nodiscard]] auto stageElapsed() const noexcept -> double { return workflow_.stageElapsed(); }
  [[nodiscard]] auto operationName() const noexcept -> std::string_view {
    return workflow_.operationName();
  }
  [[nodiscard]] auto stageName() const noexcept -> std::string_view {
    return workflow_.stageName();
  }
  [[nodiscard]] auto diagnostics() const noexcept -> const std::vector<BuildDiagnostic>& {
    return output_.diagnostics();
  }
  void clearDiagnostics() { output_.clearDiagnostics(); }

 private:
  AsyncProcess process_;
  BuildWorkflow workflow_;
  BuildOutputCollector output_;
};

}  // namespace tuiide
