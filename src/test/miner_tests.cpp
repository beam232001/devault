// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017-2018 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <miner.h>

#include <chainparams.h>
#include <coins.h>
#include <config.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <policy/policy.h>
#include <pubkey.h>
#include <script/standard.h>
#include <txmempool.h>
#include <uint256.h>
#include <util.h>
#include <utilstrencodings.h>
#include <validation.h>

#include <test/test_bitcoin.h>

#include <memory>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(miner_tests, TestingSetup)

static CFeeRate blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE_PER_KB);

static struct {
    uint8_t extranonce;
    uint32_t nonce;
} blockinfo[] = {
    {4, 0xa4a3e227}, // Need nonces that won't fail PoW! !!!
};

CBlockIndex CreateBlockIndex(int nHeight) {
    CBlockIndex index;
    index.nHeight = nHeight;
    index.pprev = chainActive.Tip();
    return index;
}

bool TestSequenceLocks(const CTransaction &tx, int flags) {
    LOCK(g_mempool.cs);
    return CheckSequenceLocks(tx, flags);
}

// Test suite for ancestor feerate transaction selection.
// Implemented as an additional function, rather than a separate test case, to
// allow reusing the blockchain created in CreateNewBlock_validity.
// Note that this test assumes blockprioritypercentage is 0.
void TestPackageSelection(Config &config, CScript scriptPubKey,
                          std::vector<CTransactionRef> &txFirst) {
    // Test the ancestor feerate transaction selection.
    TestMemPoolEntryHelper entry;

    // these 3 tests assume blockprioritypercentage is 0.
    config.SetBlockPriorityPercentage(0);

    // Test that a medium fee transaction will be selected after a higher fee
    // rate package with a low fee rate parent.
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vin[0].prevout = COutPoint(txFirst[0]->GetId(), 0);
    tx.vout.resize(1);
    tx.vout[0].nValue = Amount(5000000000LL - 1000);
    // This tx has a low fee: 1000 satoshis.
    // Save this txid for later use.
    TxId parentTxId = tx.GetId();
    g_mempool.addUnchecked(parentTxId, entry.Fee(Amount(1000))
                                           .Time(GetTime())
                                           .SpendsCoinbase(true)
                                           .FromTx(tx));

    // This tx has a medium fee: 10000 satoshis.
    tx.vin[0].prevout = COutPoint(txFirst[1]->GetId(), 0);
    tx.vout[0].nValue = Amount(5000000000LL - 10000);
    TxId mediumFeeTxId = tx.GetId();
    g_mempool.addUnchecked(mediumFeeTxId, entry.Fee(Amount(10000))
                                              .Time(GetTime())
                                              .SpendsCoinbase(true)
                                              .FromTx(tx));

    // This tx has a high fee, but depends on the first transaction.
    tx.vin[0].prevout = COutPoint(parentTxId, 0);
    // 50k satoshi fee.
    tx.vout[0].nValue = Amount(5000000000LL - 1000 - 50000);
    TxId highFeeTxId = tx.GetId();
    g_mempool.addUnchecked(highFeeTxId, entry.Fee(Amount(50000))
                                            .Time(GetTime())
                                            .SpendsCoinbase(false)
                                            .FromTx(tx));

    std::unique_ptr<CBlockTemplate> pblocktemplate =
        BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey);
    BOOST_CHECK(pblocktemplate->block.vtx[1]->GetId() == parentTxId);
    BOOST_CHECK(pblocktemplate->block.vtx[2]->GetId() == highFeeTxId);
    BOOST_CHECK(pblocktemplate->block.vtx[3]->GetId() == mediumFeeTxId);

    // Test that a package below the block min tx fee doesn't get included
    tx.vin[0].prevout = COutPoint(highFeeTxId, 0);
    // 0 fee.
    tx.vout[0].nValue = Amount(5000000000LL - 1000 - 50000);
    TxId freeTxId = tx.GetId();
    g_mempool.addUnchecked(freeTxId, entry.Fee(Amount::zero()).FromTx(tx));
    size_t freeTxSize = GetVirtualTransactionSize(CTransaction(tx));

    // Calculate a fee on child transaction that will put the package just
    // below the block min tx fee (assuming 1 child tx of the same size).
    Amount feeToUse = blockMinFeeRate.GetFee(2 * freeTxSize) - Amount::min_amount();

    tx.vin[0].prevout = COutPoint(freeTxId, 0);
    tx.vout[0].nValue = Amount(5000000000LL - 1000 - 50000) - feeToUse;
    TxId lowFeeTxId = tx.GetId();
    g_mempool.addUnchecked(lowFeeTxId, entry.Fee(feeToUse).FromTx(tx));
    pblocktemplate =
        BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey);
    // Verify that the free tx and the low fee tx didn't get selected.
    for (const auto &txn : pblocktemplate->block.vtx) {
        BOOST_CHECK(txn->GetId() != freeTxId);
        BOOST_CHECK(txn->GetId() != lowFeeTxId);
    }

    // Test that packages above the min relay fee do get included, even if one
    // of the transactions is below the min relay fee. Remove the low fee
    // transaction and replace with a higher fee transaction
    g_mempool.removeRecursive(CTransaction(tx));
    // Now we should be just over the min relay fee.
    tx.vout[0].nValue -= 2 * Amount::min_amount();
    lowFeeTxId = tx.GetId();
    g_mempool.addUnchecked(lowFeeTxId,
                           entry.Fee(feeToUse + Amount(2)).FromTx(tx));
    pblocktemplate =
        BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey);
    BOOST_CHECK(pblocktemplate->block.vtx[4]->GetId() == freeTxId);
    BOOST_CHECK(pblocktemplate->block.vtx[5]->GetId() == lowFeeTxId);

    // Test that transaction selection properly updates ancestor fee
    // calculations as ancestor transactions get included in a block. Add a
    // 0-fee transaction that has 2 outputs.
    tx.vin[0].prevout = COutPoint(txFirst[2]->GetId(), 0);
    tx.vout.resize(2);
    tx.vout[0].nValue = Amount(5000000000LL - 100000000);
    // 1BCC output.
    tx.vout[1].nValue = Amount(100000000);
    TxId freeTxId2 = tx.GetId();
    g_mempool.addUnchecked(
        freeTxId2, entry.Fee(Amount::zero()).SpendsCoinbase(true).FromTx(tx));

    // This tx can't be mined by itself.
    tx.vin[0].prevout = COutPoint(freeTxId2, 0);
    tx.vout.resize(1);
    feeToUse = blockMinFeeRate.GetFee(freeTxSize);
    tx.vout[0].nValue = Amount(5000000000LL - 100000000) - feeToUse;
    TxId lowFeeTxId2 = tx.GetId();
    g_mempool.addUnchecked(
        lowFeeTxId2, entry.Fee(feeToUse).SpendsCoinbase(false).FromTx(tx));
    pblocktemplate =
        BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey);

    // Verify that this tx isn't selected.
    for (const auto &txn : pblocktemplate->block.vtx) {
        BOOST_CHECK(txn->GetId() != freeTxId2);
        BOOST_CHECK(txn->GetId() != lowFeeTxId2);
    }

    // This tx will be mineable, and should cause lowFeeTxId2 to be selected as
    // well.
    tx.vin[0].prevout = COutPoint(freeTxId2, 1);
    // 10k satoshi fee.
    tx.vout[0].nValue = Amount(100000000 - 10000);
    g_mempool.addUnchecked(tx.GetId(), entry.Fee(Amount(10000)).FromTx(tx));
    pblocktemplate =
        BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey);
    BOOST_CHECK(pblocktemplate->block.vtx[8]->GetId() == lowFeeTxId2);
}

void TestCoinbaseMessageEB(uint64_t eb, std::string cbmsg) {
    GlobalConfig config;
    config.SetMaxBlockSize(eb);

    CScript scriptPubKey =
        CScript() << ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909"
                              "a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112"
                              "de5c384df7ba0b8d578a4c702b6bf11d5f")
                  << OP_CHECKSIG;

    std::unique_ptr<CBlockTemplate> pblocktemplate =
        BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey);

    CBlock *pblock = &pblocktemplate->block;

    // IncrementExtraNonce creates a valid coinbase and merkleRoot
    unsigned int extraNonce = 0;
    IncrementExtraNonce(config, pblock, chainActive.Tip(), extraNonce);
    unsigned int nHeight = chainActive.Tip()->nHeight + 1;
    std::vector<uint8_t> vec(cbmsg.begin(), cbmsg.end());
    BOOST_CHECK(pblock->vtx[0]->vin[0].scriptSig ==
                ((CScript() << CScriptNum::serialize(nHeight) << CScriptNum(extraNonce) << vec) +
                 COINBASE_FLAGS));
}

// Coinbase scriptSig has to contains the correct EB value
// converted to MB, rounded down to the first decimal
BOOST_AUTO_TEST_CASE(CheckCoinbase_EB) {
    TestCoinbaseMessageEB(1000001, "/EB1.0/");
    TestCoinbaseMessageEB(2000000, "/EB2.0/");
    TestCoinbaseMessageEB(8000000, "/EB8.0/");
    TestCoinbaseMessageEB(8320000, "/EB8.3/");
}
#ifdef DEBUG_THIS
// NOTE: These tests rely on CreateNewBlock doing its own self-validation!
BOOST_AUTO_TEST_CASE(CreateNewBlock_validity) {
    // Note that by default, these tests run with size accounting enabled.
    CScript scriptPubKey =
        CScript() << ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909"
                              "a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112"
                              "de5c384df7ba0b8d578a4c702b6bf11d5f")
                  << OP_CHECKSIG;
    std::unique_ptr<CBlockTemplate> pblocktemplate;
    CMutableTransaction tx, tx2;
    CScript script;
    uint256 hash;
    TestMemPoolEntryHelper entry;
    entry.nFee = 11 * Amount::min_amount();
    entry.dPriority = 111.0;
    entry.nHeight = 11;

    fCheckpointsEnabled = false;

    GlobalConfig config;

    // Simple block creation, nothing special yet:
    BOOST_CHECK(
        pblocktemplate =
            BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey));

    // We can't make transactions until we have inputs. Therefore, load 100
    // blocks :)
    int baseheight = 0;
    std::vector<CTransactionRef> txFirst;
    for (size_t i = 0; i < sizeof(blockinfo) / sizeof(*blockinfo); ++i) {
        // pointer for convenience.
        CBlock *pblock = &pblocktemplate->block;
        {
            LOCK(cs_main);
            pblock->nVersion = 1;
            pblock->nTime = chainActive.Tip()->GetMedianTimePast() + 1;
            CMutableTransaction txCoinbase(*pblock->vtx[0]);
            txCoinbase.nVersion = 1;
            txCoinbase.vin[0].scriptSig = CScript();
            txCoinbase.vin[0].scriptSig.push_back(blockinfo[i].extranonce);
            txCoinbase.vin[0].scriptSig.push_back(chainActive.Height());
            txCoinbase.vout.resize(1);
            txCoinbase.vout[0].scriptPubKey = CScript();
            pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
            if (txFirst.size() == 0) {
                baseheight = chainActive.Height();
            }
            if (txFirst.size() < 4) {
                txFirst.push_back(pblock->vtx[0]);
            }
            pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
            pblock->nNonce = blockinfo[i].nonce;
        }
        std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
      
        // Failing PoW check
#ifdef HAVE_GOOD_NONCES
      BOOST_CHECK(ProcessNewBlock(config, shared_pblock, true, nullptr));
#else
      
      ProcessNewBlock(config, shared_pblock, true, nullptr);
#endif
        pblock->hashPrevBlock = pblock->GetHash();
    }

    LOCK(cs_main);

    // Just to make sure we can still make simple blocks.
    BOOST_CHECK(
        pblocktemplate =
            BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey));

    const Amount BLOCKSUBSIDY = 50 * COIN;
    const Amount LOWFEE = CENT;
    const Amount HIGHFEE = COIN;
    const Amount HIGHERFEE = 4 * COIN;

    // block sigops > limit: 1000 CHECKMULTISIG + 1
    tx.vin.resize(1);
    // NOTE: OP_NOP is used to force 20 SigOps for the CHECKMULTISIG
    tx.vin[0].scriptSig = CScript() << OP_0 << OP_0 << OP_0 << OP_NOP
                                    << OP_CHECKMULTISIG << OP_1;
    tx.vin[0].prevout = COutPoint(txFirst[0]->GetId(), 0);
    tx.vout.resize(1);
    tx.vout[0].nValue = BLOCKSUBSIDY;
    for (unsigned int i = 0; i < 1001; ++i) {
        tx.vout[0].nValue -= LOWFEE;
        hash = tx.GetId();
        // Only first tx spends coinbase.
        bool spendsCoinbase = i == 0;
        // If we don't set the # of sig ops in the CTxMemPoolEntry, template
        // creation fails.
        g_mempool.addUnchecked(hash, entry.Fee(LOWFEE)
                                         .Time(GetTime())
                                         .SpendsCoinbase(spendsCoinbase)
                                         .FromTx(tx));
        tx.vin[0].prevout = COutPoint(hash, 0);
    }
#ifdef DEBUG_THIS
  BOOST_CHECK_THROW(
                    BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey),
                    std::runtime_error);
#else
    BOOST_CHECK(BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey));
#endif
    g_mempool.clear();

    tx.vin[0].prevout = COutPoint(txFirst[0]->GetId(), 0);
    tx.vout[0].nValue = BLOCKSUBSIDY;
    for (unsigned int i = 0; i < 1001; ++i) {
        tx.vout[0].nValue -= LOWFEE;
        hash = tx.GetId();
        // Only first tx spends coinbase.
        bool spendsCoinbase = i == 0;
        // If we do set the # of sig ops in the CTxMemPoolEntry, template
        // creation passes.
        g_mempool.addUnchecked(hash, entry.Fee(LOWFEE)
                                         .Time(GetTime())
                                         .SpendsCoinbase(spendsCoinbase)
                                         .SigOpsCost(80)
                                         .FromTx(tx));
        tx.vin[0].prevout = COutPoint(hash, 0);
    }
    BOOST_CHECK(
        pblocktemplate =
            BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey));
    g_mempool.clear();

    // block size > limit
    tx.vin[0].scriptSig = CScript();
    // 18 * (520char + DROP) + OP_1 = 9433 bytes
    std::vector<uint8_t> vchData(520);
    for (unsigned int i = 0; i < 18; ++i) {
        tx.vin[0].scriptSig << vchData << OP_DROP;
    }

    tx.vin[0].scriptSig << OP_1;
    tx.vin[0].prevout = COutPoint(txFirst[0]->GetId(), 0);
    tx.vout[0].nValue = BLOCKSUBSIDY;
    for (unsigned int i = 0; i < 128; ++i) {
        tx.vout[0].nValue -= LOWFEE;
        hash = tx.GetId();
        // Only first tx spends coinbase.
        bool spendsCoinbase = i == 0;
        g_mempool.addUnchecked(hash, entry.Fee(LOWFEE)
                                         .Time(GetTime())
                                         .SpendsCoinbase(spendsCoinbase)
                                         .FromTx(tx));
        tx.vin[0].prevout = COutPoint(hash, 0);
    }
#ifdef DEBUG_THIS
    BOOST_CHECK(
        pblocktemplate =
            BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey));
#endif    
    g_mempool.clear();

    // Orphan in mempool, template creation fails.
    hash = tx.GetId();
    g_mempool.addUnchecked(hash, entry.Fee(LOWFEE).Time(GetTime()).FromTx(tx));
#ifdef DEBUG_THIS    
    BOOST_CHECK_THROW(
        BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey), std::runtime_error);
#endif
    g_mempool.clear();

    // Child with higher priority than parent.
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vin[0].prevout = COutPoint(txFirst[1]->GetId(), 0);
    tx.vout[0].nValue = BLOCKSUBSIDY - HIGHFEE;
    hash = tx.GetId();
    g_mempool.addUnchecked(
        hash,
        entry.Fee(HIGHFEE).Time(GetTime()).SpendsCoinbase(true).FromTx(tx));
    tx.vin[0].prevout = COutPoint(hash, 0);
    tx.vin.resize(2);
    tx.vin[1].scriptSig = CScript() << OP_1;
    tx.vin[1].prevout = COutPoint(txFirst[0]->GetId(), 0);
    // First txn output + fresh coinbase - new txn fee.
    tx.vout[0].nValue = tx.vout[0].nValue + BLOCKSUBSIDY - HIGHERFEE;
    hash = tx.GetId();
    g_mempool.addUnchecked(
        hash,
        entry.Fee(HIGHERFEE).Time(GetTime()).SpendsCoinbase(true).FromTx(tx));
    BOOST_CHECK(
        pblocktemplate =
            BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey));
    g_mempool.clear();

    // Coinbase in mempool, template creation fails.
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint();
    tx.vin[0].scriptSig = CScript() << OP_0 << OP_1;
    tx.vout[0].nValue = Amount::zero();
    hash = tx.GetId();
    // Give it a fee so it'll get mined.
    g_mempool.addUnchecked(
        hash,
        entry.Fee(LOWFEE).Time(GetTime()).SpendsCoinbase(false).FromTx(tx));
#ifdef DEBUG_THIS
    BOOST_CHECK_THROW(
        BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey),
        std::runtime_error);
#endif
    g_mempool.clear();

    // Invalid (pre-p2sh) txn in mempool, template creation fails.
    std::array<int64_t, CBlockIndex::nMedianTimeSpan> times;
    for (int i = 0; i < CBlockIndex::nMedianTimeSpan; i++) {
        // Trick the MedianTimePast.
        times[i] = chainActive.Tip()
                       ->GetAncestor(chainActive.Tip()->nHeight - i)
                       ->nTime;
        chainActive.Tip()->GetAncestor(chainActive.Tip()->nHeight - i)->nTime =
            P2SH_ACTIVATION_TIME;
    }

    tx.vin[0].prevout = COutPoint(txFirst[0]->GetId(), 0);
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vout[0].nValue = BLOCKSUBSIDY - LOWFEE;
    script = CScript() << OP_0;
    tx.vout[0].scriptPubKey = GetScriptForDestination(CScriptID(script));
    hash = tx.GetId();
    g_mempool.addUnchecked(
        hash,
        entry.Fee(LOWFEE).Time(GetTime()).SpendsCoinbase(true).FromTx(tx));
    tx.vin[0].prevout = COutPoint(hash, 0);
    tx.vin[0].scriptSig = CScript()
                          << std::vector<uint8_t>(script.begin(), script.end());
    tx.vout[0].nValue -= LOWFEE;
    hash = tx.GetId();
    g_mempool.addUnchecked(
        hash,
        entry.Fee(LOWFEE).Time(GetTime()).SpendsCoinbase(false).FromTx(tx));
    BOOST_CHECK_THROW(
        BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey),
        std::runtime_error);
    g_mempool.clear();
    for (int i = 0; i < CBlockIndex::nMedianTimeSpan; i++) {
        // Restore the MedianTimePast.
        chainActive.Tip()->GetAncestor(chainActive.Tip()->nHeight - i)->nTime =
            times[i];
    }

    // Double spend txn pair in mempool, template creation fails.
    tx.vin[0].prevout = COutPoint(txFirst[0]->GetId(), 0);
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vout[0].nValue = BLOCKSUBSIDY - HIGHFEE;
    tx.vout[0].scriptPubKey = CScript() << OP_1;
    hash = tx.GetId();
    g_mempool.addUnchecked(
        hash,
        entry.Fee(HIGHFEE).Time(GetTime()).SpendsCoinbase(true).FromTx(tx));
    tx.vout[0].scriptPubKey = CScript() << OP_2;
    hash = tx.GetId();
    g_mempool.addUnchecked(
        hash,
        entry.Fee(HIGHFEE).Time(GetTime()).SpendsCoinbase(true).FromTx(tx));
    BOOST_CHECK_THROW(
        BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey),
        std::runtime_error);
    g_mempool.clear();

    // Subsidy changing.
    int nHeight = chainActive.Height();
    // Create an actual 209999-long block chain (without valid blocks).
    while (chainActive.Tip()->nHeight < 209999) {
        CBlockIndex *prev = chainActive.Tip();
        CBlockIndex *next = new CBlockIndex();
        next->phashBlock = new uint256(InsecureRand256());
        pcoinsTip->SetBestBlock(next->GetBlockHash());
        next->pprev = prev;
        next->nHeight = prev->nHeight + 1;
        next->BuildSkip();
        chainActive.SetTip(next);
    }
    BOOST_CHECK(
        pblocktemplate =
            BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey));
    // Extend to a 210000-long block chain.
    while (chainActive.Tip()->nHeight < 210000) {
        CBlockIndex *prev = chainActive.Tip();
        CBlockIndex *next = new CBlockIndex();
        next->phashBlock = new uint256(InsecureRand256());
        pcoinsTip->SetBestBlock(next->GetBlockHash());
        next->pprev = prev;
        next->nHeight = prev->nHeight + 1;
        next->BuildSkip();
        chainActive.SetTip(next);
    }
    BOOST_CHECK(
        pblocktemplate =
            BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey));
    // Delete the dummy blocks again.
    while (chainActive.Tip()->nHeight > nHeight) {
        CBlockIndex *del = chainActive.Tip();
        chainActive.SetTip(del->pprev);
        pcoinsTip->SetBestBlock(del->pprev->GetBlockHash());
        delete del->phashBlock;
        delete del;
    }

    // non-final txs in mempool
    SetMockTime(chainActive.Tip()->GetMedianTimePast() + 1);
    uint32_t flags = LOCKTIME_VERIFY_SEQUENCE | LOCKTIME_MEDIAN_TIME_PAST;
    // height map
    std::vector<int> prevheights;

    // Relative height locked.
    tx.nVersion = 2;
    tx.vin.resize(1);
    prevheights.resize(1);
    // Only 1 transaction.
    tx.vin[0].prevout = COutPoint(txFirst[0]->GetId(), 0);
    tx.vin[0].scriptSig = CScript() << OP_1;
    // txFirst[0] is the 2nd block
    tx.vin[0].nSequence = chainActive.Tip()->nHeight + 1;
    prevheights[0] = baseheight + 1;
    tx.vout.resize(1);
    tx.vout[0].nValue = BLOCKSUBSIDY - HIGHFEE;
    tx.vout[0].scriptPubKey = CScript() << OP_1;
    tx.nLockTime = 0;
    hash = tx.GetId();
    g_mempool.addUnchecked(
        hash,
        entry.Fee(HIGHFEE).Time(GetTime()).SpendsCoinbase(true).FromTx(tx));

    {
        // Locktime passes.
        CValidationState state;
        BOOST_CHECK(ContextualCheckTransactionForCurrentBlock(
            config, CTransaction(tx), state, flags));
    }

    // Sequence locks fail.
    BOOST_CHECK(!TestSequenceLocks(CTransaction(tx), flags));
    // Sequence locks pass on 2nd block.
    BOOST_CHECK(
        SequenceLocks(CTransaction(tx), flags, &prevheights,
                      CreateBlockIndex(chainActive.Tip()->nHeight + 2)));

    // Relative time locked.
    tx.vin[0].prevout = COutPoint(txFirst[1]->GetId(), 0);
    // txFirst[1] is the 3rd block.
    tx.vin[0].nSequence = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG |
                          (((chainActive.Tip()->GetMedianTimePast() + 1 -
                             chainActive[1]->GetMedianTimePast()) >>
                            CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) +
                           1);
    prevheights[0] = baseheight + 2;
    hash = tx.GetId();
    g_mempool.addUnchecked(hash, entry.Time(GetTime()).FromTx(tx));

    {
        // Locktime passes.
        CValidationState state;
        BOOST_CHECK(ContextualCheckTransactionForCurrentBlock(
            config, CTransaction(tx), state, flags));
    }

    // Sequence locks fail.
    BOOST_CHECK(!TestSequenceLocks(CTransaction(tx), flags));

    for (int i = 0; i < CBlockIndex::nMedianTimeSpan; i++) {
        // Trick the MedianTimePast.
        chainActive.Tip()->GetAncestor(chainActive.Tip()->nHeight - i)->nTime +=
            512;
    }
    // Sequence locks pass 512 seconds later.
    BOOST_CHECK(
        SequenceLocks(CTransaction(tx), flags, &prevheights,
                      CreateBlockIndex(chainActive.Tip()->nHeight + 1)));
    for (int i = 0; i < CBlockIndex::nMedianTimeSpan; i++) {
        // Undo tricked MTP.
        chainActive.Tip()->GetAncestor(chainActive.Tip()->nHeight - i)->nTime -=
            512;
    }

    // Absolute height locked.
    tx.vin[0].prevout = COutPoint(txFirst[2]->GetId(), 0);
    tx.vin[0].nSequence = CTxIn::SEQUENCE_FINAL - 1;
    prevheights[0] = baseheight + 3;
    tx.nLockTime = chainActive.Tip()->nHeight + 1;
    hash = tx.GetId();
    g_mempool.addUnchecked(hash, entry.Time(GetTime()).FromTx(tx));

    {
        // Locktime fails.
        CValidationState state;
        BOOST_CHECK(!ContextualCheckTransactionForCurrentBlock(
            config, CTransaction(tx), state, flags));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-nonfinal");
    }

    // Sequence locks pass.
    BOOST_CHECK(TestSequenceLocks(CTransaction(tx), flags));

    {
        // Locktime passes on 2nd block.
        CValidationState state;
        int64_t nMedianTimePast = chainActive.Tip()->GetMedianTimePast();
        BOOST_CHECK(ContextualCheckTransaction(
            config, CTransaction(tx), state, chainActive.Tip()->nHeight + 2,
            nMedianTimePast, nMedianTimePast));
    }

    // Absolute time locked.
    tx.vin[0].prevout = COutPoint(txFirst[3]->GetId(), 0);
    tx.nLockTime = chainActive.Tip()->GetMedianTimePast();
    prevheights.resize(1);
    prevheights[0] = baseheight + 4;
    hash = tx.GetId();
    g_mempool.addUnchecked(hash, entry.Time(GetTime()).FromTx(tx));

    {
        // Locktime fails.
        CValidationState state;
        BOOST_CHECK(!ContextualCheckTransactionForCurrentBlock(
            config, CTransaction(tx), state, flags));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-nonfinal");
    }

    // Sequence locks pass.
    BOOST_CHECK(TestSequenceLocks(CTransaction(tx), flags));

    {
        // Locktime passes 1 second later.
        CValidationState state;
        int64_t nMedianTimePast = chainActive.Tip()->GetMedianTimePast() + 1;
        BOOST_CHECK(ContextualCheckTransaction(
            config, CTransaction(tx), state, chainActive.Tip()->nHeight + 1,
            nMedianTimePast, nMedianTimePast));
    }

    // mempool-dependent transactions (not added)
    tx.vin[0].prevout = COutPoint(hash, 0);
    prevheights[0] = chainActive.Tip()->nHeight + 1;
    tx.nLockTime = 0;
    tx.vin[0].nSequence = 0;

    {
        // Locktime passes.
        CValidationState state;
        BOOST_CHECK(ContextualCheckTransactionForCurrentBlock(
            config, CTransaction(tx), state, flags));
    }

    // Sequence locks pass.
    BOOST_CHECK(TestSequenceLocks(CTransaction(tx), flags));
    tx.vin[0].nSequence = 1;
    // Sequence locks fail.
    BOOST_CHECK(!TestSequenceLocks(CTransaction(tx), flags));
    tx.vin[0].nSequence = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG;
    // Sequence locks pass.
    BOOST_CHECK(TestSequenceLocks(CTransaction(tx), flags));
    tx.vin[0].nSequence = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | 1;
    // Sequence locks fail.
    BOOST_CHECK(!TestSequenceLocks(CTransaction(tx), flags));

    pblocktemplate =
        BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey);
    BOOST_CHECK(pblocktemplate);

    // None of the of the absolute height/time locked tx should have made it
    // into the template because we still check IsFinalTx in CreateNewBlock, but
    // relative locked txs will if inconsistently added to g_mempool. For now
    // these will still generate a valid template until BIP68 soft fork.
    BOOST_CHECK_EQUAL(pblocktemplate->block.vtx.size(), 3UL);
    // However if we advance height by 1 and time by 512, all of them should be
    // mined.
    for (int i = 0; i < CBlockIndex::nMedianTimeSpan; i++) {
        // Trick the MedianTimePast.
        chainActive.Tip()->GetAncestor(chainActive.Tip()->nHeight - i)->nTime +=
            512;
    }
    chainActive.Tip()->nHeight++;
    SetMockTime(chainActive.Tip()->GetMedianTimePast() + 1);

    BOOST_CHECK(
        pblocktemplate =
            BlockAssembler(config, g_mempool).CreateNewBlock(scriptPubKey));
    BOOST_CHECK_EQUAL(pblocktemplate->block.vtx.size(), 5UL);

    chainActive.Tip()->nHeight--;
    SetMockTime(0);
    g_mempool.clear();

    TestPackageSelection(config, scriptPubKey, txFirst);

    fCheckpointsEnabled = true;
}
#endif

void CheckBlockMaxSize(const Config &config, uint64_t size, uint64_t expected) {
    gArgs.ForceSetArg("-blockmaxsize", std::to_string(size));

    BlockAssembler ba(config, g_mempool);
    BOOST_CHECK_EQUAL(ba.GetMaxGeneratedBlockSize(), expected);
}

BOOST_AUTO_TEST_CASE(BlockAssembler_construction) {
    GlobalConfig config;

    // We are working on a fake chain and need to protect ourselves.
    LOCK(cs_main);

    // Test around historical 1MB (plus one byte because that's mandatory)
    config.SetMaxBlockSize(ONE_MEGABYTE + 1);
    CheckBlockMaxSize(config, 0, 1000);
    CheckBlockMaxSize(config, 1000, 1000);
    CheckBlockMaxSize(config, 1001, 1001);
    CheckBlockMaxSize(config, 12345, 12345);

    CheckBlockMaxSize(config, ONE_MEGABYTE - 1001, ONE_MEGABYTE - 1001);
    CheckBlockMaxSize(config, ONE_MEGABYTE - 1000, ONE_MEGABYTE - 1000);
    CheckBlockMaxSize(config, ONE_MEGABYTE - 999, ONE_MEGABYTE - 999);
    CheckBlockMaxSize(config, ONE_MEGABYTE, ONE_MEGABYTE - 999);

    // Test around default cap
    config.SetMaxBlockSize(DEFAULT_MAX_BLOCK_SIZE);

    // Now we can use the default max block size.
    CheckBlockMaxSize(config, DEFAULT_MAX_BLOCK_SIZE - 1001,
                      DEFAULT_MAX_BLOCK_SIZE - 1001);
    CheckBlockMaxSize(config, DEFAULT_MAX_BLOCK_SIZE - 1000,
                      DEFAULT_MAX_BLOCK_SIZE - 1000);
    CheckBlockMaxSize(config, DEFAULT_MAX_BLOCK_SIZE - 999,
                      DEFAULT_MAX_BLOCK_SIZE - 1000);
    CheckBlockMaxSize(config, DEFAULT_MAX_BLOCK_SIZE,
                      DEFAULT_MAX_BLOCK_SIZE - 1000);

    // If the parameter is not specified, we use
    // DEFAULT_MAX_GENERATED_BLOCK_SIZE
    {
        gArgs.ClearArg("-blockmaxsize");
        BlockAssembler ba(config, g_mempool);
        BOOST_CHECK_EQUAL(ba.GetMaxGeneratedBlockSize(),
                          DEFAULT_MAX_GENERATED_BLOCK_SIZE - 1000);
    }
}

BOOST_AUTO_TEST_CASE(TestCBlockTemplateEntry) {
    const CTransaction tx;
    CTransactionRef txRef = MakeTransactionRef(tx);
    CBlockTemplateEntry txEntry(txRef, 1 * SATOSHI, 200, 10);
    BOOST_CHECK_MESSAGE(txEntry.tx == txRef, "Transactions did not match");
    BOOST_CHECK_EQUAL(txEntry.txFee, 1 * SATOSHI);
    BOOST_CHECK_EQUAL(txEntry.txSize, 200);
    BOOST_CHECK_EQUAL(txEntry.txSigOps, 10);
}

BOOST_AUTO_TEST_SUITE_END()
