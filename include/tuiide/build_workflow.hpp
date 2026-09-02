#pragma once

#include <chrono>
#include <optional>
#include <string_view>

namespace tuiide {

/** Намерение пользователя; одна операция может состоять из нескольких стадий CMake. */
enum class BuildOperation { None, Configure, Build, Rebuild, Clean };
enum class BuildStage { Idle, Configure, Clean, Build };
enum class BuildContinuation { None, Run, Debug };

/**
 * Чистая машина состояний сборки без процессов и UI.
 * Определяет цепочки Configure→Build и Clean→Build, хранит progress и отложенный
 * запуск Run/Debug, который разрешён только после успешной сборки.
 */
class BuildWorkflow {
 public:
  void begin(BuildOperation operation, bool configured,
    BuildContinuation continuation = BuildContinuation::None);
  void enterStage(BuildStage stage) noexcept;
  void setProgress(std::optional<unsigned> progress) noexcept { progress_ = progress; }
  /** Возвращает следующую стадию либо Idle, если сценарий завершён. */
  [[nodiscard]] auto nextStageAfterSuccess() -> BuildStage;
  [[nodiscard]] auto takeContinuation() noexcept -> BuildContinuation;
  void reset() noexcept;

  [[nodiscard]] auto operation() const noexcept -> BuildOperation { return operation_; }
  [[nodiscard]] auto stage() const noexcept -> BuildStage { return stage_; }
  [[nodiscard]] auto running() const noexcept -> bool { return stage_ != BuildStage::Idle; }
  [[nodiscard]] auto continuation() const noexcept -> BuildContinuation { return continuation_; }
  [[nodiscard]] auto progress() const noexcept -> std::optional<unsigned> { return progress_; }
  [[nodiscard]] auto operationElapsed() const noexcept -> double;
  [[nodiscard]] auto stageElapsed() const noexcept -> double;
  [[nodiscard]] auto operationName() const noexcept -> std::string_view;
  [[nodiscard]] auto stageName() const noexcept -> std::string_view;

 private:
  BuildOperation operation_{BuildOperation::None};
  BuildStage stage_{BuildStage::Idle};
  BuildContinuation continuation_{BuildContinuation::None};
  std::chrono::steady_clock::time_point operation_started_{};
  std::chrono::steady_clock::time_point stage_started_{};
  std::optional<unsigned> progress_;
};

}  // namespace tuiide
