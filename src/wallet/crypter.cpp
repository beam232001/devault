// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019 DeVault developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/crypter.h>

#include <crypto/aes.h>
#include <crypto/sha512.h>
#include <util.h>

#include <vector>

int CCrypter::BytesToKeySHA512AES(const std::vector<uint8_t> &chSalt,
                                  const SecureString &strKeyData, int count,
                                  uint8_t *key, uint8_t *iv) const {
    // This mimics the behavior of openssl's EVP_BytesToKey with an aes256cbc
    // cipher and sha512 message digest. Because sha512's output size (64b) is
    // greater than the aes256 block size (16b) + aes256 key size (32b), there's
    // no need to process more than once (D_0).
    if (!count || !key || !iv) {
        return 0;
    }

    uint8_t buf[CSHA512::OUTPUT_SIZE];
    CSHA512 di;

    di.Write((const uint8_t *)strKeyData.c_str(), strKeyData.size());
    if (chSalt.size()) {
        di.Write(&chSalt[0], chSalt.size());
    }
    di.Finalize(buf);

    for (int i = 0; i != count - 1; i++) {
        di.Reset().Write(buf, sizeof(buf)).Finalize(buf);
    }

    memcpy(key, buf, WALLET_CRYPTO_KEY_SIZE);
    memcpy(iv, buf + WALLET_CRYPTO_KEY_SIZE, WALLET_CRYPTO_IV_SIZE);
    memory_cleanse(buf, sizeof(buf));
    return WALLET_CRYPTO_KEY_SIZE;
}

bool CCrypter::SetKeyFromPassphrase(const SecureString &strKeyData,
                                    const std::vector<uint8_t> &chSalt,
                                    const unsigned int nRounds,
                                    const unsigned int nDerivationMethod) {
    if (nRounds < 1 || chSalt.size() != WALLET_CRYPTO_SALT_SIZE) {
        return false;
    }

    int i = 0;
    if (nDerivationMethod == 0) {
        i = BytesToKeySHA512AES(chSalt, strKeyData, nRounds, vchKey.data(),
                                vchIV.data());
    }

    if (i != (int)WALLET_CRYPTO_KEY_SIZE) {
        memory_cleanse(vchKey.data(), vchKey.size());
        memory_cleanse(vchIV.data(), vchIV.size());
        return false;
    }

    fKeySet = true;
    return true;
}

bool CCrypter::SetKey(const CKeyingMaterial &chNewKey,
                      const std::vector<uint8_t> &chNewIV) {
    if (chNewKey.size() != WALLET_CRYPTO_KEY_SIZE ||
        chNewIV.size() != WALLET_CRYPTO_IV_SIZE) {
        return false;
    }

    memcpy(vchKey.data(), chNewKey.data(), chNewKey.size());
    memcpy(vchIV.data(), chNewIV.data(), chNewIV.size());

    fKeySet = true;
    return true;
}

bool CCrypter::Encrypt(const CKeyingMaterial &vchPlaintext,
                       std::vector<uint8_t> &vchCiphertext) const {
    if (!fKeySet) {
        return false;
    }

    // max ciphertext len for a n bytes of plaintext is
    // n + AES_BLOCKSIZE bytes
    vchCiphertext.resize(vchPlaintext.size() + AES_BLOCKSIZE);

    AES256CBCEncrypt enc(vchKey.data(), vchIV.data(), true);
    size_t nLen =
        enc.Encrypt(&vchPlaintext[0], vchPlaintext.size(), &vchCiphertext[0]);
    if (nLen < vchPlaintext.size()) {
        return false;
    }
    vchCiphertext.resize(nLen);

    return true;
}

bool CCrypter::Decrypt(const std::vector<uint8_t> &vchCiphertext,
                       CKeyingMaterial &vchPlaintext) const {
    if (!fKeySet) {
        return false;
    }

    // plaintext will always be equal to or lesser than length of ciphertext
    int nLen = vchCiphertext.size();

    vchPlaintext.resize(nLen);

    AES256CBCDecrypt dec(vchKey.data(), vchIV.data(), true);
    nLen =
        dec.Decrypt(&vchCiphertext[0], vchCiphertext.size(), &vchPlaintext[0]);
    if (nLen == 0) {
        return false;
    }
    vchPlaintext.resize(nLen);
    return true;
}

static bool EncryptSecret(const CKeyingMaterial &vMasterKey,
                          const CKeyingMaterial &vchPlaintext,
                          const uint256 &nIV,
                          std::vector<uint8_t> &vchCiphertext) {
    CCrypter cKeyCrypter;
    std::vector<uint8_t> chIV(WALLET_CRYPTO_IV_SIZE);
    memcpy(&chIV[0], &nIV, WALLET_CRYPTO_IV_SIZE);
    if (!cKeyCrypter.SetKey(vMasterKey, chIV)) {
        return false;
    }
    return cKeyCrypter.Encrypt(*((const CKeyingMaterial *)&vchPlaintext),
                               vchCiphertext);
}

static bool DecryptSecret(const CKeyingMaterial &vMasterKey,
                          const std::vector<uint8_t> &vchCiphertext,
                          const uint256 &nIV, CKeyingMaterial &vchPlaintext) {
    CCrypter cKeyCrypter;
    std::vector<uint8_t> chIV(WALLET_CRYPTO_IV_SIZE);
    memcpy(&chIV[0], &nIV, WALLET_CRYPTO_IV_SIZE);
    if (!cKeyCrypter.SetKey(vMasterKey, chIV)) {
        return false;
    }
    return cKeyCrypter.Decrypt(vchCiphertext,
                               *((CKeyingMaterial *)&vchPlaintext));
}

static bool DecryptKey(const CKeyingMaterial &vMasterKey,
                       const std::vector<uint8_t> &vchCryptedSecret,
                       const CPubKey &vchPubKey, CKey &key) {
    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, vchPubKey.GetHash(),
                       vchSecret)) {
        return false;
    }

    if (vchSecret.size() != 32) {
        return false;
    }

    key.Set(vchSecret.begin(), vchSecret.end());
    return key.VerifyPubKey(vchPubKey);
}

bool CCryptoKeyStore::SetCrypted() {
    LOCK(cs_KeyStore);
    if (fUseCrypto) {
        return true;
    }
    if (!mapKeys.empty()) {
        return false;
    }
    fUseCrypto = true;
    return true;
}

bool CCryptoKeyStore::IsLocked() const {
    if (!IsCrypted()) {
        return false;
    }
    LOCK(cs_KeyStore);
    return vMasterKey.empty();
}

bool CCryptoKeyStore::Lock() {
    if (!SetCrypted()) {
        return false;
    }

    {
        LOCK(cs_KeyStore);
        vMasterKey.clear();
    }

    NotifyStatusChanged(this);
    return true;
}

bool CCryptoKeyStore::Unlock(const CKeyingMaterial &vMasterKeyIn) {
    {
        LOCK(cs_KeyStore);
        if (!SetCrypted()) {
            return false;
        }

        bool keyPass = false;
        bool keyFail = false;
        auto mi = mapCryptedKeys.begin();
        for (; mi != mapCryptedKeys.end(); ++mi) {
            const CPubKey &vchPubKey = (*mi).second.first;
            const std::vector<uint8_t> &vchCryptedSecret = (*mi).second.second;
            CKey key;
            if (!DecryptKey(vMasterKeyIn, vchCryptedSecret, vchPubKey, key)) {
                keyFail = true;
                break;
            }
            keyPass = true;
            if (fDecryptionThoroughlyChecked) {
                break;
            }
        }
        if (keyPass && keyFail) {
            LogPrintf("The wallet is probably corrupted: Some keys decrypt but "
                      "not all.\n");
            assert(false);
        }
        if (keyFail || (!keyPass & cryptedHDChain.IsNull())) {
            return false;
        }
        vMasterKey = vMasterKeyIn;
        if(!cryptedHDChain.IsNull()) {
          bool chainPass = false;
          // try to decrypt seed and make sure it matches
          CHDChain hdChainTmp;
          if (DecryptHDChain(hdChainTmp)) {
            // make sure seed matches this chain
            chainPass = cryptedHDChain.GetID() == hdChainTmp.GetSeedHash();
          }
          if (!chainPass) {
            vMasterKey.clear();
            return false;
          }
        }
        fDecryptionThoroughlyChecked = true;
    }
    NotifyStatusChanged(this);
    return true;
}

bool CCryptoKeyStore::AddKeyPubKey(const CKey &key, const CPubKey &pubkey) {
    LOCK(cs_KeyStore);
    if (!IsCrypted()) {
        return CBasicKeyStore::AddKeyPubKey(key, pubkey);
    }

    if (IsLocked()) {
        return false;
    }

    std::vector<uint8_t> vchCryptedSecret;
    CKeyingMaterial vchSecret(key.begin(), key.end());
    if (!EncryptSecret(vMasterKey, vchSecret, pubkey.GetHash(),
                       vchCryptedSecret)) {
        return false;
    }

    if (!AddCryptedKey(pubkey, vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool CCryptoKeyStore::AddCryptedKey(
    const CPubKey &vchPubKey, const std::vector<uint8_t> &vchCryptedSecret) {
    LOCK(cs_KeyStore);
    if (!SetCrypted()) {
        return false;
    }

    mapCryptedKeys[vchPubKey.GetID()] = make_pair(vchPubKey, vchCryptedSecret);
    ImplicitlyLearnRelatedKeyScripts(vchPubKey);
    return true;
}

bool CCryptoKeyStore::HaveKey(const CKeyID &address) const {
    LOCK(cs_KeyStore);
    if (!IsCrypted()) {
        return CBasicKeyStore::HaveKey(address);
    }
    return mapCryptedKeys.count(address) > 0;
}

bool CCryptoKeyStore::GetKey(const CKeyID &address, CKey &keyOut) const {
    LOCK(cs_KeyStore);
    if (!IsCrypted()) {
        return CBasicKeyStore::GetKey(address, keyOut);
    }

    auto mi = mapCryptedKeys.find(address);
    if (mi != mapCryptedKeys.end()) {
        const CPubKey &vchPubKey = (*mi).second.first;
        const std::vector<uint8_t> &vchCryptedSecret = (*mi).second.second;
        return DecryptKey(vMasterKey, vchCryptedSecret, vchPubKey, keyOut);
    }
    return false;
}

bool CCryptoKeyStore::GetPubKey(const CKeyID &address,
                                CPubKey &vchPubKeyOut) const {
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted()) {
            return CBasicKeyStore::GetPubKey(address, vchPubKeyOut);
        }

        auto mi = mapCryptedKeys.find(address);
        if (mi != mapCryptedKeys.end()) {
            vchPubKeyOut = (*mi).second.first;
            return true;
        }
        // Check for watch-only pubkeys
        return CBasicKeyStore::GetPubKey(address, vchPubKeyOut);
    }
}

std::set<CKeyID> CCryptoKeyStore::GetKeys() const {
    LOCK(cs_KeyStore);
    if (!IsCrypted()) {
        return CBasicKeyStore::GetKeys();
    }
    std::set<CKeyID> set_address;
    for (const auto &mi : mapCryptedKeys) {
        set_address.insert(mi.first);
    }
    return set_address;
}

bool CCryptoKeyStore::EncryptHDChain(const CKeyingMaterial& vMasterKeyIn, const CHDChain& hdc)
{
  SetCrypted();
  
  CHDChain hdChain = hdc;
  std::vector<unsigned char> vchCryptedSeed;
  if (!EncryptSecret(vMasterKeyIn, hdChain.GetSeed(), hdChain.GetID(), vchCryptedSeed))
    return false;
  
  cryptedHDChain = hdChain; // Will preserve the ID
  cryptedHDChain.SetCrypted(true);
  
  SecureString strMnemonic;
  std::vector<unsigned char> vchCryptedMnemonic;
  hdChain.GetMnemonic(strMnemonic);
  SecureVector vchMnemonic(strMnemonic.begin(), strMnemonic.end());
  
  if (!vchMnemonic.empty() && !EncryptSecret(vMasterKeyIn, vchMnemonic, hdChain.GetID(), vchCryptedMnemonic))
    return false;
  
  // Convert to SecureVectors for SetupCrypted
  SecureVector vchSecureCryptedMnemonic(vchCryptedMnemonic.begin(), vchCryptedMnemonic.end());
  SecureVector vchSecureCryptedSeed(vchCryptedSeed.begin(), vchCryptedSeed.end());
  
  cryptedHDChain.SetupCrypted(vchSecureCryptedMnemonic, vchSecureCryptedSeed);
  
  return true;
}

bool CCryptoKeyStore::DecryptHDChain(CHDChain& hdChainRet) const
{
    if (!IsCrypted())
        return true;

    if (cryptedHDChain.IsNull())
        return false;

    if (!cryptedHDChain.IsCrypted())
        return false;

    SecureVector vchSecureSeed;
    SecureVector vchSecureCryptedSeed = cryptedHDChain.GetSeed();
    std::vector<unsigned char> vchCryptedSeed(vchSecureCryptedSeed.begin(), vchSecureCryptedSeed.end());
    if (!DecryptSecret(vMasterKey, vchCryptedSeed, cryptedHDChain.GetID(), vchSecureSeed))
        return false;

    hdChainRet = cryptedHDChain;
    if (!hdChainRet.SetSeed(vchSecureSeed, false))
        return false;

    // hash of decrypted seed must match chain id
    if (hdChainRet.GetSeedHash() != cryptedHDChain.GetID())
        return false;

    SecureVector vchSecureCryptedMnemonic;
    if (cryptedHDChain.GetMnemonic(vchSecureCryptedMnemonic)) {
      SecureVector vchSecureMnemonic;
      std::vector<unsigned char> vchCryptedMnemonic(vchSecureCryptedMnemonic.begin(), vchSecureCryptedMnemonic.end());
      if (!vchCryptedMnemonic.empty() && !DecryptSecret(vMasterKey, vchCryptedMnemonic, cryptedHDChain.GetID(), vchSecureMnemonic))
        return false;
      hdChainRet.SetMnemonic(vchSecureMnemonic);
    }
  
    hdChainRet.SetCrypted(false);

    return true;
}


bool CCryptoKeyStore::SetCryptedHDChain(const CHDChain& chain)
{
    if (!SetCrypted())
        return false;

    if (!chain.IsCrypted())
        return false;

    cryptedHDChain = chain;
    return true;
}
bool CCryptoKeyStore::GetCryptedHDChain(CHDChain& hdChainRet) const
{
  if(IsCrypted()) {
    hdChainRet = cryptedHDChain;
    return !cryptedHDChain.IsNull();
  }
  
  return false;
}
bool CCryptoKeyStore::GetDecryptedHDChain(CHDChain& hdChainRet) const
{
  if(IsCrypted()) {
    hdChainRet = cryptedHDChain;
    if (!DecryptHDChain(hdChainRet)) {
        return false;
    }
    return !cryptedHDChain.IsNull();
  }
  
  return false;
}
