// Copyright (c) 2012-2015 The Bitcoin Core developers
// Copyright (c) 2019- The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
// (based on key_tests.cpp)

#include <script/sigcache.h>
#include <cashaddrenc.h>
#include <dstencode.h>
#include <key.h>
#include <test/test_bitcoin.h>
#include <utilstrencodings.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

// 32/33 from HD chain
static const std::string strSecret1 = "testpriv:zzxr3dnaadc733gtyeq04tl08hspp52xrmkwa6g3997yhw27zddzk06ee8m4k";
static const std::string strSecret1C = "testpriv:zq96llmh3qm29a0ufmjglh6r3jh8fuqhg5ucu5g2lygnzpmmx0w9jnwvwnenh";

/* We will be testing that these flags do not affect the cache entry.
 * This list must match the one found in script/sigcache.cpp , however
 * we duplicate it here to make sure that changes in cache behaviour also
 * require an intentional change to this test.
 */
static const uint32_t TEST_INVARIANT_FLAGS =
    SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC | SCRIPT_VERIFY_DERSIG |
    SCRIPT_VERIFY_LOW_S | SCRIPT_VERIFY_NULLDUMMY | SCRIPT_VERIFY_SIGPUSHONLY |
    SCRIPT_VERIFY_MINIMALDATA | SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS |
    SCRIPT_VERIFY_CLEANSTACK | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY |
    SCRIPT_VERIFY_CHECKSEQUENCEVERIFY | SCRIPT_VERIFY_MINIMALIF |
    SCRIPT_VERIFY_NULLFAIL | SCRIPT_VERIFY_COMPRESSED_PUBKEYTYPE |
    SCRIPT_ENABLE_SIGHASH_FORKID | SCRIPT_ENABLE_REPLAY_PROTECTION |
    SCRIPT_VERIFY_CHECKDATASIG_SIGOPS;
/* We will be testing that these flags DO affect the cache entry. The expected
 * behaviour is that flags which are not explicitly listed as invariant in
 * script/sigcache.cpp will affect the cache entry. Here we will thus enforce
 * that certain flags are omitted from that sigcache.cpp list.
 */
static const uint32_t TEST_VARIANT_FLAGS = SCRIPT_ENABLE_SCHNORR;

/**
 * Sigcache is only accessible via CachingTransactionSignatureChecker
 * as friend.
 */
class TestCachingTransactionSignatureChecker {
    CachingTransactionSignatureChecker *pchecker;

public:
    TestCachingTransactionSignatureChecker(
        CachingTransactionSignatureChecker &checkerarg) {
        pchecker = &checkerarg;
    }

    inline bool VerifyAndStore(const std::vector<uint8_t> &vchSig,
                               const CPubKey &pubkey, const uint256 &sighash,
                               uint32_t flags) {
        return pchecker->VerifySignature(vchSig, pubkey, sighash, flags);
    }

    inline bool IsCached(const std::vector<uint8_t> &vchSig,
                         const CPubKey &pubkey, const uint256 &sighash,
                         uint32_t flags) {
        return pchecker->IsCached(vchSig, pubkey, sighash, flags);
    }
};

BOOST_FIXTURE_TEST_SUITE(sigcache_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(sig_pubkey_hash_variations) {
    /**
     * Making CachingTransactionSignatureChecker requires a tx. So we make a
     * dummy transaction (doesn't matter what it is) to construct it.
     */
    CDataStream stream(
        ParseHex(
            "010000000122739e70fbee987a8be1788395a2f2e6ad18ccb7ff611cd798071539"
            "dde3c38e000000000151ffffffff010000000000000000016a00000000"),
        SER_NETWORK, PROTOCOL_VERSION);
    CTransaction dummyTx(deserialize, stream);
    PrecomputedTransactionData txdata(dummyTx);
    CachingTransactionSignatureChecker checker(&dummyTx, 0, Amount(), true,
                                               txdata);

    TestCachingTransactionSignatureChecker testChecker(checker);

    uint32_t flags = 0;

    CKey key1 = DecodeSecret(strSecret1);

    CPubKey pubkey1 = key1.GetPubKey();

    for (int n = 0; n < 16; n++) {
        std::string strMsg = strprintf("Sigcache test1 %i: xx", n);
        uint256 hashMsg = Hash(strMsg.begin(), strMsg.end());
        uint256 hashMsg2 = Hash(strMsg.begin() + 1, strMsg.end());

        std::vector<uint8_t> sig;
        BOOST_CHECK(key1.SignECDSA(hashMsg, sig));
        std::vector<uint8_t> sig2;
        BOOST_CHECK(key1.SignECDSA(hashMsg2, sig2));

        // cross-check
        BOOST_CHECK(!testChecker.VerifyAndStore(sig2, pubkey1, hashMsg, flags));
        BOOST_CHECK(!testChecker.VerifyAndStore(sig, pubkey1, hashMsg2, flags));
        // that should not have put them in cache...
        BOOST_CHECK(!testChecker.IsCached(sig2, pubkey1, hashMsg, flags));
        BOOST_CHECK(!testChecker.IsCached(sig, pubkey1, hashMsg2, flags));

        // check that it's not in cache at start
        BOOST_CHECK(!testChecker.IsCached(sig, pubkey1, hashMsg, flags));
        BOOST_CHECK(!testChecker.IsCached(sig2, pubkey1, hashMsg2, flags));
        // Insert into cache
        BOOST_CHECK(testChecker.VerifyAndStore(sig, pubkey1, hashMsg, flags));
        BOOST_CHECK(testChecker.VerifyAndStore(sig2, pubkey1, hashMsg2, flags));
        // check that it's in
        BOOST_CHECK(testChecker.IsCached(sig, pubkey1, hashMsg, flags));
        BOOST_CHECK(testChecker.IsCached(sig2, pubkey1, hashMsg2, flags));
    }
}
BOOST_AUTO_TEST_CASE(flag_invariants) {
    /**
     * Making CachingTransactionSignatureChecker requires a tx. So we make a
     * dummy transaction (doesn't matter what it is) to construct it.
     */
    CDataStream stream(
        ParseHex(
            "010000000122739e70fbee987a8be1788395a2f2e6ad18ccb7ff611cd798071539"
            "dde3c38e000000000151ffffffff010000000000000000016a00000000"),
        SER_NETWORK, PROTOCOL_VERSION);
    CTransaction dummyTx(deserialize, stream);
    PrecomputedTransactionData txdata(dummyTx);
    CachingTransactionSignatureChecker checker(&dummyTx, 0, Amount(), true,
                                               txdata);

    TestCachingTransactionSignatureChecker testChecker(checker);

    CKey key1 = DecodeSecret(strSecret1);
    CPubKey pubkey1 = key1.GetPubKey();

    // there should not be any overlap
    BOOST_REQUIRE((TEST_VARIANT_FLAGS & TEST_INVARIANT_FLAGS) == 0);

    for (int n = 0; n < 16; n++) {
        std::string strMsg = strprintf("Sigcache testflags %i: xx", n);
        uint256 hashMsg = Hash(strMsg.begin(), strMsg.end());

        std::vector<uint8_t> sig;
        BOOST_CHECK(key1.SignECDSA(hashMsg, sig));

        // choose random background flagset to test
        uint32_t base_flags = insecure_rand();

        // shouldn't be in cache at start
        BOOST_CHECK(!testChecker.IsCached(sig, pubkey1, hashMsg, base_flags));
        // insert into cache
        BOOST_CHECK(
            testChecker.VerifyAndStore(sig, pubkey1, hashMsg, base_flags));
        // check that it's in
        BOOST_CHECK(testChecker.IsCached(sig, pubkey1, hashMsg, base_flags));

        // now we flip each of 32 flags one by one, checking cache
        for (uint32_t flag = 1; flag; flag <<= 1) {
            uint32_t alt_flags = base_flags ^ flag;
            BOOST_CHECK(alt_flags != base_flags);
            bool hit = testChecker.IsCached(sig, pubkey1, hashMsg, alt_flags);
            if (TEST_VARIANT_FLAGS & flag) {
                // if it's in TEST_VARIANT_FLAGS, we must miss cache
                BOOST_CHECK_MESSAGE(!hit, n << " bad cache hit 0x" << std::hex
                                            << base_flags << " ^ 0x" << flag);
            } else if (TEST_INVARIANT_FLAGS & flag) {
                // if it's in TEST_INVARIANT_FLAGS, we must hit cache
                BOOST_CHECK_MESSAGE(hit, n << " bad cache miss 0x" << std::hex
                                           << base_flags << " ^ 0x" << flag);
            } else {
                // if it's in neither, don't care.
            }
        }
    }
}
BOOST_AUTO_TEST_SUITE_END()
