#include "tuiide/build_workflow.hpp"

namespace tuiide {

void BuildWorkflow::begin(BuildOperation operation, bool configured,
    BuildContinuation continuation) {
  operation_ = operation;
  continuation_ = continuation;
  operation_started_ = std::chrono::steady_clock::now();
  stage_started_ = operation_started_;
  progress_.reset();
  switch (operation) {
    case BuildOperation::Configure: stage_ = BuildStage::Configure; break;
    case BuildOperation::Build: stage_ = configured ? BuildStage::Build : BuildStage::Configure; break;
    case BuildOperation::Rebuild: stage_ = configured ? BuildStage::Clean : BuildStage::Configure; break;
    case BuildOperation::Clean: stage_ = configured ? BuildStage::Clean : BuildStage::Idle; break;
    case BuildOperation::None: stage_ = BuildStage::Idle; break;
  }
}

auto BuildWorkflow::takeContinuation() noexcept -> BuildContinuation {
  const auto result = continuation_;
  continuation_ = BuildContinuation::None;
  return result;
}

void BuildWorkflow::enterStage(BuildStage stage) noexcept {
  stage_ = stage;
  stage_started_ = std::chrono::steady_clock::now();
  progress_.reset();
}

auto BuildWorkflow::nextStageAfterSuccess() -> BuildStage {
  if (stage_ == BuildStage::Configure && operation_ != BuildOperation::Configure) {
    stage_ = BuildStage::Build;
    return stage_;
  }
  if (stage_ == BuildStage::Clean && operation_ == BuildOperation::Rebuild) {
    stage_ = BuildStage::Build;
    return stage_;
  }
  stage_ = BuildStage::Idle;
  return stage_;
}

void BuildWorkflow::reset() noexcept {
  operation_ = BuildOperation::None;
  stage_ = BuildStage::Idle;
  continuation_ = BuildContinuation::None;
  operation_started_ = {};
  stage_started_ = {};
  progress_.reset();
}

auto BuildWorkflow::operationElapsed() const noexcept -> double {
  if (operation_started_ == std::chrono::steady_clock::time_point{}) return 0.0;
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - operation_started_).count();
}

auto BuildWorkflow::stageElapsed() const noexcept -> double {
  if (stage_started_ == std::chrono::steady_clock::time_point{}) return 0.0;
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - stage_started_).count();
}

auto BuildWorkflow::operationName() const noexcept -> std::string_view {
  switch (operation_) {
    case BuildOperation::Configure: return "Configure";
    case BuildOperation::Build: return "Build";
    case BuildOperation::Rebuild: return "Rebuild";
    case BuildOperation::Clean: return "Clean";
    case BuildOperation::None: return "CMake operation";
  }
  return "CMake operation";
}

auto BuildWorkflow::stageName() const noexcept -> std::string_view {
  switch (stage_) {
    case BuildStage::Configure: return "Configure";
    case BuildStage::Clean: return "Clean";
    case BuildStage::Build: return "Build";
    case BuildStage::Idle: return "Idle";
  }
  return "Idle";
}

}  // namespace tuiide
