#include "fixtures.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

#include "linkobs/core/hash128.hpp"
#include "linkobs/core/text.hpp"

namespace linkobs::test {
namespace {

std::uint64_t& counter() {
  static std::uint64_t value = 0;
  return value;
}

}  // namespace

std::string temporary_directory() {
  std::error_code error;
  const std::filesystem::path path = std::filesystem::temp_directory_path(error);
  if (error) {
    return std::string{"."};
  }
  return path.string();
}

TempPath::TempPath(std::string_view stem) {
  const std::uint64_t id = counter();
  counter() = id + 1U;
  std::string base = temporary_directory();
  base.push_back('/');
  base.append("linkobs-test-");
  base.append(stem);
  base.push_back('-');
  base.append(to_dec(id));
  base.append(".lkos");
  path_ = std::move(base);
  remove();
}

TempPath::~TempPath() { remove(); }

void TempPath::remove() const {
  const bool removed = std::remove(path_.c_str()) == 0;
  (void)removed;
  const bool removed_backup = std::remove((path_ + ".prev").c_str()) == 0;
  (void)removed_backup;
  const bool removed_temporary = std::remove((path_ + ".tmp").c_str()) == 0;
  (void)removed_temporary;
}

}  // namespace linkobs::test
