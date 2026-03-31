#include "lib/common/Meta.h"
#include "lib/common/io/Json.h"
#include "lib/common/Serialize.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

using pp::common::Meta;
using pp::common::io::metaFromJsonString;
using pp::common::io::metaToJsonString;
using pp::InputArchive;
using pp::OutputArchive;

namespace {

template <typename T>
std::string archivePack(const T &value) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & value;
  return oss.str();
}

template <typename T>
bool archiveUnpack(const std::string &data, T &value) {
  std::istringstream iss(data, std::ios::binary);
  InputArchive ar(iss);
  ar & value;
  return !ar.failed();
}

std::string archivePackMetaWire(const std::string &key, uint16_t tag,
                                const std::string &payload,
                                uint64_t entryCount = 1) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & entryCount;
  ar & key;
  ar & tag;
  ar & payload;
  return oss.str();
}

} // namespace

TEST(MetaTest, RoundTrip_PrimitivesAndString) {
  Meta m;
  m.set("i64", int64_t{-42});
  m.set("u64", uint64_t{42});
  m.set("b", true);
  m.set("d", 3.125);
  m.set("s", std::string("hello"));

  Meta out;
  ASSERT_TRUE(archiveUnpack(archivePack(m), out));

  EXPECT_EQ(out.getIf<int64_t>("i64").value(), -42);
  EXPECT_EQ(out.getIf<uint64_t>("u64").value(), 42u);
  EXPECT_EQ(out.getIf<bool>("b").value(), true);
  EXPECT_DOUBLE_EQ(out.getIf<double>("d").value(), 3.125);
  EXPECT_EQ(out.getIf<std::string>("s").value(), "hello");
}

TEST(MetaTest, RoundTrip_NestedMeta) {
  Meta inner;
  inner.set("x", uint64_t{7});

  Meta m;
  m.set("inner", inner);

  Meta out;
  ASSERT_TRUE(archiveUnpack(archivePack(m), out));

  auto nestedOpt = out.getMetaIf("inner");
  ASSERT_TRUE(nestedOpt.has_value());
  EXPECT_EQ(nestedOpt->get().getIf<uint64_t>("x").value(), 7u);
}

TEST(MetaTest, DeterministicEncoding_MapOrder) {
  Meta a;
  a.set("b", uint64_t{2});
  a.set("a", uint64_t{1});

  Meta b;
  b.set("a", uint64_t{1});
  b.set("b", uint64_t{2});

  EXPECT_EQ(archivePack(a), archivePack(b));
}

TEST(MetaTest, UnknownTag_IsSkipped) {
  constexpr uint16_t kUnknownWireTag = 999;
  const std::string wire =
      archivePackMetaWire("x", kUnknownWireTag, /*payload*/ "abc");

  Meta out;
  ASSERT_TRUE(archiveUnpack(wire, out));
  EXPECT_TRUE(out.empty());
}

TEST(MetaTest, DuplicateKeys_AreRejected) {
  // Manually craft two entries with the same key.
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  uint64_t entryCount = 2;
  ar & entryCount;

  const std::string key = "dup";

  // payloads are archives of the underlying values
  {
    std::ostringstream p(std::ios::binary);
    OutputArchive par(p);
    int64_t v = 1;
    par & v;
    std::string payload = p.str();
    ar & key & Meta::ValueWire::TAG_I64 & payload;
  }
  {
    std::ostringstream p(std::ios::binary);
    OutputArchive par(p);
    uint64_t v = 2;
    par & v;
    std::string payload = p.str();
    ar & key & Meta::ValueWire::TAG_U64 & payload;
  }

  Meta out;
  EXPECT_TRUE(archiveUnpack(oss.str(), out));
  // Duplicate key is treated as invalid input; decode clears entries to signal invalid.
  EXPECT_TRUE(out.empty());
}

TEST(MetaTest, Json_RoundTrip_PrimitivesAndNested) {
  Meta inner;
  inner.set("x", uint64_t{9});
  inner.set("flag", false);

  Meta m;
  m.set("i64", int64_t{-1});
  m.set("u64", uint64_t{1});
  m.set("b", true);
  m.set("d", 2.5);
  m.set("s", std::string("ok"));
  m.set("inner", inner);

  const std::string j = metaToJsonString(m);
  Meta parsed;
  ASSERT_TRUE(metaFromJsonString(parsed, j));
  EXPECT_EQ(parsed, m);
}

TEST(MetaTest, Json_RoundTrip_NullMetaPtr) {
  Meta m;
  m.set("n", Meta::MetaPtr{});

  const std::string j = metaToJsonString(m);
  Meta parsed;
  ASSERT_TRUE(metaFromJsonString(parsed, j));
  EXPECT_EQ(parsed, m);
}

TEST(MetaTest, GetOrDefault_Int64) {
  Meta m;
  EXPECT_EQ(m.getOrDefault("a", int64_t{-1}), -1);
  m.set("a", int64_t{42});
  EXPECT_EQ(m.getOrDefault("a", int64_t{0}), 42);
  m.set("b", uint64_t{7});
  EXPECT_EQ(m.getOrDefault("b", int64_t{0}), 0);
  m.set("c", std::string("x"));
  EXPECT_EQ(m.getOrDefault("c", int64_t{99}), 99);
}

TEST(MetaTest, GetOrDefault_Uint64) {
  Meta m;
  EXPECT_EQ(m.getOrDefault("a", uint64_t{9}), 9u);
  m.set("a", uint64_t{42});
  EXPECT_EQ(m.getOrDefault("a", uint64_t{0}), 42u);
  m.set("b", int64_t{3});
  EXPECT_EQ(m.getOrDefault("b", uint64_t{0}), 0u);
  m.set("c", std::string("x"));
  EXPECT_EQ(m.getOrDefault("c", uint64_t{11}), 11u);
}

TEST(MetaTest, GetOrDefault_StringBoolDouble) {
  Meta m;
  EXPECT_EQ(m.getOrDefault("s", std::string{"d"}), "d");
  m.set("s", std::string{"ok"});
  EXPECT_EQ(m.getOrDefault("s", std::string{}), "ok");
  EXPECT_FALSE(m.getOrDefault("b", false));
  m.set("b", true);
  EXPECT_TRUE(m.getOrDefault("b", false));
  EXPECT_DOUBLE_EQ(m.getOrDefault("d", 0.0), 0.0);
  m.set("d", 1.5);
  EXPECT_DOUBLE_EQ(m.getOrDefault("d", 0.0), 1.5);
}

TEST(MetaTest, Json_IntegerSignDeterminesStoredWidth) {
  Meta m;
  ASSERT_TRUE(metaFromJsonString(m, R"({"pos":1730000000,"neg":-5})"));
  EXPECT_FALSE(m.getIf<int64_t>("pos").has_value());
  ASSERT_TRUE(m.getIf<uint64_t>("pos").has_value());
  EXPECT_EQ(m.getIf<uint64_t>("pos").value(), 1730000000u);
  ASSERT_TRUE(m.getIf<int64_t>("neg").has_value());
  EXPECT_EQ(m.getIf<int64_t>("neg").value(), -5);
}

