// Link Observatory - test harness.
//
// Deliberately tiny and deterministic. There is no timeout, no fixture magic, no
// hidden global state and no dependency on a third-party framework: a test is a
// function, a check is a boolean, and the exit code is the result.
//
// The harness never waits for a condition with a deadline. Tests that need
// concurrency use explicit handshakes; tests that need a second process use a
// bounded readiness poll whose bound is a liveness guard, not a correctness
// assumption.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace linkobs::test {

using TestFunction = void (*)();

struct TestCase {
  std::string_view suite{};
  std::string_view name{};
  TestFunction function{nullptr};
};

class Registrar {
 public:
  Registrar(std::string_view suite, std::string_view name, TestFunction function);
};

/// Registers a test. Called by the LO_TEST macro through a static Registrar.
void register_test(std::string_view suite, std::string_view name, TestFunction function);

/// Records a check. Returns the condition so callers can branch.
bool check(bool condition, std::string_view expression, const char* file, int line);

/// Records a failed check with a formatted message.
void fail(std::string_view message, const char* file, int line);

/// True when the currently running test has already failed.
[[nodiscard]] bool current_test_failed();

/// Runs every registered test, or the ones matching the filters.
/// Returns the process exit code: 0 when everything passed.
int run_all(const std::vector<std::string>& filters);

[[nodiscard]] std::string format_message(std::string_view text, std::uint64_t value);

}  // namespace linkobs::test

#define LO_TEST(suite_name, test_name)                                                     \
  static void lo_test_##suite_name##_##test_name();                                        \
  static const ::linkobs::test::Registrar lo_registrar_##suite_name##_##test_name{         \
      #suite_name, #test_name, &lo_test_##suite_name##_##test_name};                       \
  static void lo_test_##suite_name##_##test_name()

#define LO_CHECK(expression) \
  (void)::linkobs::test::check((expression), #expression, __FILE__, __LINE__)

#define LO_CHECK_MSG(expression, message)                                  \
  do {                                                                     \
    if (!::linkobs::test::check((expression), #expression, __FILE__, __LINE__)) { \
      ::linkobs::test::fail((message), __FILE__, __LINE__);                \
    }                                                                      \
  } while (false)

#define LO_REQUIRE(expression)                                        \
  do {                                                                \
    if (!::linkobs::test::check((expression), #expression, __FILE__, __LINE__)) { \
      return;                                                         \
    }                                                                 \
  } while (false)

#define LO_CHECK_EQ(lhs, rhs) LO_CHECK((lhs) == (rhs))
