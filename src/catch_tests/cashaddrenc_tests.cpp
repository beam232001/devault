// Copyright (c) 2017 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cashaddr.h>
#include <cashaddrenc.h>
#include <chainparams.h>
#include <random.h>
#include <test/test_bitcoin.h>
#include <uint256.h>
#include <variant>

#include "catch_unit.h"

namespace {

std::vector<std::string> GetNetworks() {
  return {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST};
}

uint160 insecure_GetRandUInt160(FastRandomContext &rand) {
  uint160 n;
  for (uint8_t *c = n.begin(); c != n.end(); ++c) {
    *c = static_cast<uint8_t>(rand.rand32());
  }
  return n;
}

std::vector<uint8_t> insecure_GetRandomByteArray(FastRandomContext &rand, size_t n) {
  std::vector<uint8_t> out;
  out.reserve(n);

  for (size_t i = 0; i < n; i++) {
    out.push_back(uint8_t(rand.randbits(8)));
  }
  return out;
}

class DstTypeChecker {
  public:
  void operator()(const CKeyID &id) { isKey = true; }
  void operator()(const CScriptID &id) { isScript = true; }
  void operator()(const CNoDestination &) {}

  static bool IsScriptDst(const CTxDestination &d) {
    DstTypeChecker checker;
    std::visit(checker, d);
    return checker.isScript;
  }

  static bool IsKeyDst(const CTxDestination &d) {
    DstTypeChecker checker;
    std::visit(checker, d);
    return checker.isKey;
  }

  private:
  DstTypeChecker() : isKey(false), isScript(false) {}
  bool isKey;
  bool isScript;
};

// Map all possible size bits in the version to the expected size of the
// hash in bytes.
const std::array<std::pair<uint8_t, uint32_t>, 8> valid_sizes = {
    {{0, 20}, {1, 24}, {2, 28}, {3, 32}, {4, 40}, {5, 48}, {6, 56}, {7, 64}}};

} // namespace

TEST_CASE("encode_decode_all_sizes") {
  FastRandomContext rand(true);
  const std::string prefix = "devault";

  for (auto ps : valid_sizes) {
    std::vector<uint8_t> data = insecure_GetRandomByteArray(rand, ps.second);
    CashAddrContent content = {PUBKEY_TYPE, data};
    std::vector<uint8_t> packed_data = PackCashAddrContent(content);

    // Check that the packed size is correct
    BOOST_CHECK_EQUAL(packed_data[1] >> 2, ps.first);
    std::string address = cashaddr::Encode(prefix, packed_data);

    // Check that the address decodes properly
    CashAddrContent decoded = DecodeCashAddrContent(address, prefix);
    int i = 0;
    for (auto c : content.hash) {
      BOOST_CHECK_EQUAL(c, decoded.hash[i++]);
    }
  }
}

TEST_CASE("check_packaddr_throws") {
  FastRandomContext rand(true);

  for (auto ps : valid_sizes) {
    std::vector<uint8_t> data = insecure_GetRandomByteArray(rand, ps.second - 1);
    CashAddrContent content = {PUBKEY_TYPE, data};
    BOOST_CHECK_THROW(PackCashAddrContent(content), std::runtime_error);
  }
}

TEST_CASE("encode_decode") {
  std::vector<CTxDestination> toTest = {CNoDestination{}, CKeyID(uint160S("badf00d")), CScriptID(uint160S("f00dbad"))};

  for (auto dst : toTest) {
    for (auto net : GetNetworks()) {
      const auto netParams = CreateChainParams(net);
      std::string encoded = EncodeCashAddr(dst, *netParams);
      CTxDestination decoded = DecodeCashAddr(encoded, *netParams);
      BOOST_CHECK(dst == decoded);
    }
  }
}

// Check that an encoded cash address is not valid on another network.
TEST_CASE("invalid_on_wrong_network") {
  const CTxDestination dst = CKeyID(uint160S("c0ffee"));
  const CTxDestination invalidDst = CNoDestination{};

  for (auto net : GetNetworks()) {
    for (auto otherNet : GetNetworks()) {
      if (net == otherNet) continue;

      const auto netParams = CreateChainParams(net);
      std::string encoded = EncodeCashAddr(dst, *netParams);

      const auto otherNetParams = CreateChainParams(otherNet);
      CTxDestination decoded = DecodeCashAddr(encoded, *otherNetParams);
      //            BOOST_CHECK(decoded != dst);
      BOOST_CHECK(decoded == invalidDst);
    }
  }
}

TEST_CASE("random_dst") {
  FastRandomContext rand(true);

  const size_t NUM_TESTS = 5000;
  const auto params = CreateChainParams(CBaseChainParams::MAIN);

  for (size_t i = 0; i < NUM_TESTS; ++i) {
    uint160 hash = insecure_GetRandUInt160(rand);
    const CTxDestination dst_key = CKeyID(hash);
    const CTxDestination dst_scr = CScriptID(hash);

    const std::string encoded_key = EncodeCashAddr(dst_key, *params);
    const CTxDestination decoded_key = DecodeCashAddr(encoded_key, *params);

    const std::string encoded_scr = EncodeCashAddr(dst_scr, *params);
    const CTxDestination decoded_scr = DecodeCashAddr(encoded_scr, *params);

    std::string err("cashaddr failed for hash: ");
    err += hash.ToString();

    BOOST_CHECK_MESSAGE(dst_key == decoded_key, err);
    BOOST_CHECK_MESSAGE(dst_scr == decoded_scr, err);

    BOOST_CHECK_MESSAGE(DstTypeChecker::IsKeyDst(decoded_key), err);
    BOOST_CHECK_MESSAGE(DstTypeChecker::IsScriptDst(decoded_scr), err);
  }
}

/**
 * Cashaddr payload made of 5-bit nibbles. The last one is padded. When
 * converting back to bytes, this extra padding is truncated. In order to ensure
 * cashaddr are cannonicals, we check that the data we truncate is zeroed.
 */
TEST_CASE("check_padding") {
  uint8_t version = 0;
  std::vector<uint8_t> data = {version};
  for (size_t i = 0; i < 33; ++i) {
    data.push_back(1);
  }

  BOOST_CHECK_EQUAL(data.size(), 34UL);

  const CTxDestination nodst = CNoDestination{};
  const auto params = CreateChainParams(CBaseChainParams::MAIN);

  for (uint8_t i = 0; i < 32; i++) {
    data[data.size() - 1] = i;
    std::string fake = cashaddr::Encode(params->CashAddrPrefix(), data);
    CTxDestination dst = DecodeCashAddr(fake, *params);

    // We have 168 bits of payload encoded as 170 bits in 5 bits nimbles. As
    // a result, we must have 2 zeros.
    if (i & 0x03) {
      BOOST_CHECK(dst == nodst);
    } else {
      // BOOST_CHECK(dst != nodst);
    }
  }
}

/**
 * We ensure type is extracted properly from the version.
 */
TEST_CASE("check_type") {
  std::vector<uint8_t> data;
  data.resize(34);

  const std::string prefix = "devault";

  for (uint8_t v = 0; v < 16; v++) {
    std::fill(begin(data), end(data), 0);
    data[0] = v;
    auto content = DecodeCashAddrContent(cashaddr::Encode(prefix, data), prefix);
    BOOST_CHECK_EQUAL(content.type, v);
    BOOST_CHECK_EQUAL(content.hash.size(), 20UL);

    // Check that using the reserved bit result in a failure.
    data[0] |= 0x10;
    content = DecodeCashAddrContent(cashaddr::Encode(prefix, data), prefix);
    BOOST_CHECK_EQUAL(content.type, 0);
    BOOST_CHECK_EQUAL(content.hash.size(), 0UL);
  }
}

/**
 * We ensure size is extracted and checked properly.
 */
TEST_CASE("check_size") {
  const CTxDestination nodst = CNoDestination{};
  const std::string prefix = "devault";

  std::vector<uint8_t> data;

  for (auto ps : valid_sizes) {
    // Number of bytes required for a 5-bit packed version of a hash, with
    // version byte.  Add half a byte(4) so integer math provides the next
    // multiple-of-5 that would fit all the data.
    size_t expectedSize = (8 * (1 + ps.second) + 4) / 5;
    data.resize(expectedSize);
    std::fill(begin(data), end(data), 0);
    // After conversion from 8 bit packing to 5 bit packing, the size will
    // be in the second 5-bit group, shifted left twice.
    data[1] = ps.first << 2;

    auto content = DecodeCashAddrContent(cashaddr::Encode(prefix, data), prefix);

    BOOST_CHECK_EQUAL(content.type, 0);
    BOOST_CHECK_EQUAL(content.hash.size(), ps.second);

    data.push_back(0);
    content = DecodeCashAddrContent(cashaddr::Encode(prefix, data), prefix);

    BOOST_CHECK_EQUAL(content.type, 0);
    BOOST_CHECK_EQUAL(content.hash.size(), 0UL);

    data.pop_back();
    data.pop_back();
    content = DecodeCashAddrContent(cashaddr::Encode(prefix, data), prefix);

    BOOST_CHECK_EQUAL(content.type, 0);
    BOOST_CHECK_EQUAL(content.hash.size(), 0UL);
  }
}

TEST_CASE("test_encode_address") {
  const auto params = CreateChainParams(CBaseChainParams::MAIN);

  std::vector<std::vector<uint8_t>> hash{
      {118, 160, 64, 83, 189, 160, 168, 139, 218, 81, 119, 184, 106, 21, 195, 178, 159, 85, 152, 115},
      {203, 72, 18, 50, 41, 156, 213, 116, 49, 81, 172, 75, 45, 99, 174, 25, 142, 123, 176, 169},
      {1, 31, 40, 228, 115, 201, 95, 64, 19, 215, 213, 62, 197, 251, 195, 180, 45, 248, 237, 16}};

  std::vector<std::string> pubkey = {"devault:qpm2qsznhks23z7629mms6s4cwef74vcwvztjeqp4y",
                                     "devault:qr95sy3j9xwd2ap32xkykttr4cvcu7as4yfp5gwwna",
                                     "devault:qqq3728yw0y47sqn6l2na30mcw6zm78dzq9ca7gu96"};
  std::vector<std::string> script = {"devault:ppm2qsznhks23z7629mms6s4cwef74vcwv4w0k8zwe",
                                     "devault:pr95sy3j9xwd2ap32xkykttr4cvcu7as4y7yf8fdgq",
                                     "devault:pqq3728yw0y47sqn6l2na30mcw6zm78dzqjaq30l78"};

  for (size_t i = 0; i < hash.size(); ++i) {
    const CTxDestination dstKey = CKeyID(uint160(hash[i]));
    BOOST_CHECK_EQUAL(pubkey[i], EncodeCashAddr(dstKey, *params));

    CashAddrContent keyContent{PUBKEY_TYPE, hash[i]};
    BOOST_CHECK_EQUAL(pubkey[i], EncodeCashAddr("devault", keyContent));

    const CTxDestination dstScript = CScriptID(uint160(hash[i]));
    BOOST_CHECK_EQUAL(script[i], EncodeCashAddr(dstScript, *params));

    CashAddrContent scriptContent{SCRIPT_TYPE, hash[i]};
    BOOST_CHECK_EQUAL(script[i], EncodeCashAddr("devault", scriptContent));
  }
}

struct CashAddrTestVector {
  std::string prefix;
  CashAddrType type;
  std::vector<uint8_t> hash;
  std::string addr;
};

TEST_CASE("test_vectors") {
  std::vector<CashAddrTestVector> cases = {
      // 20 bytes
      {"devault", PUBKEY_TYPE, ParseHex("F5BF48B397DAE70BE82B3CCA4793F8EB2B6CDAC9"),
       "devault:qr6m7j9njldwwzlg9v7v53unlr4jkmx6eyecek538n"},
      {"dvtest", SCRIPT_TYPE, ParseHex("F5BF48B397DAE70BE82B3CCA4793F8EB2B6CDAC9"),
       "dvtest:pr6m7j9njldwwzlg9v7v53unlr4jkmx6ey55h922u9"},
      {"prefix", CashAddrType(15), ParseHex("F5BF48B397DAE70BE82B3CCA4793F8EB2B6CDAC9"),
       "prefix:0r6m7j9njldwwzlg9v7v53unlr4jkmx6ey3qnjwsrf"},
      // 24 bytes
      {"devault", PUBKEY_TYPE, ParseHex("7ADBF6C17084BC86C1706827B41A56F5CA32865925E946EA"),
       "devault:q9adhakpwzztepkpwp5z0dq62m6u5v5xtyj7j3h2exr8k8mz"},
      {"dvtest", SCRIPT_TYPE, ParseHex("7ADBF6C17084BC86C1706827B41A56F5CA32865925E946EA"),
       "dvtest:p9adhakpwzztepkpwp5z0dq62m6u5v5xtyj7j3h2y2haq9fg"},
      {"prefix", CashAddrType(15), ParseHex("7ADBF6C17084BC86C1706827B41A56F5CA32865925E946EA"),
       "prefix:09adhakpwzztepkpwp5z0dq62m6u5v5xtyj7j3h2p29kc2lp"},
      // 28 bytes
      {"devault", PUBKEY_TYPE, ParseHex("3A84F9CF51AAE98A3BB3A78BF16A6183790B18719126325BFC0C075B"),
       "devault:qgagf7w02x4wnz3mkwnchut2vxphjzccwxgjvvjmlsxqwkcxknvp4ds"},
      {"dvtest", SCRIPT_TYPE, ParseHex("3A84F9CF51AAE98A3BB3A78BF16A6183790B18719126325BFC0C075B"),
       "dvtest:pgagf7w02x4wnz3mkwnchut2vxphjzccwxgjvvjmlsxqwkc9lnuh0s5"},
      {"prefix", CashAddrType(15), ParseHex("3A84F9CF51AAE98A3BB3A78BF16A6183790B18719126325BFC0C075B"),
       "prefix:0gagf7w02x4wnz3mkwnchut2vxphjzccwxgjvvjmlsxqwkc5djw8s9g"},
      // 32 bytes
      {"devault", PUBKEY_TYPE,
       ParseHex("3173EF6623C6B48FFD1A3DCC0CC6489B0A07BB47A37F47CFEF4FE69DE825"
                "C060"),
       "devault:"
       "qvch8mmxy0rtfrlarg7ucrxxfzds5pamg73h7370aa87d80gyhqxq5y0kg5qx"},
      {"dvtest", SCRIPT_TYPE,
       ParseHex("3173EF6623C6B48FFD1A3DCC0CC6489B0A07BB47A37F47CFEF4FE69DE825"
                "C060"),
       "dvtest:"
       "pvch8mmxy0rtfrlarg7ucrxxfzds5pamg73h7370aa87d80gyhqxqmpxms7w3"},
      {"prefix", CashAddrType(15),
       ParseHex("3173EF6623C6B48FFD1A3DCC0CC6489B0A07BB47A37F47CFEF4FE69DE825"
                "C060"),
       "prefix:"
       "0vch8mmxy0rtfrlarg7ucrxxfzds5pamg73h7370aa87d80gyhqxqsh6jgp6w"},
      // 40 bytes
      {"devault", PUBKEY_TYPE,
       ParseHex("C07138323E00FA4FC122D3B85B9628EA810B3F381706385E289B0B256311"
                "97D194B5C238BEB136FB"),
       "devault:"
       "qnq8zwpj8cq05n7pytfmskuk9r4gzzel8qtsvwz79zdskftrzxtar994cgutavfklvrp846gv2"},
      {"dvtest", SCRIPT_TYPE,
       ParseHex("C07138323E00FA4FC122D3B85B9628EA810B3F381706385E289B0B256311"
                "97D194B5C238BEB136FB"),
       "dvtest:"
       "pnq8zwpj8cq05n7pytfmskuk9r4gzzel8qtsvwz79zdskftrzxtar994cgutavfklv89lv9j0x"},
      {"prefix", CashAddrType(15),
       ParseHex("C07138323E00FA4FC122D3B85B9628EA810B3F381706385E289B0B256311"
                "97D194B5C238BEB136FB"),
       "prefix:"
       "0nq8zwpj8cq05n7pytfmskuk9r4gzzel8qtsvwz79zdskftrzxtar994cgutavfklvwsvctzqy"},
      // 48 bytes
      {"devault", PUBKEY_TYPE,
       ParseHex("E361CA9A7F99107C17A622E047E3745D3E19CF804ED63C5C40C6BA763696"
                "B98241223D8CE62AD48D863F4CB18C930E4C"),
       "devault:"
       "qh3krj5607v3qlqh5c3wq3lrw3wnuxw0sp8dv0zugrrt5a3kj6ucysfz8kxwv2k53krr7n933jfsunqlqeh3l7t"},
      {"dvtest", SCRIPT_TYPE,
       ParseHex("E361CA9A7F99107C17A622E047E3745D3E19CF804ED63C5C40C6BA763696"
                "B98241223D8CE62AD48D863F4CB18C930E4C"),
       "dvtest:"
       "ph3krj5607v3qlqh5c3wq3lrw3wnuxw0sp8dv0zugrrt5a3kj6ucysfz8kxwv2k53krr7n933jfsunq0f5xq4ls"},
      {"prefix", CashAddrType(15),
       ParseHex("E361CA9A7F99107C17A622E047E3745D3E19CF804ED63C5C40C6BA763696"
                "B98241223D8CE62AD48D863F4CB18C930E4C"),
       "prefix:"
       "0h3krj5607v3qlqh5c3wq3lrw3wnuxw0sp8dv0zugrrt5a3kj6ucysfz8kxwv2k53krr7n933jfsunqakcssnmn"},
      // 56 bytes
      {"devault", PUBKEY_TYPE,
       ParseHex("D9FA7C4C6EF56DC4FF423BAAE6D495DBFF663D034A72D1DC7D52CBFE7D1E"
                "6858F9D523AC0A7A5C34077638E4DD1A701BD017842789982041"),
       "devault:"
       "qmvl5lzvdm6km38lgga64ek5jhdl7e3aqd9895wu04fvhlnare5937w4ywkq57juxsrhvw8ym5d8qx7sz7zz0zvcypqsvcynuxrr"},
      {"dvtest", SCRIPT_TYPE,
       ParseHex("D9FA7C4C6EF56DC4FF423BAAE6D495DBFF663D034A72D1DC7D52CBFE7D1E"
                "6858F9D523AC0A7A5C34077638E4DD1A701BD017842789982041"),
       "dvtest:"
       "pmvl5lzvdm6km38lgga64ek5jhdl7e3aqd9895wu04fvhlnare5937w4ywkq57juxsrhvw8ym5d8qx7sz7zz0zvcypqszntvgurf"},
      {"prefix", CashAddrType(15),
       ParseHex("D9FA7C4C6EF56DC4FF423BAAE6D495DBFF663D034A72D1DC7D52CBFE7D1E"
                "6858F9D523AC0A7A5C34077638E4DD1A701BD017842789982041"),
       "prefix:"
       "0mvl5lzvdm6km38lgga64ek5jhdl7e3aqd9895wu04fvhlnare5937w4ywkq57juxsrhvw8ym5d8qx7sz7zz0zvcypqsgjrqpnw8"},
      // 64 bytes
      {"devault", PUBKEY_TYPE,
       ParseHex("D0F346310D5513D9E01E299978624BA883E6BDA8F4C60883C10F28C2967E"
                "67EC77ECC7EEEAEAFC6DA89FAD72D11AC961E164678B868AEEEC5F2C1DA0"
                "8884175B"),
       "devault:"
       "qlg0x333p4238k0qrc5ej7rzfw5g8e4a4r6vvzyrcy8j3s5k0en7calvclhw46hudk5flttj6ydvjc0pv3nchp52amk97tqa5zygg96mhx2x8y7"
       "q"},
      {"dvtest", SCRIPT_TYPE,
       ParseHex("D0F346310D5513D9E01E299978624BA883E6BDA8F4C60883C10F28C2967E"
                "67EC77ECC7EEEAEAFC6DA89FAD72D11AC961E164678B868AEEEC5F2C1DA0"
                "8884175B"),
       "dvtest:"
       "plg0x333p4238k0qrc5ej7rzfw5g8e4a4r6vvzyrcy8j3s5k0en7calvclhw46hudk5flttj6ydvjc0pv3nchp52amk97tqa5zygg96mu7jjnw5"
       "d"},
      {"prefix", CashAddrType(15),
       ParseHex("D0F346310D5513D9E01E299978624BA883E6BDA8F4C60883C10F28C2967E"
                "67EC77ECC7EEEAEAFC6DA89FAD72D11AC961E164678B868AEEEC5F2C1DA0"
                "8884175B"),
       "prefix:"
       "0lg0x333p4238k0qrc5ej7rzfw5g8e4a4r6vvzyrcy8j3s5k0en7calvclhw46hudk5flttj6ydvjc0pv3nchp52amk97tqa5zygg96ms92w684"
       "5"},
  };

  for (const auto &t : cases) {
    CashAddrContent content{t.type, t.hash};
    BOOST_CHECK_EQUAL(t.addr, EncodeCashAddr(t.prefix, content));

    std::string err("hash mistmatch for address: ");
    err += t.addr;

    content = DecodeCashAddrContent(t.addr, t.prefix);
    BOOST_CHECK_EQUAL(t.type, content.type);
    BOOST_CHECK_MESSAGE(t.hash == content.hash, err);
  }
}
