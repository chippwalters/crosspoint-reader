// Host unit tests for the pure helpers behind the USB-serial file-transfer
// protocol (src/network/FtUtil.h): CRC-32, protected-path classification,
// parent-directory extraction, and unsigned parsing.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "network/FtUtil.h"

namespace {
uint32_t crc(const std::string& s) {
  return FtUtil::crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
}  // namespace

// ---- CRC-32 (zlib/IEEE) — known reference vectors -------------------------
TEST(FtCrc32, KnownVectors) {
  EXPECT_EQ(crc(""), 0x00000000u);
  EXPECT_EQ(crc("a"), 0xE8B7BE43u);
  EXPECT_EQ(crc("123456789"), 0xCBF43926u);  // canonical CRC-32 check value
  EXPECT_EQ(crc("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
}

TEST(FtCrc32, IncrementalEqualsWholeBuffer) {
  const std::string s = "PAPERBIT USB-serial file transfer integrity payload.";
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  // Split the stream into two chunks (as the firmware does per 4 KB chunk).
  uint32_t inc = 0xFFFFFFFFu;
  inc = FtUtil::crc32Update(inc, p, 10);
  inc = FtUtil::crc32Update(inc, p + 10, s.size() - 10);
  inc ^= 0xFFFFFFFFu;
  EXPECT_EQ(inc, FtUtil::crc32(p, s.size()));
}

// ---- isProtected ----------------------------------------------------------
TEST(FtIsProtected, BlocksSystemPaths) {
  EXPECT_TRUE(FtUtil::isProtected("/.crosspoint"));
  EXPECT_TRUE(FtUtil::isProtected("/.crosspoint/vault.json"));
  EXPECT_TRUE(FtUtil::isProtected("/.crosspoint/opds.json"));
  EXPECT_TRUE(FtUtil::isProtected("/Vault"));
  EXPECT_TRUE(FtUtil::isProtected("/Vault/note.pbv"));
}

TEST(FtIsProtected, AllowsEverythingElse) {
  EXPECT_FALSE(FtUtil::isProtected("/"));
  EXPECT_FALSE(FtUtil::isProtected(""));
  EXPECT_FALSE(FtUtil::isProtected("/Books/a.epub"));
  EXPECT_FALSE(FtUtil::isProtected("/Vaulted"));       // prefix of /Vault but distinct
  EXPECT_FALSE(FtUtil::isProtected("/Vault2/x"));       // not /Vault or /Vault/
  EXPECT_FALSE(FtUtil::isProtected("/.crosspointX"));   // similar but not protected
  EXPECT_FALSE(FtUtil::isProtected(nullptr));
}

// ---- parentOf -------------------------------------------------------------
TEST(FtParentOf, ExtractsParent) {
  EXPECT_EQ(FtUtil::parentOf("/Books/a.epub"), "/Books");
  EXPECT_EQ(FtUtil::parentOf("/Books/sub/x.txt"), "/Books/sub");
}

TEST(FtParentOf, EdgeCasesYieldEmpty) {
  EXPECT_EQ(FtUtil::parentOf("/a.txt"), "");  // top-level: parent is root -> none to create
  EXPECT_EQ(FtUtil::parentOf("noslash"), "");
  EXPECT_EQ(FtUtil::parentOf("/"), "");
  EXPECT_EQ(FtUtil::parentOf(""), "");
  EXPECT_EQ(FtUtil::parentOf(nullptr), "");
}

// ---- parseU32 -------------------------------------------------------------
TEST(FtParseU32, ParsesDecimals) {
  EXPECT_EQ(FtUtil::parseU32("0"), 0u);
  EXPECT_EQ(FtUtil::parseU32("123"), 123u);
  EXPECT_EQ(FtUtil::parseU32("4294967295"), 4294967295u);  // UINT32_MAX
}

TEST(FtParseU32, NonNumericYieldsZero) {
  EXPECT_EQ(FtUtil::parseU32(""), 0u);
  EXPECT_EQ(FtUtil::parseU32("abc"), 0u);
  EXPECT_EQ(FtUtil::parseU32(nullptr), 0u);
}
