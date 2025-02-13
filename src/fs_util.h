// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <fs.h>

extern const char *const BITCOIN_PID_FILENAME;

struct CDiskBlockPos;

fs::path GetDefaultDataDir();
fs::path GetDebugLogPath();
/** Get name of RPC authentication cookie file */
fs::path GetAuthCookieFile();
bool CheckIfWalletDatExists(bool fNetSpecific = true);
const fs::path &GetDataDir(bool fNetSpecific = true);
const fs::path GetDataDirNoCreate();
void ClearDatadirCache();
fs::path GetConfigFile(const std::string &confPath);
bool RenameOver(const fs::path& src, const fs::path& dest);
bool TryCreateDirectories(const fs::path &p);
void FileCommit(FILE *file);
bool TruncateFile(FILE *file, unsigned int length);
void AllocateFileRange(FILE *file, unsigned int offset, unsigned int length);
void SetupEnvironment();
bool LockDirectory(const fs::path &directory, const std::string lockfile_name, bool probe_only);
void ReleaseDirectoryLocks();
bool LockDataDirectory(bool probeOnly);
bool DirIsWritable(const fs::path &directory);

// in validation.cpp
/** Translation to a filesystem path. */
fs::path GetBlockPosFilename(const CDiskBlockPos &pos, const char *prefix);

/* Only used in Windows */
fs::path GetSpecialFolderPath(int nFolder, bool fCreate = true);
fs::path GetPidFile();
const fs::path &GetBlocksDir(bool fNetSpecific = true);
void CreatePidFile(const fs::path &path, pid_t pid);

