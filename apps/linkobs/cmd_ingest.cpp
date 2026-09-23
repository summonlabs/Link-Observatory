#include <iostream>

#include "common.hpp"

#include "linkobs/ingest/codec.hpp"

namespace linkobs::cli {

int run_ingest(const Context& context) {
  int exit_code = static_cast<int>(ExitCode::Ok);
  const std::string db = require_db(context, exit_code);
  if (exit_code != static_cast<int>(ExitCode::Ok)) {
    return exit_code;
  }
  const bool persist = !context.options.has("no-persist");

  const std::string input_path = context.options.get_or("input", "-");
  const std::optional<std::string> text = read_text_input(input_path, kMaxInputBytes);
  if (!text.has_value()) {
    std::cerr << "error: input could not be read or exceeds " << kMaxInputBytes << " bytes\n";
    return static_cast<int>(ExitCode::Io);
  }

  RuntimeConfig config = make_config(context, db, true, persist);
  config.workers = context.options.has("workers")
                       ? static_cast<std::size_t>(context.options.get_u64("workers", 1U))
                       : 0U;
  Observatory observatory(std::move(config), make_clock(context));

  const Status started = observatory.start();
  if (!started.ok() && started.code() != StatusCode::NotFound) {
    std::cerr << "error: " << started.to_string() << "\n";
    return exit_code_for(started.code());
  }

  ParseOutcome outcome = parse_batch(*text, observatory.config().policy);
  std::size_t submitted = 0;
  if (outcome.ok()) {
    submitted = outcome.batch.records.size();
    const Status applied = observatory.submit_batch(std::move(outcome.batch));
    if (!applied.ok()) {
      std::cerr << "error: " << applied.to_string() << "\n";
      const Status stopped = observatory.stop();
      (void)stopped;
      return exit_code_for(applied.code());
    }
  }

  const Status stopped = observatory.stop();
  const RuntimeStatistics stats = observatory.statistics();

  std::cout << "records=" << submitted << " rejected=" << outcome.rejected
            << " accepted=" << stats.records_accepted << " fenced=" << stats.records_fenced
            << " declarations=" << stats.declarations_applied
            << " state-changes=" << stats.state_changes << "\n";
  for (const std::string& error : outcome.errors) {
    std::cerr << "rejected " << error << "\n";
  }
  if (!stopped.ok()) {
    std::cerr << "error: " << stopped.to_string() << "\n";
    return exit_code_for(stopped.code());
  }
  if (outcome.rejected != 0U) {
    return static_cast<int>(ExitCode::Data);
  }
  return static_cast<int>(ExitCode::Ok);
}

}  // namespace linkobs::cli
