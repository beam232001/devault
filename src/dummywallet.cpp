// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019 The DeVault developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <walletinitinterface.h>

class DummyWalletInit : public WalletInitInterface {
public:
    void AddWalletOptions() const override {}
    bool HasWalletSupport() const override { return false; }
    bool ParameterInteraction() const override { return true; }
    void RegisterRPC(CRPCTable &) const override {}
    bool Verify(const CChainParams &chainParams) const override { return true; }
    bool Open(const CChainParams &chainParams, const SecureString& walletPassphrase,
              const std::vector<std::string>& words) const override { return true; }
    bool CheckIfWalletExists(const CChainParams &chainParams) const override { return false; }
    void Start(CScheduler &scheduler) const override {}
    void Flush() const override {}
    void Stop() const override {}
    void Close() const override {}
};

const WalletInitInterface &g_wallet_init_interface = DummyWalletInit();
