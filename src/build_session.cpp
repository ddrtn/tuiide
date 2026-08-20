#include "tuiide/build_session.hpp"

#include <utility>

namespace tuiide {

void BuildSession::prepare() {
  process_.stop();
  (void)process_.drain();
  workflow_.reset();
  output_.reset();
}

void BuildSession::begin(BuildOperation operation, bool configured,
    BuildContinuation continuation) {
  workflow_.begin(operation, configured, continuation);
}

auto BuildSession::start(std::vector<std::string> arguments,
    const std::filesystem::path& working_directory,
    const std::map<std::string, std::string>& environment) -> bool {
  return process_.start(arguments, true, working_directory, environment);
}

auto BuildSession::drain(const std::filesystem::path& project_root)
    -> BuildDrainResult {
  BuildDrainResult result;
  result.output = process_.drain();
  for (const auto& chunk : result.output)
    result.diagnostics_added += ingest(chunk, project_root).diagnostics_added;
  return result;
}

auto BuildSession::poll(const std::filesystem::path& project_root) -> BuildPollResult {
  BuildPollResult result;
  auto drained = drain(project_root);
  result.output = std::move(drained.output);
  result.diagnostics_added = drained.diagnostics_added;
  const auto exit_code = process_.exitCode();
  if (!workflow_.running() || process_.running() || !exit_code) return result;
  result.completion = BuildStageCompletion{*exit_code, workflow_.stage(), workflow_.stageElapsed()};
  process_.stop();
  drained = drain(project_root);
  result.diagnostics_added += drained.diagnostics_added;
  for (auto& chunk : drained.output) result.output.push_back(std::move(chunk));
  return result;
}

auto BuildSession::ingest(std::string_view chunk,
    const std::filesystem::path& project_root) -> BuildOutputUpdate {
  auto update = output_.append(chunk, project_root);
  if (update.progress) workflow_.setProgress(update.progress);
  return update;
}

auto BuildSession::finish(const std::filesystem::path& project_root) -> BuildFinishResult {
  process_.stop();
  const auto final_output = output_.finish(project_root);
  if (final_output.progress) workflow_.setProgress(final_output.progress);
  BuildFinishResult result;
  result.diagnostics_added = final_output.diagnostics_added;
  result.elapsed = workflow_.operationElapsed();
  result.operation = workflow_.operationName();
  result.continuation = workflow_.takeContinuation();
  workflow_.reset();
  return result;
}

auto BuildSession::cancel(const std::filesystem::path& project_root) -> BuildCancelResult {
  BuildCancelResult result;
  result.stage = workflow_.stageName();
  result.elapsed = workflow_.operationElapsed();
  process_.stop();
  auto drained = drain(project_root);
  result.output = std::move(drained.output);
  result.diagnostics_added = drained.diagnostics_added;
  const auto final_output = output_.finish(project_root);
  result.diagnostics_added += final_output.diagnostics_added;
  workflow_.reset();
  return result;
}

void BuildSession::reset() {
  process_.stop();
  (void)process_.drain();
  workflow_.reset();
  output_.reset();
}

}  // namespace tuiide
