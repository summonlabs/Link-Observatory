// Link Observatory - deterministic explanations.
//
// An explanation is a byte string that states exactly which evidence was
// considered, how old it was, which rules fired and why the reported state was
// selected. For identical inputs the bytes are identical, across runs, processes
// and machines. Nothing in the output depends on addresses, locale, wall-clock
// formatting or hash-map iteration order.

#pragma once

#include <string>

#include "linkobs/classify/classifier.hpp"

namespace linkobs {

struct ExplainOptions {
  bool include_families{true};
  bool include_rules{true};
  bool include_last_known{true};
};

/// Renders a classification as canonical text. Every line is "key: value" with a
/// single space, and the field order is fixed.
[[nodiscard]] std::string explain(const Classification& classification,
                                  const ExplainOptions& options);

/// Renders the classification as one deterministic line, suitable for logs and
/// for equality assertions.
[[nodiscard]] std::string summarize(const Classification& classification);

}  // namespace linkobs
