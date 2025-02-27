// Copyright (c) 2017 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_CASHADDRENC_H
#define BITCOIN_CASHADDRENC_H

#include <script/standard.h>
#include <key.h>
#include <string>
#include <vector>

class CChainParams;
class CTxOut;

enum CashAddrType : uint8_t { PUBKEY_TYPE = 0, SCRIPT_TYPE = 1, SECRET_TYPE = 2 };

struct CashAddrContent {
    CashAddrType type;
    std::vector<uint8_t> hash;
};

std::string GetAddrFromTxOut(const CTxOut& out);

std::string EncodeCashAddr(const CTxDestination &, const CChainParams &);
std::string EncodeCashAddr(const std::string &prefix,
                           const CashAddrContent &content);

CTxDestination DecodeCashAddr(const std::string &addr,
                              const CChainParams &params);
CashAddrContent DecodeCashAddrContent(const std::string &addr,
                                      const std::string &prefix);
CTxDestination DecodeCashAddrDestination(const CashAddrContent &content);

std::vector<uint8_t> PackCashAddrContent(const CashAddrContent &content);

CKey DecodeSecret(const std::string &addr);
std::string EncodeSecret(const CKey& key);

#endif
