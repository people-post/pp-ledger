#include "BeaconServer.h"
#include "common/io/Json.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace pp;

namespace {

std::string readFile(const std::filesystem::path &path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string keysJson(const Beacon::InitKeyConfig &keys) {
  return pp::common::io::metaToJsonString(keys.ltsToMeta(), 2) + "\n";
}

} // namespace

// Regression: a second --init used to fail writing init-keys.json after the
// chain was already recreated with new keys, losing them.
TEST(BeaconServerInitTest, SecondInitWritesNewKeyFileAndKeepsFirst) {
  const auto workDir =
      std::filesystem::temp_directory_path() / "pp-ledger-beacon-reinit-test";
  std::error_code ec;
  std::filesystem::remove_all(workDir, ec);

  // Each init runs in its own scope, like separate `pp-beacon --init` runs:
  // a live server keeps ledger files open, which blocks the next init's
  // cleanup on Windows.
  std::filesystem::path path1;
  std::string content1;
  {
    BeaconServer first;
    auto r1 = first.init(workDir.string());
    ASSERT_TRUE(r1.isOk()) << r1.error().message;
    path1 = first.initKeysPath();
    content1 = keysJson(r1.value());
  }
  EXPECT_EQ(path1, workDir / "init-keys.json");
  ASSERT_TRUE(std::filesystem::exists(path1));
  EXPECT_EQ(readFile(path1), content1);

  std::filesystem::path path2;
  std::string content2;
  {
    BeaconServer second;
    auto r2 = second.init(workDir.string());
    ASSERT_TRUE(r2.isOk()) << r2.error().message;
    path2 = second.initKeysPath();
    content2 = keysJson(r2.value());
  }
  EXPECT_EQ(path2, workDir / "init-keys-2.json");
  ASSERT_TRUE(std::filesystem::exists(path2));
  EXPECT_EQ(readFile(path2), content2);
  EXPECT_NE(content2, content1);
  EXPECT_EQ(readFile(path1), content1);

#if !defined(_WIN32)
  // 0600 is POSIX-only (see writeToNewFile).
  const auto perms = std::filesystem::status(path2).permissions();
  EXPECT_EQ(perms & (std::filesystem::perms::group_all |
                     std::filesystem::perms::others_all),
            std::filesystem::perms::none);
#endif

  std::filesystem::remove_all(workDir, ec);
}
