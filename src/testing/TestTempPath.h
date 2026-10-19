#ifndef PP_LEDGER_TESTING_TEST_TEMP_PATH_H
#define PP_LEDGER_TESTING_TEST_TEMP_PATH_H

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace pp::test {

/**
 * A path in the system temp directory, named after `name` and private to this test process.
 *
 * ctest runs every test case in a process of its own, and `ctest -j` runs them side by side: a
 * fixed name would be shared by concurrent tests, one wiping or reopening another's files. The
 * process id goes before any extension (`ledger-test` → `ledger-test-4242`, `out.txt` →
 * `out-4242.txt`). Tests still remove what they create.
 */
inline std::filesystem::path uniqueTempPath(const std::string &name) {
#ifdef _WIN32
  const auto pid = _getpid();
#else
  const auto pid = getpid();
#endif
  const std::filesystem::path base(name);
  return std::filesystem::temp_directory_path() /
         (base.stem().string() + "-" + std::to_string(pid) + base.extension().string());
}

} // namespace pp::test

#endif // PP_LEDGER_TESTING_TEST_TEMP_PATH_H
