#include "EnvConfig.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pp;

namespace {

/** Sets an environment variable for the scope of a test. */
class ScopedEnv {
public:
  ScopedEnv(std::string name, const std::string &value) : name_(std::move(name)) {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value.c_str());
#else
    setenv(name_.c_str(), value.c_str(), 1);
#endif
  }
  ~ScopedEnv() {
#ifdef _WIN32
    _putenv_s(name_.c_str(), "");
#else
    unsetenv(name_.c_str());
#endif
  }
  ScopedEnv(const ScopedEnv &) = delete;
  ScopedEnv &operator=(const ScopedEnv &) = delete;

private:
  std::string name_;
};

std::filesystem::path tempFile(const std::string &name, const std::string &content) {
  const auto path = std::filesystem::temp_directory_path() / ("pp-ledger-env-" + name);
  std::ofstream(path, std::ios::trunc) << content;
  return path;
}

} // namespace

// Each kind lands in the JSON the config file parses into; a nested field keeps its siblings.
TEST(EnvConfigTest, OverlayWritesEachKindOverTheFile) {
  common::Object config;
  config.set("port", uint64_t{8519});
  common::Object anchor;
  anchor.set("networkId", "from-file");
  config.set("networkAnchor", anchor);

  ScopedEnv port("PP_LEDGER_PORT", "9000");
  ScopedEnv peers("PP_LEDGER_ALLOWED_PEERS", " QmA, QmB ,,QmC\nQmD ");
  ScopedEnv hash("PP_LEDGER_GENESIS_HASH", "ab");
  std::vector<env_config::Field> fields = env_config::commonFields();
  for (auto &field : env_config::anchorFields()) {
    fields.push_back(field);
  }
  auto set = env_config::overlay(config, fields);
  ASSERT_TRUE(set.isOk()) << set.error().message;
  EXPECT_EQ(set.value(), 3u);
  EXPECT_EQ(config.getNonNegInt("port"), 9000) << "the environment wins over the file";
  const common::Array *list = config.getArray("allowedPeers");
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->elements.size(), 4u) << "commas or newlines; blanks dropped";
  EXPECT_EQ(common::asString(list->elements[1]), "QmB");
  const common::Object *merged = config.getObject("networkAnchor");
  ASSERT_NE(merged, nullptr);
  EXPECT_EQ(merged->getString("networkId"), "from-file");
  EXPECT_EQ(merged->getString("genesisHash"), "ab");
}

TEST(EnvConfigTest, ABadNumberIsRefused) {
  common::Object config;
  ScopedEnv port("PP_LEDGER_PORT", "80a");
  auto set = env_config::overlay(config, env_config::commonFields());
  ASSERT_FALSE(set.isOk());
  EXPECT_NE(set.error().message.find("PP_LEDGER_PORT"), std::string::npos) << set.error().message;
}

// A secret comes from NAME or from the file NAME_FILE names (Docker / Kubernetes secrets).
TEST(EnvConfigTest, SecretsFromTheVariableOrAFile) {
  {
    ScopedEnv key("PP_TEST_SECRET", " hex-key \n");
    EXPECT_EQ(env_config::secret("PP_TEST_SECRET").value(), "hex-key");
  }
  const auto file = tempFile("secret", "file-key\n");
  {
    ScopedEnv key("PP_TEST_SECRET_FILE", file.string());
    EXPECT_EQ(env_config::secret("PP_TEST_SECRET").value(), "file-key");
    ScopedEnv both("PP_TEST_SECRET", "hex-key");
    EXPECT_FALSE(env_config::secret("PP_TEST_SECRET").isOk()) << "one source, not two";
  }
  {
    ScopedEnv missing("PP_TEST_SECRET_FILE", (file.string() + ".missing"));
    EXPECT_FALSE(env_config::secret("PP_TEST_SECRET").isOk());
  }
  EXPECT_FALSE(env_config::secret("PP_TEST_SECRET").value().has_value()) << "neither set";
  std::filesystem::remove(file);

  // A secret field (the miner's keys) reads the file too.
  const auto keys = tempFile("keys", "key-one\nkey-two\n");
  ScopedEnv keysFile("PP_MINER_KEYS_FILE", keys.string());
  common::Object config;
  ASSERT_TRUE(env_config::overlay(config, {{"PP_MINER_KEYS", "keys", env_config::Kind::List, "", true}}).isOk());
  const common::Array *list = config.getArray("keys");
  ASSERT_NE(list, nullptr);
  EXPECT_EQ(list->elements.size(), 2u);
  std::filesystem::remove(keys);
}

// No file and no variable: none (the caller writes its template). Either one: a config.
TEST(EnvConfigTest, RunConfigFromTheFileTheEnvironmentOrNeither) {
  const auto missing = (std::filesystem::temp_directory_path() / "pp-ledger-env-no-config.json").string();
  std::filesystem::remove(missing);
  auto none = env_config::loadConfig(missing, env_config::commonFields());
  ASSERT_TRUE(none.isOk());
  EXPECT_FALSE(none.value().has_value());

  {
    ScopedEnv port("PP_LEDGER_PORT", "9001");
    auto fromEnv = env_config::loadConfig(missing, env_config::commonFields());
    ASSERT_TRUE(fromEnv.isOk());
    ASSERT_TRUE(fromEnv.value().has_value()) << "the environment alone is a config";
    EXPECT_EQ(fromEnv.value()->getNonNegInt("port"), 9001);
    EXPECT_FALSE(std::filesystem::exists(missing)) << "nothing is written";
  }

  const auto file = tempFile("config.json", R"({"port": 8519, "beacon": "x"})");
  ScopedEnv port("PP_LEDGER_PORT", "9002");
  auto both = env_config::loadConfig(file.string(), env_config::commonFields());
  ASSERT_TRUE(both.isOk());
  ASSERT_TRUE(both.value().has_value());
  EXPECT_EQ(both.value()->getNonNegInt("port"), 9002);
  EXPECT_EQ(both.value()->getString("beacon"), "x") << "the file's other fields stay";
  std::filesystem::remove(file);
}
