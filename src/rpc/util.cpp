// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dstencode.h>
#include <keystore.h>
#include <pubkey.h>
#include <rpc/protocol.h>
#include <rpc/util.h>
#include <tinyformat.h>
#include <utilstrencodings.h>

// Converts a hex string to a public key if possible
CPubKey HexToPubKey(const std::string &hex_in) {
    if (!IsHex(hex_in)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           "Invalid public key: " + hex_in);
    }
    CPubKey vchPubKey(ParseHex(hex_in));
    if (!vchPubKey.IsFullyValid()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           "Invalid public key: " + hex_in);
    }
    return vchPubKey;
}

// Retrieves a public key for an address from the given CKeyStore
CPubKey AddrToPubKey(const CChainParams &chainparams, CKeyStore *const keystore,
                     const std::string &addr_in) {
    CTxDestination dest = DecodeDestination(addr_in, chainparams);
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           "Invalid address: " + addr_in);
    }
#ifdef HAVE_VARIANT
    const CKeyID *keyID = &std::get<CKeyID>(dest);
#else
    const CKeyID *keyID = boost::get<CKeyID>(&dest);
#endif
    if (!keyID) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           strprintf("%s does not refer to a key", addr_in));
    }
    CPubKey vchPubKey;
    if (!keystore->GetPubKey(*keyID, vchPubKey)) {
        throw JSONRPCError(
            RPC_INVALID_ADDRESS_OR_KEY,
            strprintf("no full public key for address %s", addr_in));
    }
    if (!vchPubKey.IsFullyValid()) {
        throw JSONRPCError(RPC_INTERNAL_ERROR,
                           "Wallet contains an invalid public key");
    }
    return vchPubKey;
}

// Creates a multisig redeemscript from a given list of public keys and number
// required.
CScript CreateMultisigRedeemscript(const int required,
                                   const std::vector<CPubKey> &pubkeys) {
    // Gather public keys
    if (required < 1) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            "a multisignature address must require at least one key to redeem");
    }
    if ((int)pubkeys.size() < required) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           strprintf("not enough keys supplied (got %u keys, "
                                     "but need at least %d to redeem)",
                                     pubkeys.size(), required));
    }
    if (pubkeys.size() > 16) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "Number of keys involved in the multisignature "
                           "address creation > 16\nReduce the number");
    }

    CScript result = GetScriptForMultisig(required, pubkeys);

    if (result.size() > MAX_SCRIPT_ELEMENT_SIZE) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            (strprintf("redeemScript exceeds size limit: %d > %d",
                       result.size(), MAX_SCRIPT_ELEMENT_SIZE)));
    }

    return result;
}
