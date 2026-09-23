#include "harness.hpp"

#include <algorithm>
#include <iostream>
#include <string>

#include "linkobs/core/text.hpp"

namespace linkobs::test {
namespace {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> instance;
  return instance;
}

bool& current_failure() {
  static bool failed = false;
  return failed;
}

std::vector<std::string>& current_diagnostics() {
  static std::vector<std::string> instance;
  return instance;
}

int& current_line() {
  static int line = 0;
  return line;
}

std::string& current_file() {
  static std::string file;
  return file;
}

}  // namespace

Registrar::Registrar(std::string_view suite, std::string_view name, TestFunction function) {
  register_test(suite, name, function);
}

void register_test(std::string_view suite, std::string_view name, TestFunction function) {
  TestCase test{};
  test.suite = suite;
  test.name = name;
  test.function = function;
  registry().push_back(test);
}

bool check(bool condition, std::string_view expression, const char* file, int line) {
  if (condition) {
    return true;
  }
  current_failure() = true;
  current_file() = file;
  current_line() = line;
  std::string message{"  check failed: "};
  message.append(expression);
  current_diagnostics().push_back(std::move(message));
  return false;
}

void fail(std::string_view message, const char* file, int line) {
  current_failure() = true;
  current_file() = file;
  current_line() = line;
  std::string text{"  failure: "};
  text.append(message);
  current_diagnostics().push_back(std::move(text));
}

bool current_test_failed() { return current_failure(); }

std::string format_message(std::string_view text, std::uint64_t value) {
  std::string out{text};
  out.append(to_dec(value));
  return out;
}

int run_all(const std::vector<std::string>& filters) {
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;

  for (const TestCase& test : registry()) {
    std::string full{test.suite};
    full.push_back('.');
    full.append(test.name);
    if (!filters.empty()) {
      const bool matches = std::any_of(filters.begin(), filters.end(),
                                       [&full](const std::string& filter) {
                                         return full.find(filter) != std::string::npos;
                                       });
      if (!matches) {
        continue;
      }
    }

    current_failure() = false;
    current_diagnostics().clear();
    current_file().clear();
    current_line() = 0;

    test.function();

    if (current_failure()) {
      ++failed;
      failures.push_back(full);
      std::cout << "FAIL " << full << "\n";
      for (const std::string& diagnostic : current_diagnostics()) {
        std::cout << diagnostic << "\n";
      }
      if (!current_file().empty()) {
        std::cout << "  at " << current_file() << ":" << current_line() << "\n";
      }
    } else {
      ++passed;
      std::cout << "PASS " << full << "\n";
    }
    std::cout.flush();
  }

  std::cout << "tests passed=" << passed << " failed=" << failed << "\n";
  return failed == 0U ? 0 : 1;
}

}  // namespace linkobs::test

int main(int argc, char** argv) {
  std::vector<std::string> filters;
  for (int index = 1; index < argc; ++index) {
    filters.emplace_back(argv[index]);
  }
  return linkobs::test::run_all(filters);
}
