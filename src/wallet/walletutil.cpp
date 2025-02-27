// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util.h>
#include <wallet/walletutil.h>

fs::path GetWalletDir() {
    fs::path path;

    if (gArgs.IsArgSet("-walletdir")) {
        path = gArgs.GetArg("-walletdir", "");
        if (!fs::is_directory(path)) {
            // If the path specified doesn't exist, we return the deliberately
            // invalid empty string.
            path = "";
        }
    } else {
        path = GetDataDir();
        // Always use a wallets directory
        path /= "wallets";
    }

    return path;
}
fs::path GetWalletDirNoCreate(fs::path& added_dir) {
  fs::path path;
  
  if (gArgs.IsArgSet("-walletdir")) {
    path = gArgs.GetArg("-walletdir", "");
    if (!fs::is_directory(path)) {
      // If the path specified doesn't exist, we return the deliberately
      // invalid empty string.
      path = "";
    }
  } else {
    path = GetDataDirNoCreate();
    
    // This will be Net specific addition
    if (added_dir != "") {
      path /= added_dir;
    }
  
    // Always assume a wallets directory
    path /= "wallets";
  }
  
  return path;
}
