// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2016-2023 The Hush developers
// Distributed under the GPLv3 software license, see the accompanying
// file COPYING or https://www.gnu.org/licenses/gpl-3.0.en.html

/******************************************************************************
 * Copyright © 2014-2019 The SuperNET Developers.                             *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * SuperNET software, including this file may be copied, modified, propagated *
 * or distributed except according to the terms contained in the LICENSE file *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/
#include "wallet/wallet.h"
#include "asyncrpcqueue.h"
#include "checkpoints.h"
#include "coincontrol.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "consensus/consensus.h"
#include "init.h"
#include "key_io.h"
#include "main.h"
#include "net.h"
#include "rpc/protocol.h"
#include "script/script.h"
#include "script/sign.h"
#include "timedata.h"
#include "utilmoneystr.h"
#include "zcash/Note.hpp"
#include "crypter.h"
#include "coins.h"
#include "wallet/asyncrpcoperation_saplingconsolidation.h"
#include "wallet/asyncrpcoperation_sweep.h"
#include "zcash/zip32.h"
#include "cc/CCinclude.h"
#include <assert.h>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

using namespace std;
using namespace libzcash;

/**
 * Settings
 */
const char * DEFAULT_WALLET_DAT = "wallet.dat";
CFeeRate payTxFee(DEFAULT_TRANSACTION_FEE);
CAmount maxTxFee = DEFAULT_TRANSACTION_MAXFEE;
unsigned int nTxConfirmTarget = DEFAULT_TX_CONFIRM_TARGET;
bool bSpendZeroConfChange = true;
bool fSendFreeTransactions = false;
bool fPayAtLeastCustomFee = true;
#include "hush_defs.h"

CBlockIndex *hush_chainactive(int32_t height);
extern std::string DONATION_PUBKEY;
extern int32_t HUSH_LOADINGBLOCKS;
int32_t hush_dpowconfs(int32_t height,int32_t numconfs);
int tx_height( const uint256 &hash );
bool fTxDeleteEnabled = false;
bool fTxConflictDeleteEnabled = false;
int fDeleteInterval = DEFAULT_TX_DELETE_INTERVAL;
unsigned int fDeleteTransactionsAfterNBlocks = DEFAULT_TX_RETENTION_BLOCKS;
unsigned int fKeepLastNTransactions = DEFAULT_TX_RETENTION_LASTTX;

/**
 * Fees smaller than this (in satoshi) are considered zero fee (for transaction creation)
 * Override with -mintxfee
 */
CFeeRate CWallet::minTxFee = CFeeRate(1000);

/** @defgroup mapWallet
 *
 * @{
 */

struct CompareValueOnly
{
    bool operator()(const pair<CAmount, pair<const CWalletTx*, unsigned int> >& t1,
                    const pair<CAmount, pair<const CWalletTx*, unsigned int> >& t2) const
    {
        return t1.first < t2.first;
    }
};

std::string JSOutPoint::ToString() const
{
    return strprintf("JSOutPoint(%s, %d, %d)", hash.ToString().substr(0,10), js, n);
}

std::string COutput::ToString() const
{
    return strprintf("COutput(%s, %d, %d) [%s]", tx->GetHash().ToString(), i, nDepth, FormatMoney(tx->vout[i].nValue));
}

const CWalletTx* CWallet::GetWalletTx(const uint256& hash) const
{
    LOCK(cs_wallet);
    std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(hash);
    if (it == mapWallet.end())
        return NULL;
    return &(it->second);
}


// Generate a new Sapling spending key and return its public payment address
SaplingPaymentAddress CWallet::GenerateNewSaplingZKey(bool addToWallet)
{
    AssertLockHeld(cs_wallet); // mapSaplingZKeyMetadata

    // Create new metadata
    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    // Try to get the seed
    HDSeed seed;
    if (!GetHDSeed(seed))
        throw std::runtime_error("CWallet::GenerateNewSaplingZKey(): HD seed not found");

    auto m = libzcash::SaplingExtendedSpendingKey::Master(seed);
    uint32_t bip44CoinType = Params().BIP44CoinType();

    // We use a fixed keypath scheme of m/32'/coin_type'/account'
    // Derive m/32'
    auto m_32h = m.Derive(32 | ZIP32_HARDENED_KEY_LIMIT);
    // Derive m/32'/coin_type'
    auto m_32h_cth = m_32h.Derive(bip44CoinType | ZIP32_HARDENED_KEY_LIMIT);

    // Derive account key at next index, skip keys already known to the wallet
    libzcash::SaplingExtendedSpendingKey xsk;
    do
    {
        xsk = m_32h_cth.Derive(hdChain.saplingAccountCounter | ZIP32_HARDENED_KEY_LIMIT);
        metadata.hdKeypath = "m/32'/" + std::to_string(bip44CoinType) + "'/" + std::to_string(hdChain.saplingAccountCounter) + "'";
        metadata.seedFp = hdChain.seedFp;
        // Increment childkey index
        hdChain.saplingAccountCounter++;
    } while (HaveSaplingSpendingKey(xsk.expsk.full_viewing_key()));

    // Update the chain model in the database
    if (fFileBacked && !CWalletDB(strWalletFile).WriteHDChain(hdChain))
        throw std::runtime_error("CWallet::GenerateNewSaplingZKey(): Writing HD chain model failed");

    auto ivk = xsk.expsk.full_viewing_key().in_viewing_key();
    mapSaplingZKeyMetadata[ivk] = metadata;

    auto addr = xsk.DefaultAddress();
    if (addToWallet && !AddSaplingZKey(xsk, addr)) {
        throw std::runtime_error("CWallet::GenerateNewSaplingZKey(): AddSaplingZKey failed");
    }
    // return default sapling payment address.
    return addr;
}

// Add spending key to keystore
bool CWallet::AddSaplingZKey(
    const libzcash::SaplingExtendedSpendingKey &sk,
    const libzcash::SaplingPaymentAddress &defaultAddr)
{
    AssertLockHeld(cs_wallet); // mapSaplingZKeyMetadata

    if (!CCryptoKeyStore::AddSaplingSpendingKey(sk, defaultAddr)) {
        return false;
    }

    nTimeFirstKey = 1; // No birthday information for viewing keys.
    if (!fFileBacked) {
        return true;
    }

    if (!IsCrypted()) {
        auto ivk = sk.expsk.full_viewing_key().in_viewing_key();
        return CWalletDB(strWalletFile).WriteSaplingZKey(ivk, sk, mapSaplingZKeyMetadata[ivk]);
    }

    return true;
}

// Add payment address -> incoming viewing key map entry
bool CWallet::AddSaplingIncomingViewingKey(
    const libzcash::SaplingIncomingViewingKey &ivk,
    const libzcash::SaplingPaymentAddress &addr)
{
    AssertLockHeld(cs_wallet); // mapSaplingZKeyMetadata

    if (!CCryptoKeyStore::AddSaplingIncomingViewingKey(ivk, addr)) {
        return false;
    }

    if (!fFileBacked) {
        return true;
    }

    if (!IsCrypted()) {
        return CWalletDB(strWalletFile).WriteSaplingPaymentAddress(addr, ivk);
    }

    return true;
}


CPubKey CWallet::GenerateNewKey()
{
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    bool fCompressed = CanSupportFeature(FEATURE_COMPRPUBKEY); // default to compressed public keys if we want 0.6.0 wallets

    CKey secret;
    secret.MakeNewKey(fCompressed);

    // Compressed public keys were introduced in version 0.6.0
    if (fCompressed)
        SetMinVersion(FEATURE_COMPRPUBKEY);

    CPubKey pubkey = secret.GetPubKey();
    assert(secret.VerifyPubKey(pubkey));

    // Create new metadata
    int64_t nCreationTime = GetTime();
    mapKeyMetadata[pubkey.GetID()] = CKeyMetadata(nCreationTime);
    if (!nTimeFirstKey || nCreationTime < nTimeFirstKey)
        nTimeFirstKey = nCreationTime;

    if (!AddKeyPubKey(secret, pubkey))
        throw std::runtime_error("CWallet::GenerateNewKey(): AddKey failed");
    return pubkey;
}

bool CWallet::AddKeyPubKey(const CKey& secret, const CPubKey &pubkey)
{
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    if (!CCryptoKeyStore::AddKeyPubKey(secret, pubkey))
        return false;

    // check if we need to remove from watch-only
    CScript script;
    script = GetScriptForDestination(pubkey.GetID());
    if (HaveWatchOnly(script))
        RemoveWatchOnly(script);

    if (!fFileBacked)
        return true;
    if (!IsCrypted()) {
        return CWalletDB(strWalletFile).WriteKey(pubkey,
                                                 secret.GetPrivKey(),
                                                 mapKeyMetadata[pubkey.GetID()]);
    }
    return true;
}

bool CWallet::AddCryptedKey(const CPubKey &vchPubKey,
                            const vector<unsigned char> &vchCryptedSecret)
{

    if (!CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret))
        return false;
    if (!fFileBacked)
        return true;
    {
        LOCK(cs_wallet);
        if (pwalletdbEncryption)
            return pwalletdbEncryption->WriteCryptedKey(vchPubKey,
                                                        vchCryptedSecret,
                                                        mapKeyMetadata[vchPubKey.GetID()]);
        else
            return CWalletDB(strWalletFile).WriteCryptedKey(vchPubKey,
                                                            vchCryptedSecret,
                                                            mapKeyMetadata[vchPubKey.GetID()]);
    }
    return false;
}

bool CWallet::AddCryptedSaplingSpendingKey(const libzcash::SaplingExtendedFullViewingKey &extfvk,
                                           const std::vector<unsigned char> &vchCryptedSecret,
                                           const libzcash::SaplingPaymentAddress &defaultAddr)
{
    if (!CCryptoKeyStore::AddCryptedSaplingSpendingKey(extfvk, vchCryptedSecret, defaultAddr))
        return false;
    if (!fFileBacked)
        return true;
    {
        LOCK(cs_wallet);
        if (pwalletdbEncryption) {
            return pwalletdbEncryption->WriteCryptedSaplingZKey(extfvk,
                                                         vchCryptedSecret,
                                                         mapSaplingZKeyMetadata[extfvk.fvk.in_viewing_key()]);
        } else {
            return CWalletDB(strWalletFile).WriteCryptedSaplingZKey(extfvk,
                                                         vchCryptedSecret,
                                                         mapSaplingZKeyMetadata[extfvk.fvk.in_viewing_key()]);
        }
    }
    return false;
}

bool CWallet::LoadKeyMetadata(const CPubKey &pubkey, const CKeyMetadata &meta)
{
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    if (meta.nCreateTime && (!nTimeFirstKey || meta.nCreateTime < nTimeFirstKey))
        nTimeFirstKey = meta.nCreateTime;

    mapKeyMetadata[pubkey.GetID()] = meta;
    return true;
}

bool CWallet::LoadCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret)
{
    return CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret);
}


bool CWallet::LoadCryptedSaplingZKey(
    const libzcash::SaplingExtendedFullViewingKey &extfvk,
    const std::vector<unsigned char> &vchCryptedSecret)
{
     return CCryptoKeyStore::AddCryptedSaplingSpendingKey(extfvk, vchCryptedSecret, extfvk.DefaultAddress());
}

bool CWallet::LoadSaplingZKeyMetadata(const libzcash::SaplingIncomingViewingKey &ivk, const CKeyMetadata &meta)
{
    AssertLockHeld(cs_wallet); // mapSaplingZKeyMetadata
    mapSaplingZKeyMetadata[ivk] = meta;
    return true;
}

bool CWallet::LoadSaplingZKey(const libzcash::SaplingExtendedSpendingKey &key)
{
    return CCryptoKeyStore::AddSaplingSpendingKey(key, key.DefaultAddress());
}

bool CWallet::LoadSaplingPaymentAddress(
    const libzcash::SaplingPaymentAddress &addr,
    const libzcash::SaplingIncomingViewingKey &ivk)
{
    return CCryptoKeyStore::AddSaplingIncomingViewingKey(ivk, addr);
}

bool CWallet::AddCScript(const CScript& redeemScript)
{
    if (!CCryptoKeyStore::AddCScript(redeemScript))
        return false;
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteCScript(Hash160(redeemScript), redeemScript);
}

bool CWallet::LoadCScript(const CScript& redeemScript)
{
    /* A sanity check was added in pull #3843 to avoid adding redeemScripts
     * that never can be redeemed. However, old wallets may still contain
     * these. Do not add them to the wallet and warn. */
    if (redeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE)
    {
        std::string strAddr = EncodeDestination(CScriptID(redeemScript));
        LogPrintf("%s: Warning: This wallet contains a redeemScript of size %i which exceeds maximum size %i thus can never be redeemed. Do not use address %s.\n",
            __func__, redeemScript.size(), MAX_SCRIPT_ELEMENT_SIZE, strAddr);
        return true;
    }

    return CCryptoKeyStore::AddCScript(redeemScript);
}

bool CWallet::AddWatchOnly(const CScript &dest)
{
    if (!CCryptoKeyStore::AddWatchOnly(dest))
        return false;
    nTimeFirstKey = 1; // No birthday information for watch-only keys.
    NotifyWatchonlyChanged(true);
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteWatchOnly(dest);
}

bool CWallet::RemoveWatchOnly(const CScript &dest)
{
    AssertLockHeld(cs_wallet);
    if (!CCryptoKeyStore::RemoveWatchOnly(dest))
        return false;
    if (!HaveWatchOnly())
        NotifyWatchonlyChanged(false);
    if (fFileBacked)
        if (!CWalletDB(strWalletFile).EraseWatchOnly(dest))
            return false;

    return true;
}

bool CWallet::LoadWatchOnly(const CScript &dest)
{
    return CCryptoKeyStore::AddWatchOnly(dest);
}

bool CWallet::Unlock(const SecureString& strWalletPassphrase)
{
    CCrypter crypter;
    CKeyingMaterial vMasterKey;

    {
        LOCK(cs_wallet);
        BOOST_FOREACH(const MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                continue; // try another master key
            if (CCryptoKeyStore::Unlock(vMasterKey))
                return true;
        }
    }
    return false;
}

bool CWallet::ChangeWalletPassphrase(const SecureString& strOldWalletPassphrase, const SecureString& strNewWalletPassphrase)
{
    bool fWasLocked = IsLocked();

    {
        LOCK(cs_wallet);
        Lock();

        CCrypter crypter;
        CKeyingMaterial vMasterKey;
        BOOST_FOREACH(MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strOldWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;
            if (CCryptoKeyStore::Unlock(vMasterKey))
            {
                int64_t nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = pMasterKey.second.nDeriveIterations * (100 / ((double)(GetTimeMillis() - nStartTime)));

                nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations + pMasterKey.second.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                LogPrintf("Wallet passphrase changed to an nDeriveIterations of %i\n", pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                    return false;
                if (!crypter.Encrypt(vMasterKey, pMasterKey.second.vchCryptedKey))
                    return false;
                CWalletDB(strWalletFile).WriteMasterKey(pMasterKey.first, pMasterKey.second);
                if (fWasLocked)
                    Lock();
                return true;
            }
        }
    }

    return false;
}

void CWallet::ChainTip(const CBlockIndex *pindex,
                       const CBlock *pblock,
                       boost::optional<std::pair<SproutMerkleTree, SaplingMerkleTree>> added)
{
    if (added) {
        bool initialDownloadCheck = IsInitialBlockDownload();
        // Prevent witness cache building && consolidation transactions
        // from being created when node is syncing after launch,
        // and also when node wakes up from suspension/hibernation and incoming blocks are old.
        // 144 blocks = 3hrs @ 75s blocktime
        if (!initialDownloadCheck &&
            pblock->GetBlockTime() > GetTime() - 144*ASSETCHAINS_BLOCKTIME)
        {
            BuildWitnessCache(pindex, false);
            if (fSaplingConsolidationEnabled) {
                RunSaplingConsolidation(pindex->GetHeight());
            }
            if (fSweepEnabled) {
                RunSaplingSweep(pindex->GetHeight());
            }
            if (fTxDeleteEnabled) {
                DeleteWalletTransactions(pindex);
            }
        } else {
            //Build initial witnesses on every block
            BuildWitnessCache(pindex, true);
            if (fTxDeleteEnabled) {
                if (initialDownloadCheck && pindex->GetHeight() % fDeleteInterval == 0) {
                    DeleteWalletTransactions(pindex);
                }
            }
        }
    } else {
        DecrementNoteWitnesses(pindex);
        UpdateNullifierNoteMapForBlock(pblock);
    }
}

void CWallet::RunSaplingSweep(int blockHeight) {
    // Sapling is always active since height=1 of HUSH+HSCs
    // if (!NetworkUpgradeActive(blockHeight, Params().GetConsensus(), Consensus::UPGRADE_SAPLING)) {
    //     return;
    // }
    AssertLockHeld(cs_wallet);
    if (!fSweepEnabled) {
        return;
    }

    if (nextSweep > blockHeight) {
        LogPrintf("%s: Not time to sweep yet at blockHeight=%d nextSweep=%d\n", __func__,  blockHeight, nextSweep);
        return;
    }
    LogPrintf("%s: Sweep enabled at blockHeight=%d nextSweep=%d\n", __func__,  blockHeight, nextSweep);

    //Don't Run if consolidation will run soon.
    if (fSaplingConsolidationEnabled && nextConsolidation - 5 <= blockHeight) {
        LogPrintf("%s: not sweeping since next consolidation is within 5 blocks, nextConsolidation=%d , blockHeight=%d\n", __func__, nextConsolidation, blockHeight);
        return;
    }

    //Don't Run While consolidation is running.
    if (fConsolidationRunning) {
        LogPrintf("%s: not sweeping since consolidation is currently running at height=%d\n", __func__, blockHeight);
        return;
    }

    fSweepRunning = true;

    std::shared_ptr<AsyncRPCQueue> q = getAsyncRPCQueue();
    std::shared_ptr<AsyncRPCOperation> lastOperation = q->getOperationForId(saplingSweepOperationId);
    if (lastOperation != nullptr) {
        lastOperation->cancel();
    }
    pendingSaplingSweepTxs.clear();
    std::shared_ptr<AsyncRPCOperation> operation(new AsyncRPCOperation_sweep(blockHeight + 5));
    saplingSweepOperationId = operation->getId();
    q->addOperation(operation);
}

void CWallet::RunSaplingConsolidation(int blockHeight) {
    // Sapling is always active on HUSH+HSCs
    //if (!NetworkUpgradeActive(blockHeight, Params().GetConsensus(), Consensus::UPGRADE_SAPLING)) {
    //    return;
    //}

    LOCK(cs_wallet);

    if (!fSaplingConsolidationEnabled) {
        return;
    }

    if (nextConsolidation > blockHeight) {
        LogPrintf("%s: Not time to consolidate yet at blockHeight=%d nextConsolidation=%d\n", __func__, blockHeight, nextConsolidation);
        return;
    }

    LogPrintf("%s: consolidation enabled at blockHeight=%d fSweepRunning=%d\n", __func__,  blockHeight, fSweepRunning );

    if (fSweepRunning) {
        LogPrintf("%s: not consolidating since sweep is currently running at height=%d\n", __func__, blockHeight);
        return;
    }

    LogPrintf("%s:  creating consolidation operation at blockHeight=%d\n", __func__,  blockHeight);
    std::shared_ptr<AsyncRPCQueue> q = getAsyncRPCQueue();
    std::shared_ptr<AsyncRPCOperation> lastOperation = q->getOperationForId(saplingConsolidationOperationId);
    if (lastOperation != nullptr) {
        lastOperation->cancel();
    }
    pendingSaplingConsolidationTxs.clear();
    std::shared_ptr<AsyncRPCOperation> operation(new AsyncRPCOperation_saplingconsolidation(blockHeight + 5));
    saplingConsolidationOperationId = operation->getId();
    q->addOperation(operation);
}

bool CWallet::CommitAutomatedTx(const CTransaction& tx) {
  CWalletTx wtx(this, tx);
  CReserveKey reservekey(pwalletMain);
  fprintf(stderr,"%s: %s\n",__func__,tx.ToString().c_str());
  return CommitTransaction(wtx, reservekey);
}

void CWallet::SetBestChain(const CBlockLocator& loc)
{
    CWalletDB walletdb(strWalletFile);
    SetBestChainINTERNAL(walletdb, loc);
}

std::set<std::pair<libzcash::PaymentAddress, uint256>> CWallet::GetNullifiersForAddresses(
        const std::set<libzcash::PaymentAddress> & addresses)
{
    std::set<std::pair<libzcash::PaymentAddress, uint256>> nullifierSet;
    // Sapling ivk -> list of addrs map
    // (There may be more than one diversified address for a given ivk.)
    std::map<libzcash::SaplingIncomingViewingKey, std::vector<libzcash::SaplingPaymentAddress>> ivkMap;
    for (const auto & addr : addresses) {
        auto saplingAddr = boost::get<libzcash::SaplingPaymentAddress>(&addr);
        if (saplingAddr != nullptr) {
            libzcash::SaplingIncomingViewingKey ivk;
            this->GetSaplingIncomingViewingKey(*saplingAddr, ivk);
            ivkMap[ivk].push_back(*saplingAddr);
        }
    }
    for (const auto & txPair : mapWallet) {
        // Sapling
        for (const auto & noteDataPair : txPair.second.mapSaplingNoteData) {
            auto & noteData = noteDataPair.second;
            auto & nullifier = noteData.nullifier;
            auto & ivk = noteData.ivk;
            if (nullifier && ivkMap.count(ivk)) {
                for (const auto & addr : ivkMap[ivk]) {
                    nullifierSet.insert(std::make_pair(addr, nullifier.get()));
                }
            }
        }
    }
    return nullifierSet;
}


bool CWallet::IsNoteSaplingChange(const std::set<std::pair<libzcash::PaymentAddress, uint256>> & nullifierSet,
        const libzcash::PaymentAddress & address,
        const SaplingOutPoint & op)
{
    // A Note is marked as "change" if the address that received it
    // also spent Notes in the same transaction. This will catch,
    // for instance:
    // - Change created by spending fractions of Notes (because
    //   z_sendmany sends change to the originating z-address).
    // - Notes created by consolidation transactions (e.g. using
    //   z_mergetoaddress).
    // - Notes sent from one address to itself.
    for (const SpendDescription &spend : mapWallet[op.hash].vShieldedSpend) {
        if (nullifierSet.count(std::make_pair(address, spend.nullifier))) {
            return true;
        }
    }
    return false;
}

bool CWallet::SetMinVersion(enum WalletFeature nVersion, CWalletDB* pwalletdbIn, bool fExplicit)
{
    LOCK(cs_wallet); // nWalletVersion
    if (nWalletVersion >= nVersion)
        return true;

    // when doing an explicit upgrade, if we pass the max version permitted, upgrade all the way
    if (fExplicit && nVersion > nWalletMaxVersion)
            nVersion = FEATURE_LATEST;

    nWalletVersion = nVersion;

    if (nVersion > nWalletMaxVersion)
        nWalletMaxVersion = nVersion;

    if (fFileBacked)
    {
        CWalletDB* pwalletdb = pwalletdbIn ? pwalletdbIn : new CWalletDB(strWalletFile);
        if (nWalletVersion > 40000)
            pwalletdb->WriteMinVersion(nWalletVersion);
        if (!pwalletdbIn)
            delete pwalletdb;
    }

    return true;
}

bool CWallet::SetMaxVersion(int nVersion)
{
    LOCK(cs_wallet); // nWalletVersion, nWalletMaxVersion
    // cannot downgrade below current version
    if (nWalletVersion > nVersion)
        return false;

    nWalletMaxVersion = nVersion;

    return true;
}

set<uint256> CWallet::GetConflicts(const uint256& txid) const
{
    set<uint256> result;
    AssertLockHeld(cs_wallet);

    std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(txid);
    if (it == mapWallet.end())
        return result;
    const CWalletTx& wtx = it->second;

    std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range;

    BOOST_FOREACH(const CTxIn& txin, wtx.vin)
    {
        if (mapTxSpends.count(txin.prevout) <= 1)
            continue;  // No conflict if zero or one spends
        range = mapTxSpends.equal_range(txin.prevout);
        for (TxSpends::const_iterator it = range.first; it != range.second; ++it)
            result.insert(it->second);
    }

    std::pair<TxNullifiers::const_iterator, TxNullifiers::const_iterator> range_o;

    for (const SpendDescription &spend : wtx.vShieldedSpend) {
        uint256 nullifier = spend.nullifier;
        if (mapTxSaplingNullifiers.count(nullifier) <= 1) {
            continue;  // No conflict if zero or one spends
        }
        range_o = mapTxSaplingNullifiers.equal_range(nullifier);
        for (TxNullifiers::const_iterator it = range_o.first; it != range_o.second; ++it) {
            result.insert(it->second);
        }
    }
    return result;
}

void CWallet::Flush(bool shutdown)
{
    bitdb.Flush(shutdown);
}

bool CWallet::Verify(const string& walletFile, string& warningString, string& errorString)
{
    LogPrintf("Using wallet %s\n", walletFile);
    uiInterface.InitMessage(_("Verifying wallet..."));

    if (walletFile != boost::filesystem::basename(walletFile) + boost::filesystem::extension(walletFile)) {
        boost::filesystem::path path(walletFile);
        if (path.is_absolute()) {
            if (!boost::filesystem::exists(path.parent_path())) {
                LogPrintf("Absolute path %s does not exist!", walletFile);
                return false;
            }
        } else {
            boost::filesystem::path full_path = GetDataDir() / path;
            if (!boost::filesystem::exists(full_path.parent_path())) {
                LogPrintf("Relative path %s does not exist!", walletFile);
                return false;
            }
        }
    }

    if (!bitdb.Open(GetDataDir()))
    {
        // try moving the database env out of the way
        boost::filesystem::path pathDatabase = GetDataDir() / "database";
        boost::filesystem::path pathDatabaseBak = GetDataDir() / strprintf("database.%d.bak", GetTime());
        try {
            boost::filesystem::rename(pathDatabase, pathDatabaseBak);
            LogPrintf("Moved old %s to %s. Retrying.\n", pathDatabase.string(), pathDatabaseBak.string());
        } catch (const boost::filesystem::filesystem_error&) {
            // failure is ok (well, not really, but it's not worse than what we started with)
        }

        // try again
        if (!bitdb.Open(GetDataDir())) {
            // if it still fails, it probably means we can't even create the database env
            string msg = strprintf(_("Error initializing wallet database environment %s!"), GetDataDir());
            errorString += msg;
            return true;
        }
    }

    if (GetBoolArg("-salvagewallet", false))
    {
        // Recover readable keypairs:
        if (!CWalletDB::Recover(bitdb, walletFile, true))
            return false;
    }

    if (boost::filesystem::exists(GetDataDir() / walletFile))
    {
        CDBEnv::VerifyResult r = bitdb.Verify(walletFile, CWalletDB::Recover);
        if (r == CDBEnv::RECOVER_OK)
        {
            warningString += strprintf(_("Warning: wallet.dat corrupt, data salvaged!"
                                     " Original wallet.dat saved as wallet.{timestamp}.bak in %s; if"
                                     " your balance or transactions are incorrect you should"
                                     " restore from a backup."), GetDataDir());
        }
        if (r == CDBEnv::RECOVER_FAIL)
            errorString += _("wallet.dat corrupt, salvage failed");
    }

    return true;
}

template <class T>
void CWallet::SyncMetaData(pair<typename TxSpendMap<T>::iterator, typename TxSpendMap<T>::iterator> range)
{
    // We want all the wallet transactions in range to have the same metadata as
    // the oldest (smallest nOrderPos).
    // So: find smallest nOrderPos:

    int nMinOrderPos = std::numeric_limits<int>::max();
    const CWalletTx* copyFrom = NULL;
    for (typename TxSpendMap<T>::iterator it = range.first; it != range.second; ++it)
    {
        const uint256& hash = it->second;
        int n = mapWallet[hash].nOrderPos;
        if (n < nMinOrderPos)
        {
            nMinOrderPos = n;
            copyFrom = &mapWallet[hash];
        }
    }
    // Now copy data from copyFrom to rest:
    for (typename TxSpendMap<T>::iterator it = range.first; it != range.second; ++it)
    {
        const uint256& hash = it->second;
        CWalletTx* copyTo = &mapWallet[hash];
        if (copyFrom == copyTo) continue;
        copyTo->mapValue = copyFrom->mapValue;
        // mapSproutNoteData and mapSaplingNoteData not copied on purpose
        // (it is always set correctly for each CWalletTx)
        copyTo->vOrderForm = copyFrom->vOrderForm;
        // fTimeReceivedIsTxTime not copied on purpose
        // nTimeReceived not copied on purpose
        copyTo->nTimeSmart = copyFrom->nTimeSmart;
        copyTo->fFromMe = copyFrom->fFromMe;
        copyTo->strFromAccount = copyFrom->strFromAccount;
        // nOrderPos not copied on purpose
        // cached members not copied on purpose
    }
}

/**
 * Outpoint is spent if any non-conflicted transaction
 * spends it:
 */
bool CWallet::IsSpent(const uint256& hash, unsigned int n) const
{
    const COutPoint outpoint(hash, n);
    pair<TxSpends::const_iterator, TxSpends::const_iterator> range;
    range = mapTxSpends.equal_range(outpoint);

    for (TxSpends::const_iterator it = range.first; it != range.second; ++it)
    {
        const uint256& wtxid = it->second;
        std::map<uint256, CWalletTx>::const_iterator mit = mapWallet.find(wtxid);
        if (mit != mapWallet.end() && mit->second.GetDepthInMainChain() >= 0)
            return true; // Spent
    }
    return false;
}

unsigned int CWallet::GetSpendDepth(const uint256& hash, unsigned int n) const
{
    const COutPoint outpoint(hash, n);
    pair<TxSpends::const_iterator, TxSpends::const_iterator> range;
    range = mapTxSpends.equal_range(outpoint);

    for (TxSpends::const_iterator it = range.first; it != range.second; ++it)
    {
        const uint256& wtxid = it->second;
        std::map<uint256, CWalletTx>::const_iterator mit = mapWallet.find(wtxid);
        if (mit != mapWallet.end() && mit->second.GetDepthInMainChain() >= 0)
            return mit->second.GetDepthInMainChain(); // Spent
    }
    return 0;
}

bool CWallet::IsSaplingSpent(const uint256& nullifier) const {
    pair<TxNullifiers::const_iterator, TxNullifiers::const_iterator> range;
    range = mapTxSaplingNullifiers.equal_range(nullifier);

    for (TxNullifiers::const_iterator it = range.first; it != range.second; ++it) {
        const uint256& wtxid = it->second;
        std::map<uint256, CWalletTx>::const_iterator mit = mapWallet.find(wtxid);
        if (mit != mapWallet.end() && mit->second.GetDepthInMainChain() >= 0) {
            return true; // Spent
        }
    }
    return false;
}

unsigned int CWallet::GetSaplingSpendDepth(const uint256& nullifier) const {
    pair<TxNullifiers::const_iterator, TxNullifiers::const_iterator> range;
    range = mapTxSaplingNullifiers.equal_range(nullifier);

    for (TxNullifiers::const_iterator it = range.first; it != range.second; ++it) {
        const uint256& wtxid = it->second;
        std::map<uint256, CWalletTx>::const_iterator mit = mapWallet.find(wtxid);
        if (mit != mapWallet.end() && mit->second.GetDepthInMainChain() >= 0) {
            return mit->second.GetDepthInMainChain(); // Spent
        }
    }
    return 0;
}

void CWallet::AddToTransparentSpends(const COutPoint& outpoint, const uint256& wtxid)
{
    mapTxSpends.insert(make_pair(outpoint, wtxid));

    pair<TxSpends::iterator, TxSpends::iterator> range;
    range = mapTxSpends.equal_range(outpoint);
    SyncMetaData<COutPoint>(range);
}

void CWallet::AddToSaplingSpends(const uint256& nullifier, const uint256& wtxid)
{
    mapTxSaplingNullifiers.insert(make_pair(nullifier, wtxid));

    pair<TxNullifiers::iterator, TxNullifiers::iterator> range;
    range = mapTxSaplingNullifiers.equal_range(nullifier);
    SyncMetaData<uint256>(range);
}

void CWallet::AddToSpends(const uint256& wtxid)
{
    assert(mapWallet.count(wtxid));
    CWalletTx& thisTx = mapWallet[wtxid];
    if (thisTx.IsCoinBase()) // Coinbases don't spend anything!
        return;

    for (const CTxIn& txin : thisTx.vin) {
        AddToTransparentSpends(txin.prevout, wtxid);
    }
    for (const SpendDescription &spend : thisTx.vShieldedSpend) {
        AddToSaplingSpends(spend.nullifier, wtxid);
    }
}

std::set<uint256> CWallet::GetNullifiers()
{
    std::set<uint256> nullifierSet;
    for (const auto & txPair : mapWallet) {
        // Sapling
        for (const auto & noteDataPair : txPair.second.mapSaplingNoteData) {
            auto & noteData = noteDataPair.second;
            auto & nullifier = noteData.nullifier;
            if (nullifier) {
                nullifierSet.insert(nullifier.get());
            }
        }
    }
    return nullifierSet;
}

int64_t CWallet::NullifierCount()
{
    LOCK(cs_wallet);
    if(fZdebug) {
        //fprintf(stderr,"%s:mapTxSaplingNullifers.size=%d\n",__FUNCTION__,(int)mapTxSaplingNullifiers.size() );
        //fprintf(stderr,"%s:mempool.getNullifiers.size=%d\n",__FUNCTION__,(int)mempool.getNullifiers().size() );
        //fprintf(stderr,"%s:cacheSaplingNullifiers.size=%d\n",__FUNCTION__,(int)pcoinsTip->getNullifiers().size() );
    }
    return pcoinsTip->getNullifiers().size();
}


void CWallet::ClearNoteWitnessCache()
{
    LOCK(cs_wallet);
    int notes = 0;
    for (std::pair<const uint256, CWalletTx>& wtxItem : mapWallet) {
        for (mapSaplingNoteData_t::value_type& item : wtxItem.second.mapSaplingNoteData) {
            item.second.witnesses.clear();
            item.second.witnessHeight = -1;
            notes++;
        }
    }
    LogPrintf("%s: Cleared witness data from %d wallet items and %d SaplingNotes\n", __func__, mapWallet.size(), notes);
}

void CWallet::DecrementNoteWitnesses(const CBlockIndex* pindex)
{
    LOCK(cs_wallet);

    extern int32_t HUSH_REWIND;

    for (std::pair<const uint256, CWalletTx>& wtxItem : mapWallet) {
        //Sapling
        for (auto& item : wtxItem.second.mapSaplingNoteData) {
            auto* nd = &(item.second);
            if (nd->nullifier && pwalletMain->GetSaplingSpendDepth(*item.second.nullifier) <= WITNESS_CACHE_SIZE) {
                // Only decrement witnesses that are not above the current height
                if (nd->witnessHeight <= pindex->GetHeight()) {
                    if (nd->witnesses.size() > 1) {
                        // indexHeight is the height of the block being removed, so
                        // the new witness cache height is one below it.
                        nd->witnesses.pop_front();
                        nd->witnessHeight = pindex->GetHeight() - 1;
                    }
                }
            }
        }
    }
    assert(HUSH_REWIND != 0 || WITNESS_CACHE_SIZE != _COINBASE_MATURITY+10);
}

template<typename NoteData>
void ClearSingleNoteWitnessCache(NoteData* nd)
{
    nd->witnesses.clear();
    nd->witnessHeight = -1;
    nd->witnessRootValidated = false;
}

int CWallet::SaplingWitnessMinimumHeight(const uint256& nullifier, int nWitnessHeight, int nMinimumHeight)
{
    if (GetSaplingSpendDepth(nullifier) <= WITNESS_CACHE_SIZE) {
        nMinimumHeight = min(nWitnessHeight, nMinimumHeight);
    }
    return nMinimumHeight;
}

int CWallet::VerifyAndSetInitialWitness(const CBlockIndex* pindex, bool witnessOnly)
{
  LOCK2(cs_main, cs_wallet);

  int nWitnessTxIncrement = 0;
  int nWitnessTotalTxCount = mapWallet.size();
  int nMinimumHeight = pindex->GetHeight();

  for (std::pair<const uint256, CWalletTx>& wtxItem : mapWallet) {
    nWitnessTxIncrement += 1;

    if (wtxItem.second.mapSaplingNoteData.empty())
      continue;

    if (wtxItem.second.GetDepthInMainChain() > 0) {
      auto wtxHash = wtxItem.second.GetHash();
      int wtxHeight = mapBlockIndex[wtxItem.second.hashBlock]->GetHeight();

      for (mapSaplingNoteData_t::value_type& item : wtxItem.second.mapSaplingNoteData) {

        auto op = item.first;
        auto* nd = &(item.second);
        CBlockIndex* pblockindex;
        uint256 blockRoot;
        uint256 witnessRoot;

        if (!nd->nullifier)
          ::ClearSingleNoteWitnessCache(nd);

        if (!nd->witnesses.empty() && nd->witnessHeight > 0) {

          //Skip all functions for validated witness while witness only = true
          if (nd->witnessRootValidated && witnessOnly)
            continue;

          //Skip Validation when witness root has been validated
          if (nd->witnessRootValidated) {
            nMinimumHeight = SaplingWitnessMinimumHeight(*item.second.nullifier, nd->witnessHeight, nMinimumHeight);
            continue;
          }

          //Skip Validation when witness height is greater that block height
          if (nd->witnessHeight > pindex->GetHeight() - 1) {
            nMinimumHeight = SaplingWitnessMinimumHeight(*item.second.nullifier, nd->witnessHeight, nMinimumHeight);
            continue;
          }

          //Validate the witness at the witness height
          witnessRoot = nd->witnesses.front().root();
          pblockindex = chainActive[nd->witnessHeight];
          blockRoot = pblockindex->hashFinalSaplingRoot;
          if (witnessRoot == blockRoot) {
            nd->witnessRootValidated = true;
            nMinimumHeight = SaplingWitnessMinimumHeight(*item.second.nullifier, nd->witnessHeight, nMinimumHeight);
            continue;
          }
        }

        //Clear witness Cache for all other scenarios
        pblockindex = chainActive[wtxHeight];
        ::ClearSingleNoteWitnessCache(nd);

        LogPrintf("%s: Setting Initial Sapling Witness for tx %s, %i of %i\n", __func__, wtxHash.ToString(), nWitnessTxIncrement, nWitnessTotalTxCount);

        SaplingMerkleTree saplingTree;
        blockRoot = pblockindex->pprev->hashFinalSaplingRoot;
        pcoinsTip->GetSaplingAnchorAt(blockRoot, saplingTree);

        //Cycle through blocks and transactions building sapling tree until the commitment needed is reached
        const CBlock* pblock;
        CBlock block;
        if (!ReadBlockFromDisk(block, pblockindex, 1)) {
            throw std::runtime_error(
                strprintf("Cannot read block height %d (%s) from disk", pindex->GetHeight(), pindex->GetBlockHash().GetHex()));
        }
        pblock = &block;

        for (const CTransaction& tx : block.vtx) {
          auto hash = tx.GetHash();

          // Sapling
          for (uint32_t i = 0; i < tx.vShieldedOutput.size(); i++) {
            const uint256& note_commitment = tx.vShieldedOutput[i].cm;

            // Increment existing witness until the end of the block
            if (!nd->witnesses.empty()) {
              nd->witnesses.front().append(note_commitment);
            }

            //Only needed for intial witness
            if (nd->witnesses.empty()) {
              saplingTree.append(note_commitment);

              // If this is our note, witness it
              if (hash == wtxHash) {
                SaplingOutPoint outPoint {hash, i};
                if (op == outPoint) {
                  nd->witnesses.push_front(saplingTree.witness());
                }
              }
            }
          }
        }
        nd->witnessHeight = pblockindex->GetHeight();
        UpdateSaplingNullifierNoteMapWithTx(wtxItem.second);
        nMinimumHeight = SaplingWitnessMinimumHeight(*item.second.nullifier, nd->witnessHeight, nMinimumHeight);
      }
    }
  }

  if(fZdebug)
    LogPrintf("%s: nMinimumHeight=%d\n",__func__, nMinimumHeight);
  return nMinimumHeight;
}

void CWallet::BuildWitnessCache(const CBlockIndex* pindex, bool witnessOnly)
{

  LOCK2(cs_main, cs_wallet);

  int startHeight = VerifyAndSetInitialWitness(pindex, witnessOnly) + 1;

  if (startHeight > pindex->GetHeight() || witnessOnly) {
    return;
  }

  uint256 saplingRoot;
  CBlockIndex* pblockindex = chainActive[startHeight];
  int height = chainActive.Height();
  if(fZdebug)
    LogPrintf("%s: height=%d, startHeight=%d\n", __func__, height, startHeight);

  while (pblockindex) {
    if (ShutdownRequested()) {
        LogPrintf("%s: shutdown requested, aborting building witnesses\n", __func__);
        break;
    }
    if(pwalletMain->fAbortRescan) {
        LogPrintf("%s: rescan aborted at block %d, stopping witness building\n", pwalletMain->rescanHeight);
        pwalletMain->fRescanning = false;
        return;
    }

    if (pblockindex->GetHeight() % 100 == 0 && pblockindex->GetHeight() < height - 5) {
      LogPrintf("Building Witnesses for block %i %.4f complete, %d remaining\n", pblockindex->GetHeight(), pblockindex->GetHeight() / double(height), height - pblockindex->GetHeight() );
    }

    SaplingMerkleTree saplingTree;
    saplingRoot = pblockindex->pprev->hashFinalSaplingRoot;
    pcoinsTip->GetSaplingAnchorAt(saplingRoot, saplingTree);

    //Cycle through blocks and transactions building sapling tree until the commitment needed is reached
    CBlock block;
    if (!ReadBlockFromDisk(block, pblockindex, 1)) {
        throw std::runtime_error(
           strprintf("Cannot read block height %d (%s) from disk", pindex->GetHeight(), pindex->GetBlockHash().GetHex()));
    }

    for (std::pair<const uint256, CWalletTx>& wtxItem : mapWallet) {

      if (wtxItem.second.mapSaplingNoteData.empty())
        continue;

      if (wtxItem.second.GetDepthInMainChain() > 0) {

        //Sapling
        for (mapSaplingNoteData_t::value_type& item : wtxItem.second.mapSaplingNoteData) {
          auto* nd = &(item.second);
          if (nd->nullifier && nd->witnessHeight == pblockindex->GetHeight() - 1
              && GetSaplingSpendDepth(*item.second.nullifier) <= WITNESS_CACHE_SIZE) {

            nd->witnesses.push_front(nd->witnesses.front());
            while (nd->witnesses.size() > WITNESS_CACHE_SIZE) {
                nd->witnesses.pop_back();
            }

            for (const CTransaction& tx : block.vtx) {
              for (uint32_t i = 0; i < tx.vShieldedOutput.size(); i++) {
                const uint256& note_commitment = tx.vShieldedOutput[i].cm;
                nd->witnesses.front().append(note_commitment);
              }
            }
            nd->witnessHeight = pblockindex->GetHeight();
          }
        }

      }
    }

    if (pblockindex == pindex)
      break;

    pblockindex = chainActive.Next(pblockindex);

  }

}

bool CWallet::EncryptWallet(const SecureString& strWalletPassphrase)
{
    if (IsCrypted())
        return false;

    CKeyingMaterial vMasterKey;

    vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    GetRandBytes(&vMasterKey[0], WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey;

    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    GetRandBytes(&kMasterKey.vchSalt[0], WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64_t nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = 2500000 / ((double)(GetTimeMillis() - nStartTime));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + kMasterKey.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    LogPrintf("Encrypting Wallet with an nDeriveIterations of %i\n", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod))
        return false;
    if (!crypter.Encrypt(vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    {
        LOCK(cs_wallet);
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        if (fFileBacked)
        {
            assert(!pwalletdbEncryption);
            pwalletdbEncryption = new CWalletDB(strWalletFile);
            if (!pwalletdbEncryption->TxnBegin()) {
                delete pwalletdbEncryption;
                pwalletdbEncryption = NULL;
                return false;
            }
            pwalletdbEncryption->WriteMasterKey(nMasterKeyMaxID, kMasterKey);
        }

        if (!EncryptKeys(vMasterKey))
        {
            if (fFileBacked) {
                pwalletdbEncryption->TxnAbort();
                delete pwalletdbEncryption;
            }
            // We now probably have half of our keys encrypted in memory, and half not...
            // die and let the user reload the unencrypted wallet.
            assert(false);
        }

        // Encryption was introduced in version 0.4.0
        SetMinVersion(FEATURE_WALLETCRYPT, pwalletdbEncryption, true);

        if (fFileBacked)
        {
            if (!pwalletdbEncryption->TxnCommit()) {
                delete pwalletdbEncryption;
                // We now have keys encrypted in memory, but not on disk...
                // die to avoid confusion and let the user reload the unencrypted wallet.
                assert(false);
            }

            delete pwalletdbEncryption;
            pwalletdbEncryption = NULL;
        }

        Lock();
        Unlock(strWalletPassphrase);
        NewKeyPool();
        Lock();

        // Need to completely rewrite the wallet file; if we don't, bdb might keep
        // bits of the unencrypted private key in slack space in the database file.
        CDB::Rewrite(strWalletFile);

    }
    NotifyStatusChanged(this);

    return true;
}

int64_t CWallet::IncOrderPosNext(CWalletDB *pwalletdb)
{
    AssertLockHeld(cs_wallet); // nOrderPosNext
    int64_t nRet = nOrderPosNext++;
    if (pwalletdb) {
        pwalletdb->WriteOrderPosNext(nOrderPosNext);
    } else {
        CWalletDB(strWalletFile).WriteOrderPosNext(nOrderPosNext);
    }
    return nRet;
}

CWallet::TxItems CWallet::OrderedTxItems(std::list<CAccountingEntry>& acentries, std::string strAccount)
{
    AssertLockHeld(cs_wallet); // mapWallet
    CWalletDB walletdb(strWalletFile);

    // First: get all CWalletTx and CAccountingEntry into a sorted-by-order multimap.
    TxItems txOrdered;

    // Note: maintaining indices in the database of (account,time) --> txid and (account, time) --> acentry
    // would make this much faster for applications that do this a lot.
    for (map<uint256, CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        CWalletTx* wtx = &((*it).second);
        txOrdered.insert(make_pair(wtx->nOrderPos, TxPair(wtx, (CAccountingEntry*)0)));
        //fprintf(stderr,"ordered iter.%d %s\n",(int32_t)wtx->nOrderPos,wtx->GetHash().GetHex().c_str());
    }
    acentries.clear();
    walletdb.ListAccountCreditDebit(strAccount, acentries);
    BOOST_FOREACH(CAccountingEntry& entry, acentries)
    {
        txOrdered.insert(make_pair(entry.nOrderPos, TxPair((CWalletTx*)0, &entry)));
    }

    return txOrdered;
}


void CWallet::MarkDirty()
{
    {
        LOCK(cs_wallet);
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet)
            item.second.MarkDirty();
    }
}

/**
 * Ensure that every note in the wallet (for which we possess a spending key)
 * has a cached nullifier.
 */
bool CWallet::UpdateNullifierNoteMap()
{
    {
        LOCK(cs_wallet);

        if (IsLocked())
            return false;

        ZCNoteDecryption dec;
        for (std::pair<const uint256, CWalletTx>& wtxItem : mapWallet) {
            // TODO: Sapling.  This method is only called from RPC walletpassphrase, which is currently unsupported
            // as RPC encryptwallet is hidden behind two flags: -developerencryptwallet -experimentalfeatures

            UpdateNullifierNoteMapWithTx(wtxItem.second);
        }
    }
    return true;
}

/**
 * Update mapSaplingNullifiersToNotes
 * with the cached nullifiers in this tx.
 */
void CWallet::UpdateNullifierNoteMapWithTx(const CWalletTx& wtx)
{
    {
        LOCK(cs_wallet);

        for (const mapSaplingNoteData_t::value_type& item : wtx.mapSaplingNoteData) {
            if (item.second.nullifier) {
                mapSaplingNullifiersToNotes[*item.second.nullifier] = item.first;
            }
        }
    }
}

/**
 * Update mapSaplingNullifiersToNotes, computing the nullifier from a cached witness if necessary.
 */
void CWallet::UpdateSaplingNullifierNoteMapWithTx(CWalletTx& wtx) {
    LOCK(cs_wallet);

    for (mapSaplingNoteData_t::value_type &item : wtx.mapSaplingNoteData) {
        SaplingOutPoint op = item.first;
        SaplingNoteData nd = item.second;

        if (nd.witnesses.empty()) {
            // If there are no witnesses, erase the nullifier and associated mapping.
            if (item.second.nullifier) {
                mapSaplingNullifiersToNotes.erase(item.second.nullifier.get());
            }
            item.second.nullifier = boost::none;
        }
        else {
            uint64_t position = nd.witnesses.front().position();
            // Skip if we only have incoming viewing key
            if (mapSaplingFullViewingKeys.count(nd.ivk) != 0) {
                SaplingFullViewingKey fvk = mapSaplingFullViewingKeys.at(nd.ivk);
                OutputDescription output = wtx.vShieldedOutput[op.n];
                auto optPlaintext = SaplingNotePlaintext::decrypt(output.encCiphertext, nd.ivk, output.ephemeralKey, output.cm);
                if (!optPlaintext) {
                    // An item in mapSaplingNoteData must have already been successfully decrypted,
                    // otherwise the item would not exist in the first place.
                    assert(false);
                }
                auto optNote = optPlaintext.get().note(nd.ivk);
                if (!optNote) {
                    assert(false);
                }
                auto optNullifier = optNote.get().nullifier(fvk, position);
                if (!optNullifier) {
                    // This should not happen.  If it does, maybe the position has been corrupted or miscalculated?
                    assert(false);
                }
                uint256 nullifier = optNullifier.get();
                mapSaplingNullifiersToNotes[nullifier] = op;
                item.second.nullifier = nullifier;
            }
        }
    }
}

/**
 * Iterate over transactions in a block and update the cached Sapling nullifiers
 * for transactions which belong to the wallet.
 */
void CWallet::UpdateNullifierNoteMapForBlock(const CBlock *pblock) {
    LOCK(cs_wallet);

    for (const CTransaction& tx : pblock->vtx) {
        auto hash = tx.GetHash();
        bool txIsOurs = mapWallet.count(hash);
        if (txIsOurs) {
            UpdateSaplingNullifierNoteMapWithTx(mapWallet[hash]);
        }
    }
}

bool CWallet::AddToWallet(const CWalletTx& wtxIn, bool fFromLoadWallet, CWalletDB* pwalletdb)
{
    uint256 hash = wtxIn.GetHash();

    if (fFromLoadWallet)
    {
        mapWallet[hash] = wtxIn;
        mapWallet[hash].BindWallet(this);
        UpdateNullifierNoteMapWithTx(mapWallet[hash]);
        AddToSpends(hash);
    }
    else
    {
        LOCK(cs_wallet);
        // Inserts only if not already there, returns tx inserted or tx found
        pair<map<uint256, CWalletTx>::iterator, bool> ret = mapWallet.insert(make_pair(hash, wtxIn));
        CWalletTx& wtx = (*ret.first).second;
        wtx.BindWallet(this);
        UpdateNullifierNoteMapWithTx(wtx);
        bool fInsertedNew = ret.second;
        if (fInsertedNew)
        {
            wtx.nTimeReceived = GetTime();
            wtx.nOrderPos     = IncOrderPosNext(pwalletdb);
            wtx.nTimeSmart    = wtx.nTimeReceived;
            if (!wtxIn.hashBlock.IsNull())
            {
                if (mapBlockIndex.count(wtxIn.hashBlock))
                {
                    int64_t latestNow = wtx.nTimeReceived;
                    int64_t latestEntry = 0;
                    {
                        // Tolerate times up to the last timestamp in the wallet not more than 5 minutes into the future
                        int64_t latestTolerated = latestNow + 300;
                        std::list<CAccountingEntry> acentries;
                        TxItems txOrdered = OrderedTxItems(acentries);
                        for (TxItems::reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it)
                        {
                            CWalletTx *const pwtx = (*it).second.first;
                            if (pwtx == &wtx)
                                continue;
                            CAccountingEntry *const pacentry = (*it).second.second;
                            int64_t nSmartTime;
                            if (pwtx)
                            {
                                nSmartTime = pwtx->nTimeSmart;
                                if (!nSmartTime)
                                    nSmartTime = pwtx->nTimeReceived;
                            }
                            else
                                nSmartTime = pacentry->nTime;
                            if (nSmartTime <= latestTolerated)
                            {
                                latestEntry = nSmartTime;
                                if (nSmartTime > latestNow)
                                    latestNow = nSmartTime;
                                break;
                            }
                        }
                    }

                    int64_t blocktime = mapBlockIndex[wtxIn.hashBlock]->GetBlockTime();
                    wtx.nTimeSmart = std::max(latestEntry, std::min(blocktime, latestNow));
                }
                else
                    LogPrintf("AddToWallet(): found %s in block %s not in index\n",
                             wtxIn.GetHash().ToString(),
                             wtxIn.hashBlock.ToString());
            }
            AddToSpends(hash);
        }

        bool fUpdated = false;
        if (!fInsertedNew)
        {
            // Merge
            if (!wtxIn.hashBlock.IsNull() && wtxIn.hashBlock != wtx.hashBlock)
            {
                wtx.hashBlock = wtxIn.hashBlock;
                fUpdated = true;
            }
            if (wtxIn.nIndex != -1 && (wtxIn.vMerkleBranch != wtx.vMerkleBranch || wtxIn.nIndex != wtx.nIndex))
            {
                wtx.vMerkleBranch = wtxIn.vMerkleBranch;
                wtx.nIndex = wtxIn.nIndex;
                fUpdated = true;
            }
            if (UpdatedNoteData(wtxIn, wtx)) {
                fUpdated = true;
            }
            if (wtxIn.fFromMe && wtxIn.fFromMe != wtx.fFromMe)
            {
                wtx.fFromMe = wtxIn.fFromMe;
                fUpdated = true;
            }
        }

        //// debug print
        LogPrintf("AddToWallet %s at height %d %s%s\n", wtxIn.GetHash().ToString(), hush_blockheight(wtxIn.hashBlock), (fInsertedNew ? "new" : ""), (fUpdated ? "update" : ""));

        // Write to disk
        if (fInsertedNew || fUpdated)
            if (!wtx.WriteToDisk(pwalletdb))
                return false;

        // Break debit/credit balance caches:
        wtx.MarkDirty();

        // Notify UI of new or updated transaction
        NotifyTransactionChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

        // notify an external script when a wallet transaction comes in or is updated
        std::string strCmd = GetArg("-walletnotify", "");

        if ( !strCmd.empty())
        {
            boost::replace_all(strCmd, "%s", wtxIn.GetHash().GetHex());
            boost::thread t(runCommand, strCmd); // thread runs free
        }

    }
    return true;
}

bool CWallet::UpdatedNoteData(const CWalletTx& wtxIn, CWalletTx& wtx)
{
    bool unchangedSaplingFlag = (wtxIn.mapSaplingNoteData.empty() || wtxIn.mapSaplingNoteData == wtx.mapSaplingNoteData);
    if (!unchangedSaplingFlag) {
        auto tmp = wtxIn.mapSaplingNoteData;
        // Ensure we keep any cached witnesses we may already have

        for (const std::pair <SaplingOutPoint, SaplingNoteData> nd : wtx.mapSaplingNoteData) {
            if (tmp.count(nd.first) && nd.second.witnesses.size() > 0) {
                tmp.at(nd.first).witnesses.assign(
                        nd.second.witnesses.cbegin(), nd.second.witnesses.cend());
            }
            tmp.at(nd.first).witnessHeight = nd.second.witnessHeight;
        }

        // Now copy over the updated note data
        wtx.mapSaplingNoteData = tmp;
    }

    return !unchangedSaplingFlag;
}

/**
 * Add a transaction to the wallet, or update it.
 * pblock is optional, but should be provided if the transaction is known to be in a block.
 * If fUpdate is true, existing transactions will be updated.
 */

bool CWallet::AddToWalletIfInvolvingMe(const CTransaction& tx, const CBlock* pblock, bool fUpdate)
{
    if(fDebug)
        fprintf(stderr,"%s: tx=%s\n", __func__, tx.GetHash().ToString().c_str() );

    {
        AssertLockHeld(cs_wallet);
        if ( tx.IsCoinBase() && tx.vout[0].nValue == 0 )
            return false;
        bool fExisted = mapWallet.count(tx.GetHash()) != 0;
        if (fExisted && !fUpdate) return false;
        auto saplingNoteDataAndAddressesToAdd = FindMySaplingNotes(tx);
        auto saplingNoteData                  = saplingNoteDataAndAddressesToAdd.first;
        auto addressesToAdd                   = saplingNoteDataAndAddressesToAdd.second;
        for (const auto &addressToAdd : addressesToAdd) {
            if (!AddSaplingIncomingViewingKey(addressToAdd.second, addressToAdd.first)) {
                return false;
            }
        }
        static std::string NotaryAddress; static bool didinit;
        if ( !didinit && NotaryAddress.empty() && NOTARY_PUBKEY33[0] != 0 ) {
            didinit = true;
            char Raddress[64]; 
            pubkey2addr((char *)Raddress,(uint8_t *)NOTARY_PUBKEY33);
            NotaryAddress.assign(Raddress);
            vAllowListAddress = mapMultiArgs["-allowlistaddress"];
            if ( !vAllowListAddress.empty() )
            {
                fprintf(stderr, "Activated Wallet Filter \n  Notary Address: %s \n  Adding allowlist address's:\n", NotaryAddress.c_str());
                for ( auto wladdr : vAllowListAddress )
                    fprintf(stderr, "    %s\n", wladdr.c_str());
            }
        }
        if (fExisted || IsMine(tx) || IsFromMe(tx) || saplingNoteData.size() > 0) {
            // wallet filter for notary nodes. Enables by setting -allowlistaddress= as startup param or in conf file (works same as -addnode but with taddr)
            if ( !tx.IsCoinBase() && !vAllowListAddress.empty() && !NotaryAddress.empty() ) {
                int numvinIsOurs = 0, numvinIsAllowList = 0;
                for (size_t i = 0; i < tx.vin.size(); i++)
                {
                    uint256 hash; CTransaction txin; CTxDestination address;
                    if ( myGetTransaction(tx.vin[i].prevout.hash,txin,hash) && ExtractDestination(txin.vout[tx.vin[i].prevout.n].scriptPubKey, address) )
                    {
                        if ( CBitcoinAddress(address).ToString() == NotaryAddress )
                            numvinIsOurs++;
                        for ( auto wladdr : vAllowListAddress )
                        {
                            if ( CBitcoinAddress(address).ToString() == wladdr )
                            {
                                //fprintf(stderr, "We received from allowlisted address.%s\n", wladdr.c_str());
                                numvinIsAllowList++;
                            }
                        }
                    }
                }
                // Now we know if it was a tx sent to us, by either a allowlisted address, or ourself.
                if ( numvinIsOurs != 0 )
                    fprintf(stderr, "We sent from address: %s vins: %d\n",NotaryAddress.c_str(),numvinIsOurs);
                if ( numvinIsOurs == 0 && numvinIsAllowList == 0 )
                    return false;
            }

            CWalletTx wtx(this,tx);

            if (saplingNoteData.size() > 0) {
                wtx.SetSaplingNoteData(saplingNoteData);
            }

            // Get merkle branch if transaction was found in a block
            if (pblock)
                wtx.SetMerkleBranch(*pblock);

            // Do not flush the wallet here for performance reasons
            // this is safe, as in case of a crash, we rescan the necessary blocks on startup through our SetBestChain-mechanism
            CWalletDB walletdb(strWalletFile, "r+", false);

            return AddToWallet(wtx, false, &walletdb);
        }
    }
    return false;
}

void CWallet::SyncTransaction(const CTransaction& tx, const CBlock* pblock)
{
    LOCK(cs_wallet);
    if(fDebug)
        fprintf(stderr,"%s: tx=%s\n", __func__, tx.GetHash().ToString().c_str() );
    if (!AddToWalletIfInvolvingMe(tx, pblock, true))
        return; // Not one of ours

    MarkAffectedTransactionsDirty(tx);
}

void CWallet::MarkAffectedTransactionsDirty(const CTransaction& tx)
{
    if(fDebug)
        fprintf(stderr,"%s: tx=%s\n", __func__, tx.GetHash().ToString().c_str() );
    // If a transaction changes 'conflicted' state, that changes the balance
    // available of the outputs it spends. So force those to be
    // recomputed, also:
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        if (mapWallet.count(txin.prevout.hash))
            mapWallet[txin.prevout.hash].MarkDirty();
    }

    for (const SpendDescription &spend : tx.vShieldedSpend) {
        uint256 nullifier = spend.nullifier;
        if (mapSaplingNullifiersToNotes.count(nullifier) &&
            mapWallet.count(mapSaplingNullifiersToNotes[nullifier].hash)) {
            mapWallet[mapSaplingNullifiersToNotes[nullifier].hash].MarkDirty();
        }
    }
    if(fDebug)
        fprintf(stderr,"%s: finished marking dirty transactions\n", __func__);
}

void CWallet::EraseFromWallet(const uint256 &hash)
{
    if (!fFileBacked)
        return;
    {
        LOCK(cs_wallet);
        if (mapWallet.erase(hash))
            CWalletDB(strWalletFile).EraseTx(hash);
    }

    LogPrintf("%s: erased txid %s\n", __func__, hash.ToString().c_str() );

    return;
}

void CWallet::RescanWallet()
{
    if (needsRescan)
    {
        CBlockIndex *start = chainActive.Height() > 0 ? chainActive[1] : NULL;
        if (start)
            ScanForWalletTransactions(start, true);
        needsRescan = false;
    }
}


/**
 * Finds all output notes in the given transaction that have been sent to
 * SaplingPaymentAddresses in this wallet.
 *
 * It should never be necessary to call this method with a CWalletTx, because
 * the result of FindMySaplingNotes (for the addresses available at the time) will
 * already have been cached in CWalletTx.mapSaplingNoteData.
 */
std::pair<mapSaplingNoteData_t, SaplingIncomingViewingKeyMap> CWallet::FindMySaplingNotes(const CTransaction &tx) const
{
    LOCK(cs_SpendingKeyStore);
    uint256 hash = tx.GetHash();
    uint32_t nZouts = tx.vShieldedOutput.size();
    if(fDebug && (nZouts > 0)) {
        LogPrintf("%s: zouts=%d in tx=%s\n",__func__,nZouts, hash.ToString().c_str());
    }

    mapSaplingNoteData_t noteData;
    SaplingIncomingViewingKeyMap viewingKeysToAdd;

    // Protocol Spec: 4.19 Block Chain Scanning (Sapling)
    for (uint32_t i = 0; i < nZouts; ++i) {
        const OutputDescription output = tx.vShieldedOutput[i];
        bool found = false;
        for (auto it = mapSaplingFullViewingKeys.begin(); it != mapSaplingFullViewingKeys.end(); ++it) {
            SaplingIncomingViewingKey ivk = it->first;
            auto result = SaplingNotePlaintext::decrypt(output.encCiphertext, ivk, output.ephemeralKey, output.cm);
            if (result) {
                auto address = ivk.address(result.get().d);
                if (address && mapSaplingIncomingViewingKeys.count(address.get()) == 0) {
                    viewingKeysToAdd[address.get()] = ivk;
                }
                // We don't cache the nullifier here as computing it requires knowledge of the note position
                // in the commitment tree, which can only be determined when the transaction has been mined.
                SaplingOutPoint op {hash, i};
                SaplingNoteData nd;
                nd.ivk = ivk;
                noteData.insert(std::make_pair(op, nd));
                found = true;
                break;
            }
        }
        if (!found) {
            for (auto it = mapSaplingIncomingViewingKeys.begin(); it != mapSaplingIncomingViewingKeys.end(); ++it) {
                SaplingIncomingViewingKey ivk = it-> second;
                auto result = SaplingNotePlaintext::decrypt(output.encCiphertext, ivk, output.ephemeralKey, output.cm);
                if (!result) {
                    continue;
                }
                // We don't cache the nullifier here as computing it requires knowledge of the note position
                // in the commitment tree, which can only be determined when the transaction has been mined.
                SaplingOutPoint op {hash, i};
                SaplingNoteData nd;
                nd.ivk = ivk;
                noteData.insert(std::make_pair(op, nd));
                break;
            }
        }
    }

    return std::make_pair(noteData, viewingKeysToAdd);
}

bool CWallet::IsSaplingNullifierFromMe(const uint256& nullifier) const
{
    {
        LOCK(cs_wallet);
        if (mapSaplingNullifiersToNotes.count(nullifier) &&
                mapWallet.count(mapSaplingNullifiersToNotes.at(nullifier).hash)) {
            return true;
        }
    }
    return false;
}

void CWallet::GetSaplingNoteWitnesses(std::vector<SaplingOutPoint> notes,
                                      std::vector<boost::optional<SaplingWitness>>& witnesses,
                                      uint256 &final_anchor)
{
    LOCK(cs_wallet);
    witnesses.resize(notes.size());
    boost::optional<uint256> rt;
    int i = 0;
    for (SaplingOutPoint note : notes) {
        //fprintf(stderr,"%s: i=%d\n", __func__,i);
        auto noteData   = mapWallet[note.hash].mapSaplingNoteData;
        auto nWitnesses = noteData[note].witnesses.size();
        if (mapWallet.count(note.hash) && noteData.count(note) && nWitnesses > 0) {
            fprintf(stderr,"%s: Found %lu witnesses for note %s\n", __func__, nWitnesses, note.hash.ToString().c_str() );
            witnesses[i] = noteData[note].witnesses.front();
            if (!rt) {
                //fprintf(stderr,"%s: Setting witness root\n",__func__);
                rt = witnesses[i]->root();
            } else {
                if(*rt == witnesses[i]->root()) {
                    //fprintf(stderr,"%s: rt=%s\n",__func__,rt.GetHash().ToString().c_str());
                    //fprintf(stderr,"%s: witnesses[%d]->root()=%s\n",__func__,i,witnesses[i]->root().GetHash().ToString().c_str());
                    // Something is fucky
                    std::string err = string("CWallet::GetSaplingNoteWitnesses: Invalid witness root! rt=") + rt.get().ToString();
                    err            += string("\n!= witness[i]->root()=") + witnesses[i]->root().ToString();
                    //throw std::logic_error(err);
                    fprintf(stderr,"%s: IGNORING %s\n", __func__,err.c_str());
                }

            }
        }
        i++;
    }
    // All returned witnesses have the same anchor
    if (rt) {
        final_anchor = *rt;
        //fprintf(stderr,"%s: final_anchor=%s\n", __func__, rt.get().ToString().c_str() );
    }
}

isminetype CWallet::IsMine(const CTxIn &txin) const
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                return (::IsMine(*this, prev.vout[txin.prevout.n].scriptPubKey));
        }
    }
    return ISMINE_NO;
}

CAmount CWallet::GetDebit(const CTxIn &txin, const isminefilter& filter) const
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                if (::IsMine(*this, prev.vout[txin.prevout.n].scriptPubKey) & filter)
                    return prev.vout[txin.prevout.n].nValue;
        }
    }
    return 0;
}

isminetype CWallet::IsMine(const CTxOut& txout) const
{
    return ::IsMine(*this, txout.scriptPubKey);
}

CAmount CWallet::GetCredit(const CTxOut& txout, const isminefilter& filter) const
{
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error("CWallet::GetCredit(): value out of range");
    return ((IsMine(txout) & filter) ? txout.nValue : 0);
}

bool CWallet::IsChange(const CTxOut& txout) const
{
    // TODO: fix handling of 'change' outputs. The assumption is that any
    // payment to a script that is ours, but is not in the address book
    // is change. That assumption is likely to break when we implement multisignature
    // wallets that return change back into a multi-signature-protected address;
    // a better way of identifying which outputs are 'the send' and which are
    // 'the change' will need to be implemented (maybe extend CWalletTx to remember
    // which output, if any, was change).
    if (::IsMine(*this, txout.scriptPubKey))
    {
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address))
            return true;

        LOCK(cs_wallet);
        if (!mapAddressBook.count(address))
            return true;
    }
    return false;
}

CAmount CWallet::GetChange(const CTxOut& txout) const
{
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error("CWallet::GetChange(): value out of range");
    return (IsChange(txout) ? txout.nValue : 0);
}

typedef vector<unsigned char> valtype;
unsigned int HaveKeys(const vector<valtype>& pubkeys, const CKeyStore& keystore);

bool CWallet::IsMine(const CTransaction& tx)
{
    for (int i = 0; i < tx.vout.size(); i++)
    {
        if (IsMine(tx, i))
            return true;
    }
    return false;
}

// special case handling for non-standard OP_RETURN script outputs, which need the transaction
// to determine ownership
isminetype CWallet::IsMine(const CTransaction& tx, uint32_t voutNum)
{
    vector<valtype> vSolutions;
    txnouttype whichType;
    const CScript scriptPubKey = CScript(tx.vout[voutNum].scriptPubKey);

    if (!Solver(scriptPubKey, whichType, vSolutions)) {
        if (this->HaveWatchOnly(scriptPubKey))
            return ISMINE_WATCH_ONLY;
        return ISMINE_NO;
    }

    CKeyID keyID;
    CScriptID scriptID;
    CScript subscript;
    int voutNext = voutNum + 1;

    switch (whichType)
    {
        case TX_NONSTANDARD:
        case TX_NULL_DATA:
            break;

        case TX_CRYPTOCONDITION:
            // for now, default is that the first value returned will be the script, subsequent values will be
            // pubkeys. if we have the first pub key in our wallet, we consider this spendable
            if (vSolutions.size() > 1)
            {
                keyID = CPubKey(vSolutions[1]).GetID();
                if (this->HaveKey(keyID))
                    return ISMINE_SPENDABLE;
            }
            break;

        case TX_PUBKEY:
            keyID = CPubKey(vSolutions[0]).GetID();
            if (this->HaveKey(keyID))
                return ISMINE_SPENDABLE;
            break;

        case TX_PUBKEYHASH:
            keyID = CKeyID(uint160(vSolutions[0]));
            if (this->HaveKey(keyID))
                return ISMINE_SPENDABLE;
            break;

        case TX_SCRIPTHASH:
            scriptID = CScriptID(uint160(vSolutions[0]));
			//TODO: remove CLTV stuff not relevant to Hush
            if (this->GetCScript(scriptID, subscript))
            {
                // if this is a CLTV, handle it differently
                if (subscript.IsCheckLockTimeVerify())
                {
                    return (::IsMine(*this, subscript));
                }
                else
                {
                    isminetype ret = ::IsMine(*this, subscript);
                    if (ret == ISMINE_SPENDABLE)
                        return ret;
                }
            }
            else if (tx.vout.size() > (voutNext = voutNum + 1) &&
                tx.vout[voutNext].scriptPubKey.size() > 7 &&
                tx.vout[voutNext].scriptPubKey[0] == OP_RETURN)
            {
                // get the opret script from next vout, verify that the front is CLTV and hash matches
                // if so, remove it and use the solver
                opcodetype op;
                std::vector<uint8_t> opretData;
                CScript::const_iterator it = tx.vout[voutNext].scriptPubKey.begin() + 1;
                if (tx.vout[voutNext].scriptPubKey.GetOp2(it, op, &opretData))
                {
                    if (opretData.size() > 0 && opretData[0] == OPRETTYPE_TIMELOCK)
                    {
                        CScript opretScript = CScript(opretData.begin() + 1, opretData.end());

                        if (CScriptID(opretScript) == scriptID &&
                            opretScript.IsCheckLockTimeVerify())
                        {
                            // if we find that this is ours, we need to add this script to the wallet,
                            // and we can then recognize this transaction
                            isminetype t = ::IsMine(*this, opretScript);
                            if (t != ISMINE_NO)
                            {
                                this->AddCScript(opretScript);
                            }
                            return t;
                        }
                    }
                }
            }
            break;

        case TX_MULTISIG:
            // Only consider transactions "mine" if we own ALL the
            // keys involved. Multi-signature transactions that are
            // partially owned (somebody else has a key that can spend
            // them) enable spend-out-from-under-you attacks, especially
            // in shared-wallet situations.
            vector<valtype> keys(vSolutions.begin()+1, vSolutions.begin()+vSolutions.size()-1);
            if (HaveKeys(keys, *this) == keys.size())
                return ISMINE_SPENDABLE;
            break;
    }

    if (this->HaveWatchOnly(scriptPubKey))
        return ISMINE_WATCH_ONLY;

    return ISMINE_NO;
}

bool CWallet::IsFromMe(const CTransaction& tx) const
{
    if (GetDebit(tx, ISMINE_ALL) > 0) {
        return true;
    }
    for (const SpendDescription &spend : tx.vShieldedSpend) {
        if (IsSaplingNullifierFromMe(spend.nullifier)) {
            return true;
        }
    }
    return false;
}

CAmount CWallet::GetDebit(const CTransaction& tx, const isminefilter& filter) const
{
    CAmount nDebit = 0;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        nDebit += GetDebit(txin, filter);
        if (!MoneyRange(nDebit))
            throw std::runtime_error("CWallet::GetDebit(): value out of range");
    }
    return nDebit;
}

CAmount CWallet::GetCredit(const CTransaction& tx, int32_t voutNum, const isminefilter& filter) const
{
    if (voutNum >= tx.vout.size() || !MoneyRange(tx.vout[voutNum].nValue))
        throw std::runtime_error("CWallet::GetCredit(): value out of range");
    return ((IsMine(tx.vout[voutNum]) & filter) ? tx.vout[voutNum].nValue : 0);
}

CAmount CWallet::GetCredit(const CTransaction& tx, const isminefilter& filter) const
{
    CAmount nCredit = 0;
    for (int i = 0; i < tx.vout.size(); i++)
    {
        nCredit += GetCredit(tx, i, filter);
    }
    return nCredit;
}

CAmount CWallet::GetChange(const CTransaction& tx) const
{
    CAmount nChange = 0;
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        nChange += GetChange(txout);
        if (!MoneyRange(nChange))
            throw std::runtime_error("CWallet::GetChange(): value out of range");
    }
    return nChange;
}

bool CWallet::IsHDFullyEnabled() const
{
    // Only Sapling addresses are HD for now
    return false;
}

void CWallet::GenerateNewSeed()
{
    LOCK(cs_wallet);

    auto seed = HDSeed::Random(HD_WALLET_SEED_LENGTH);

    int64_t nCreationTime = GetTime();

    // If the wallet is encrypted and locked, this will fail.
    if (!SetHDSeed(seed))
        throw std::runtime_error(std::string(__func__) + ": SetHDSeed failed");

    // store the key creation time together with
    // the child index counter in the database
    // as a hdchain object
    CHDChain newHdChain;
    newHdChain.nVersion = CHDChain::VERSION_HD_BASE;
    newHdChain.seedFp = seed.Fingerprint();
    newHdChain.nCreateTime = nCreationTime;
    SetHDChain(newHdChain, false);
}

bool CWallet::SetHDSeed(const HDSeed& seed)
{
    if (!CCryptoKeyStore::SetHDSeed(seed)) {
        return false;
    }

    if (!fFileBacked) {
        return true;
    }

    {
        LOCK(cs_wallet);
        if (!IsCrypted()) {
            return CWalletDB(strWalletFile).WriteHDSeed(seed);
        }
    }
    return true;
}

bool CWallet::SetCryptedHDSeed(const uint256& seedFp, const std::vector<unsigned char> &vchCryptedSecret)
{
    if (!CCryptoKeyStore::SetCryptedHDSeed(seedFp, vchCryptedSecret)) {
        return false;
    }

    if (!fFileBacked) {
        return true;
    }

    {
        LOCK(cs_wallet);
        if (pwalletdbEncryption)
            return pwalletdbEncryption->WriteCryptedHDSeed(seedFp, vchCryptedSecret);
        else
            return CWalletDB(strWalletFile).WriteCryptedHDSeed(seedFp, vchCryptedSecret);
    }
    return false;
}

void CWallet::SetHDChain(const CHDChain& chain, bool memonly)
{
    LOCK(cs_wallet);
    if (!memonly && fFileBacked && !CWalletDB(strWalletFile).WriteHDChain(chain))
        throw std::runtime_error(std::string(__func__) + ": writing chain failed");

    hdChain = chain;
}

bool CWallet::LoadHDSeed(const HDSeed& seed)
{
    return CBasicKeyStore::SetHDSeed(seed);
}

bool CWallet::LoadCryptedHDSeed(const uint256& seedFp, const std::vector<unsigned char>& seed)
{
    return CCryptoKeyStore::SetCryptedHDSeed(seedFp, seed);
}

void CWalletTx::SetSaplingNoteData(mapSaplingNoteData_t &noteData)
{
    mapSaplingNoteData.clear();
    for (const std::pair<SaplingOutPoint, SaplingNoteData> nd : noteData) {
        if (nd.first.n < vShieldedOutput.size()) {
            mapSaplingNoteData[nd.first] = nd.second;
        } else {
            throw std::logic_error("CWalletTx::SetSaplingNoteData(): Invalid note");
        }
    }
}


boost::optional<std::pair<
    SaplingNotePlaintext,
    SaplingPaymentAddress>> CWalletTx::DecryptSaplingNote(SaplingOutPoint op) const
{
    // Check whether we can decrypt this SaplingOutPoint
    if (this->mapSaplingNoteData.count(op) == 0) {
        return boost::none;
    }

    auto output = this->vShieldedOutput[op.n];
    auto nd = this->mapSaplingNoteData.at(op);

    auto maybe_pt = SaplingNotePlaintext::decrypt(
        output.encCiphertext,
        nd.ivk,
        output.ephemeralKey,
        output.cm);
    assert(static_cast<bool>(maybe_pt));
    auto notePt = maybe_pt.get();

    auto maybe_pa = nd.ivk.address(notePt.d);
    assert(static_cast<bool>(maybe_pa));
    auto pa = maybe_pa.get();

    return std::make_pair(notePt, pa);
}

boost::optional<std::pair<
    SaplingNotePlaintext,
    SaplingPaymentAddress>> CWalletTx::RecoverSaplingNote(
        SaplingOutPoint op, std::set<uint256>& ovks) const
{
    auto output = this->vShieldedOutput[op.n];

    for (auto ovk : ovks) {
        auto outPt = SaplingOutgoingPlaintext::decrypt(
            output.outCiphertext,
            ovk,
            output.cv,
            output.cm,
            output.ephemeralKey);
        if (!outPt) {
            continue;
        }

        auto maybe_pt = SaplingNotePlaintext::decrypt(
            output.encCiphertext,
            output.ephemeralKey,
            outPt->esk,
            outPt->pk_d,
            output.cm);
        assert(static_cast<bool>(maybe_pt));
        auto notePt = maybe_pt.get();

        return std::make_pair(notePt, SaplingPaymentAddress(notePt.d, outPt->pk_d));
    }

    // Couldn't recover with any of the provided OutgoingViewingKeys
    return boost::none;
}

int64_t CWalletTx::GetTxTime() const
{
    int64_t n = nTimeSmart;
    return n ? n : nTimeReceived;
}

// GetAmounts will determine the transparent debits and credits for a given wallet tx.
void CWalletTx::GetAmounts(list<COutputEntry>& listReceived,
                           list<COutputEntry>& listSent, CAmount& nFee, string& strSentAccount, const isminefilter& filter) const
{
    nFee = 0;
    listReceived.clear();
    listSent.clear();
    strSentAccount = strFromAccount;

    // Is this tx sent/signed by me?
    CAmount nDebit = GetDebit(filter);
    bool isFromMyTaddr = nDebit > 0; // debit>0 means we signed/sent this transaction

    // Compute fee if we sent this transaction.
    if (isFromMyTaddr) {
        CAmount nValueOut = GetValueOut();  // transparent outputs plus all Sprout vpub_old and negative Sapling valueBalance
        CAmount nValueIn = GetShieldedValueIn();
        nFee = nDebit - nValueOut + nValueIn;
    }

    // Create output entry for vpub_old/new, if we sent utxos from this transaction
    if (isFromMyTaddr) {
        CAmount myVpubOld = 0;
        CAmount myVpubNew = 0;

        // Create an output for the value taken from or added to the transparent value pool by JoinSplits
        if (myVpubOld > myVpubNew) {
            COutputEntry output = {CNoDestination(), myVpubOld - myVpubNew, (int)vout.size()};
            listSent.push_back(output);
        } else if (myVpubNew > myVpubOld) {
            COutputEntry output = {CNoDestination(), myVpubNew - myVpubOld, (int)vout.size()};
            listReceived.push_back(output);
        }
    }

    // If we sent utxos from this transaction, create output for value taken from (negative valueBalance)
    // or added (positive valueBalance) to the transparent value pool by Sapling shielding and unshielding.
    if (isFromMyTaddr) {
        if (valueBalance < 0) {
            COutputEntry output = {CNoDestination(), -valueBalance, (int) vout.size()};
            listSent.push_back(output);
        } else if (valueBalance > 0) {
            COutputEntry output = {CNoDestination(), valueBalance, (int) vout.size()};
            listReceived.push_back(output);
        }
    }

    // Sent/received.
    int32_t oneshot = 0;
    for (unsigned int i = 0; i < vout.size(); ++i)
    {
        const CTxOut& txout = vout[i];
        isminetype fIsMine = pwallet->IsMine(txout);
        // Only need to handle txouts if AT LEAST one of these is true:
        //   1) they debit from us (sent)
        //   2) the output is to us (received)
        if (nDebit > 0)
        {
            // Don't report 'change' txouts
            if (!(filter & ISMINE_CHANGE) && pwallet->IsChange(txout))
            {
                if ( oneshot++ > 1 )
                {
                    //fprintf(stderr,"skip change vout\n");
                    continue;
                }
            }
        }
        else if (!(fIsMine & filter))
        {
            //fprintf(stderr,"skip filtered vout %d %d\n",(int32_t)fIsMine,(int32_t)filter);
            continue;
        }
        // In either case, we need to get the destination address
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address))
        {
            //LogPrintf("CWalletTx::GetAmounts: Unknown transaction type found, txid %s\n",this->GetHash().ToString()); complains on the opreturns
            address = CNoDestination();
        }

        COutputEntry output = {address, txout.nValue, (int)i};

        // If we are debited by the transaction, add the output as a "sent" entry
        if (nDebit > 0)
            listSent.push_back(output);
        //else fprintf(stderr,"not sent vout %d %d\n",(int32_t)fIsMine,(int32_t)filter);

        // If we are receiving the output, add it as a "received" entry
        if (fIsMine & filter)
            listReceived.push_back(output);
        //else fprintf(stderr,"not received vout %d %d\n",(int32_t)fIsMine,(int32_t)filter);
    }

}

void CWalletTx::GetAccountAmounts(const string& strAccount, CAmount& nReceived,
                                  CAmount& nSent, CAmount& nFee, const isminefilter& filter) const
{
    nReceived = nSent = nFee = 0;

    CAmount allFee;
    string strSentAccount;
    list<COutputEntry> listReceived;
    list<COutputEntry> listSent;
    GetAmounts(listReceived, listSent, allFee, strSentAccount, filter);

    if (strAccount == strSentAccount)
    {
        BOOST_FOREACH(const COutputEntry& s, listSent)
            nSent += s.amount;
        nFee = allFee;
    }
    {
        LOCK(pwallet->cs_wallet);
        BOOST_FOREACH(const COutputEntry& r, listReceived)
        {
            if (pwallet->mapAddressBook.count(r.destination))
            {
                map<CTxDestination, CAddressBookData>::const_iterator mi = pwallet->mapAddressBook.find(r.destination);
                if (mi != pwallet->mapAddressBook.end() && (*mi).second.name == strAccount)
                    nReceived += r.amount;
            }
            else if (strAccount.empty())
            {
                nReceived += r.amount;
            }
        }
    }
}


bool CWalletTx::WriteToDisk(CWalletDB *pwalletdb)
{
    return pwalletdb->WriteTx(GetHash(), *this);
}

/**
 * Reorder the transactions based on block hieght and block index.
 * Transactions can get out of order when they are deleted and subsequently
 * re-added during intial load rescan.
 */

void CWallet::ReorderWalletTransactions(std::map<std::pair<int,int>, CWalletTx*> &mapSorted, int64_t &maxOrderPos)
{
    LOCK2(cs_main, cs_wallet);
    if(fZdebug)
        fprintf(stderr,"%s: maxOrderPos=%li\n",__func__, maxOrderPos);

    int maxSortNumber = chainActive.Tip()->GetHeight() + 1;

    for (map<uint256, CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        CWalletTx* pwtx = &(it->second);
        int confirms    = pwtx->GetDepthInMainChain();
        maxOrderPos     = max(maxOrderPos, pwtx->nOrderPos);

        if (confirms > 0) {
            int wtxHeight = mapBlockIndex[pwtx->hashBlock]->GetHeight();
            auto key = std::make_pair(wtxHeight, pwtx->nIndex);
            mapSorted.insert(make_pair(key, pwtx));
        } else {
            auto key = std::make_pair(maxSortNumber, 0);
            mapSorted.insert(std::make_pair(key, pwtx));
            maxSortNumber++;
        }
    }
    if(fZdebug)
        fprintf(stderr,"%s: mapSorted.size=%lu\n",__func__, mapSorted.size());
}
 /**Update the nOrderPos with passed in ordered map.
 */

void CWallet::UpdateWalletTransactionOrder(std::map<std::pair<int,int>, CWalletTx*> &mapSorted, bool resetOrder) {
  LOCK2(cs_main, cs_wallet);

  int64_t previousPosition = 0;
  std::map<const uint256, CWalletTx*> mapUpdatedTxs;

  if(fZdebug)
        fprintf(stderr,"%s: maxSorted.size=%li resetOrder=%d\n",__func__, mapSorted.size(),resetOrder);

  //Check the position of each transaction relative to the previous one.
  for (map<std::pair<int,int>, CWalletTx*>::iterator it = mapSorted.begin(); it != mapSorted.end(); ++it) {
      CWalletTx* pwtx     = it->second;
      const uint256 wtxid = pwtx->GetHash();

      if (pwtx->nOrderPos <= previousPosition || resetOrder) {
          previousPosition++;
          pwtx->nOrderPos = previousPosition;
          mapUpdatedTxs.insert(std::make_pair(wtxid, pwtx));
      } else {
          previousPosition = pwtx->nOrderPos;
      }
  }

  if(fZdebug)
        fprintf(stderr,"%s: updating %li changed transactions\n",__func__, mapUpdatedTxs.size() );

  //Update transactions nOrderPos for transactions that changed
  CWalletDB walletdb(strWalletFile, "r+", false);
  for (map<const uint256, CWalletTx*>::iterator it = mapUpdatedTxs.begin(); it != mapUpdatedTxs.end(); ++it) {
    CWalletTx* pwtx = it->second;
    LogPrintf("%s: Updating Position to %i for Tx %s\n ", __func__, pwtx->nOrderPos, pwtx->GetHash().ToString());
    bool ret = pwtx->WriteToDisk(&walletdb);
    if(fZdebug)
        fprintf(stderr,"%s: wrote data to disk at %s for tx=%s ret=%d\n",__func__, strWalletFile.c_str(), pwtx->GetHash().ToString().c_str(), ret );
    
    mapWallet[pwtx->GetHash()].nOrderPos = pwtx->nOrderPos;
  }
  if(fZdebug)
        fprintf(stderr,"%s: updated nOrderPos on %lu transactions\n",__func__, mapUpdatedTxs.size() );

  //Update Next Wallet Tx Position
  nOrderPosNext = previousPosition++;
  CWalletDB(strWalletFile).WriteOrderPosNext(nOrderPosNext);
  if(fZdebug)
        fprintf(stderr,"%s: wrote data to disk at %s nOrderPosNext=%li\n",__func__, strWalletFile.c_str(), nOrderPosNext );
  LogPrintf("%s: Total Transactions Reordered %i, Next Position %i\n ", __func__, mapUpdatedTxs.size(), nOrderPosNext);
}

/**
 * Delete transactions from the Wallet
 */
void CWallet::DeleteTransactions(std::vector<uint256> &removeTxs) {
    LOCK(cs_wallet);

    int numTx = removeTxs.size();
    if(fZdebug)
        fprintf(stderr,"%s: removeTxs.size=%d\n", __func__, numTx);

    CWalletDB walletdb(strWalletFile, "r+", false);

    for (int i = 0; i< numTx; i++) {
        if (mapWallet.erase(removeTxs[i])) {
            walletdb.EraseTx(removeTxs[i]);
            LogPrintf("%s: Deleted tx %s, %i.\n", __func__, removeTxs[i].ToString(),i);
        } else {
            LogPrintf("%s: Deleting tx %s failed.\n", __func__, removeTxs[i].ToString());
            return;
        }
    }
//TODO: the build system should check for malloc_trim support
#if defined(__linux__)
    malloc_trim(0);
#else
    // On Mac and Win memory isn't kept back upon vector or list member erase, different garbage collector strategy. No need to force trimming.
#endif

    if(fZdebug)
        fprintf(stderr,"%s: finished deleting %d transactions\n", __func__, numTx);
}

void CWallet::DeleteWalletTransactions(const CBlockIndex* pindex) {
      LOCK2(cs_main, cs_wallet);

      int nDeleteAfter = (int)fDeleteTransactionsAfterNBlocks;
      bool runCompact = false;

      if(fZdebug)
        fprintf(stderr,"%s: nDeleteAfter=%d\n",__func__,nDeleteAfter);

      if (pindex && fTxDeleteEnabled) {
        //Check for acentries - exit function if found
        {
            std::list<CAccountingEntry> acentries;
            CWalletDB walletdb(strWalletFile);
            walletdb.ListAccountCreditDebit("*", acentries);
            if (acentries.size() > 0) {
                LogPrintf("deletetx not compatible to account entries\n");
                return;
            }
        }
        //delete transactions

        //Sort Transactions by block and block index
        int64_t maxOrderPos = 0;
        std::map<std::pair<int,int>, CWalletTx*> mapSorted;
        ReorderWalletTransactions(mapSorted, maxOrderPos);
        if (maxOrderPos > int64_t(mapSorted.size())*10) {
          //reset the postion when the max postion is 10x bigger than the
          //number of transactions in the wallet
          LogPrintf("%s: Reorder Tx - maxOrderPos %i mapSorted Size %i\n", __func__, maxOrderPos, int64_t(mapSorted.size())*10);
          UpdateWalletTransactionOrder(mapSorted, true);
        } else {
          UpdateWalletTransactionOrder(mapSorted, false);
        }

        //Process Transactions in sorted order
        int txConflictCount = 0;
        int txUnConfirmed = 0;
        int txCount = 0;
        int txSaveCount = 0;
        std::vector<uint256> removeTxs;

        for (auto & item : mapSorted)
        {

          CWalletTx* pwtx = item.second;
          const uint256& wtxid = pwtx->GetHash();
          bool deleteTx = true;
          txCount += 1;
          int wtxDepth = pwtx->GetDepthInMainChain();

          //Keep anything newer than N Blocks
          if (wtxDepth == 0)
            txUnConfirmed++;

          if (wtxDepth < nDeleteAfter && wtxDepth >= 0) {
            if(fZdebug)
                LogPrintf("%s: Transaction above minimum depth, tx %s\n", __func__, pwtx->GetHash().ToString());
            deleteTx = false;
            txSaveCount++;
            continue;
          } else if (wtxDepth == -1) {
            //Enabled by default
            if (!fTxConflictDeleteEnabled) {
              if(fZdebug)
                LogPrintf("%s: Conflict delete is not enabled tx %s\n", __func__, pwtx->GetHash().ToString());
              deleteTx = false;
              txSaveCount++;
              continue;
            } else {
              txConflictCount++;
            }
          } else {
            //Check for unspent inputs or spend less than N Blocks ago. (Sapling)
            for (auto & pair : pwtx->mapSaplingNoteData) {
              SaplingNoteData nd = pair.second;
              if (!nd.nullifier || pwalletMain->GetSaplingSpendDepth(*nd.nullifier) <= fDeleteTransactionsAfterNBlocks) {
                if(fZdebug)
                  LogPrintf("%s: Unspent sapling input tx %s\n", __func__, pwtx->GetHash().ToString());
                deleteTx = false;
                continue;
              }
            }

            if (!deleteTx) {
              txSaveCount++;
              continue;
            }

            if(fZdebug)
              LogPrintf("%s: Unspent sapling input tx %s\n", __func__, pwtx->GetHash().ToString());

            //Check for outputs that no longer have parents in the wallet. Exclude parents that are in the same transaction. (Sapling)
            for (int i = 0; i < pwtx->vShieldedSpend.size(); i++) {
              const SpendDescription& spendDesc = pwtx->vShieldedSpend[i];
              if (pwalletMain->IsSaplingNullifierFromMe(spendDesc.nullifier)) {
                const uint256& parentHash = pwalletMain->mapSaplingNullifiersToNotes[spendDesc.nullifier].hash;
                const CWalletTx* parent = pwalletMain->GetWalletTx(parentHash);
                if (parent != NULL && parentHash != wtxid) {
                  if(fZdebug)
                    LogPrintf("%s: Parent of sapling tx %s found\n", __func__, pwtx->GetHash().ToString());
                  deleteTx = false;
                  continue;
                }
              }
            }

            if (!deleteTx) {
              txSaveCount++;
              continue;
            }

            if(fZdebug)
              LogPrintf("%s: Checking for unspent transparent inputs or spends less than %d Blocks ago\n",__func__, fDeleteTransactionsAfterNBlocks);

            for (unsigned int i = 0; i < pwtx->vout.size(); i++) {
              CTxDestination address;
              ExtractDestination(pwtx->vout[i].scriptPubKey, address);
              if(IsMine(pwtx->vout[i])) {
                if (pwalletMain->GetSpendDepth(pwtx->GetHash(), i) <= fDeleteTransactionsAfterNBlocks) {
                  if(fZdebug)
                    LogPrintf("%s: Unspent transparent input tx %s\n", __func__, pwtx->GetHash().ToString());
                  deleteTx = false;
                  continue;
                }
              }
            }

            if (!deleteTx) {
              txSaveCount++;
              continue;
            }

            if(fZdebug)
              LogPrintf("%s: Checking for transparent outputs that no longer have parents in the wallet\n",__func__);
            for (int i = 0; i < pwtx->vin.size(); i++) {
              const CTxIn& txin = pwtx->vin[i];
              const uint256& parentHash = txin.prevout.hash;
              const CWalletTx* parent = pwalletMain->GetWalletTx(txin.prevout.hash);
              if (parent != NULL && parentHash != wtxid) {
                if(fZdebug)
                  LogPrintf("%s: Parent of transparent tx %s found\n", __func__, pwtx->GetHash().ToString());
                deleteTx = false;
                continue;
              }
            }

            if (!deleteTx) {
              txSaveCount++;
              continue;
            }

            //Keep Last N Transactions
            if (mapSorted.size() - txCount < fKeepLastNTransactions + txConflictCount + txUnConfirmed) {
              if(fZdebug)
                LogPrint("%s: Transaction set position %i, tx %s\n", __func__, mapSorted.size() - txCount, wtxid.ToString());
              deleteTx = false;
              txSaveCount++;
              continue;
            }
          }

          //Collect everything else for deletion
          if (deleteTx && int(removeTxs.size()) < MAX_DELETE_TX_SIZE) {
            removeTxs.push_back(wtxid);
            runCompact = true;
          }
        }

        //Delete Transactions from wallet
        DeleteTransactions(removeTxs);
        LogPrintf("%s: Total Transaction Count %i, Transactions Deleted %i\n ", __func__, txCount, int(removeTxs.size()));

        //Compress Wallet
        if (runCompact) {
            if(fZdebug)
                fprintf(stderr,"%s: compacting wallet\n",__func__);
            CWalletDB::Compact(bitdb,strWalletFile);
        }
      }
}

/**
 * Scan the block chain (starting in pindexStart) for transactions
 * from or to us. If fUpdate is true, found transactions that already
 * exist in the wallet will be updated.
 */
int CWallet::ScanForWalletTransactions(CBlockIndex* pindexStart, bool fUpdate)
{
    int ret = 0;
    int64_t nNow = GetTime();
    const CChainParams& chainParams = Params();
    if(fZdebug)
      LogPrintf("%s: fUpdate=%d now=%li\n",__func__,fUpdate,nNow);

    pwalletMain->fRescanning = true;
    CBlockIndex* pindex = pindexStart;
    pwalletMain->rescanStartHeight = pindex->GetHeight();
    pwalletMain->rescanHeight      = pwalletMain->rescanStartHeight;
    {
        LOCK2(cs_main, cs_wallet);

        // no need to read and scan block, if block was created before
        // our wallet birthday (as adjusted for block time variability)
        while (pindex && nTimeFirstKey && (pindex->GetBlockTime() < (nTimeFirstKey - 7200))) {
            pindex = chainActive.Next(pindex);
            pwalletMain->rescanHeight = pindex ? pindex->GetHeight() : 0;
        }

        ShowProgress(_("Rescanning..."), 0); // show rescan progress in GUI as dialog or on splashscreen, if -rescan on startup
        double dProgressStart = Checkpoints::GuessVerificationProgress(chainParams.Checkpoints(), pindex, false);
        double dProgressTip = Checkpoints::GuessVerificationProgress(chainParams.Checkpoints(), chainActive.LastTip(), false);

        while (pindex)
        {
            pwalletMain->rescanHeight = pindex->GetHeight();
            if(pwalletMain->fAbortRescan) {
                //TODO: should we update witness caches?
                LogPrintf("%s: Rescan aborted at block %d\n", pwalletMain->rescanHeight);
                pwalletMain->fRescanning = false;
                return ret;
            }
            if (ShutdownRequested()) {
                //TODO: should we update witness caches?
                LogPrintf("%s: Rescan interrupted by shutdown request at block %d\n", pwalletMain->rescanHeight);
                pwalletMain->fRescanning = false;
                return ret;
            }

            if (pindex->GetHeight() % 100 == 0 && dProgressTip - dProgressStart > 0.0)
                ShowProgress(_("Rescanning..."), std::max(1, std::min(99, (int)((Checkpoints::GuessVerificationProgress(chainParams.Checkpoints(), pindex, false) - dProgressStart) / (dProgressTip - dProgressStart) * 100))));

            CBlock block;
            bool involvesMe = false;
            if (!ReadBlockFromDisk(block, pindex,1)) {
                throw std::runtime_error(
                    strprintf("Cannot read block height %d (%s) from disk", pindex->GetHeight(), pindex->GetBlockHash().GetHex()));
            }

            BOOST_FOREACH(CTransaction& tx, block.vtx)
            {
                if (AddToWalletIfInvolvingMe(tx, &block, fUpdate)) {
                    involvesMe = true;
                    ret++;
                }
            }

            SaplingMerkleTree saplingTree;
            // This should never fail: we should always be able to get the tree
            // state on the path to the tip of our chain
            if (pindex->pprev) {
                if (NetworkUpgradeActive(pindex->pprev->GetHeight(), Params().GetConsensus(), Consensus::UPGRADE_SAPLING)) {
                    assert(pcoinsTip->GetSaplingAnchorAt(pindex->pprev->hashFinalSaplingRoot, saplingTree));
                }
            }

            // Build initial witness caches for blocks involving one of our addresses
            if (involvesMe) {
                LogPrintf("%s: block has one of our transactions, building witness cache\n", __func__);
                BuildWitnessCache(pindex, true);
            }

            //Delete Transactions
            if (fTxDeleteEnabled) {
                if (pindex->GetHeight() % fDeleteInterval == 0)
                   DeleteWalletTransactions(pindex);
            }

            if (GetTime() >= nNow + 60) {
                nNow = GetTime();
                LogPrintf("Still rescanning. At block %d. Progress=%f\n", pindex->GetHeight(), Checkpoints::GuessVerificationProgress(chainParams.Checkpoints(), pindex));
            }
            // update rescan height before we scan the next block
            pwalletMain->rescanHeight = pindex->GetHeight();
            pindex = chainActive.Next(pindex);
        }
        
        //Update all witness caches
        BuildWitnessCache(chainActive.Tip(), false);

        ShowProgress(_("Rescanning..."), 100); // hide progress dialog in GUI
    }

    // we are no longer rescanning
    pwalletMain->fRescanning = false;
    return ret;
}

void CWallet::ReacceptWalletTransactions()
{
    int64_t nNow = GetTime();
    if(fZdebug)
      LogPrintf("%s: now=%li\n",__func__,nNow);

    if ( IsInitialBlockDownload() )
        return;
    // If transactions aren't being broadcasted, don't let them into local mempool either
    if (!fBroadcastTransactions)
        return;
    LOCK2(cs_main, cs_wallet);
    std::map<int64_t, CWalletTx*> mapSorted;

    // Sort pending wallet transactions based on their initial wallet insertion order
    BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet)
    {
        const uint256& wtxid = item.first;
        CWalletTx& wtx = item.second;
        if(wtx.GetHash() != wtxid) {
            LogPrintf("%s: Something funky going on, skipping this tx. wtx.GetHash() != wtxid (%s != %s)\n", __func__, wtx.GetHash().ToString().c_str(), wtxid.ToString().c_str() );
            continue;
        }
        // Crashing the node because of this is lame
        // assert(wtx.GetHash() == wtxid);

        int nDepth = wtx.GetDepthInMainChain();

        if (!wtx.IsCoinBase() && nDepth < 0) {
            mapSorted.insert(std::make_pair(wtx.nOrderPos, &wtx));
        }
    }

    std::vector<uint256> vwtxh;

    // Try to add wallet transactions to memory pool
    BOOST_FOREACH(PAIRTYPE(const int64_t, CWalletTx*)& item, mapSorted)
    {
        CWalletTx& wtx = *(item.second);

        LOCK(mempool.cs);
        CValidationState state;
        // attempt to add them, but don't set any DOS level
        if (!::AcceptToMemoryPool(mempool, state, wtx, false, NULL, true, 0))
        {
            int nDoS;
            bool invalid = state.IsInvalid(nDoS);

            // log rejection and deletion
            //printf("ERROR reaccepting wallet transaction %s to mempool, reason: %s, DoS: %d\n", wtx.GetHash().ToString().c_str(), state.GetRejectReason().c_str(), nDoS);

            if (!wtx.IsCoinBase() && invalid && nDoS > 0 && state.GetRejectReason() != "tx-overwinter-expired")
            {
                LogPrintf("%s: erasing transaction %s\n", __func__, wtx.GetHash().GetHex().c_str());
                vwtxh.push_back(wtx.GetHash());
            }
        }
    }
    for (auto hash : vwtxh)
    {
        EraseFromWallet(hash);
    }
}

bool CWalletTx::RelayWalletTransaction()
{
    int64_t nNow = GetTime();
    //if(fZdebug)
    //  LogPrintf("%s: now=%li\n",__func__,nNow);
    if ( pwallet == 0 )
    {
        //fprintf(stderr,"unexpected null pwallet in RelayWalletTransaction\n");
        return(false);
    }
    assert(pwallet->GetBroadcastTransactions());
    if (!IsCoinBase())
    {
        if (GetDepthInMainChain() == 0)
        {
            // if tx is expired, dont relay
            LogPrintf("Relaying wtx %s\n", GetHash().ToString());
            RelayTransaction((CTransaction)*this);
            return true;
        }
    }
    return false;
}

set<uint256> CWalletTx::GetConflicts() const
{
    set<uint256> result;
    if (pwallet != NULL)
    {
        uint256 myHash = GetHash();
        result = pwallet->GetConflicts(myHash);
        result.erase(myHash);
    }
    return result;
}

CAmount CWalletTx::GetDebit(const isminefilter& filter) const
{
    if (vin.empty())
        return 0;

    CAmount debit = 0;
    if(filter & ISMINE_SPENDABLE)
    {
        if (fDebitCached)
            debit += nDebitCached;
        else
        {
            nDebitCached = pwallet->GetDebit(*this, ISMINE_SPENDABLE);
            fDebitCached = true;
            debit += nDebitCached;
        }
    }
    if(filter & ISMINE_WATCH_ONLY)
    {
        if(fWatchDebitCached)
            debit += nWatchDebitCached;
        else
        {
            nWatchDebitCached = pwallet->GetDebit(*this, ISMINE_WATCH_ONLY);
            fWatchDebitCached = true;
            debit += nWatchDebitCached;
        }
    }
    return debit;
}

CAmount CWalletTx::GetCredit(const isminefilter& filter) const
{
    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (IsCoinBase() && GetBlocksToMaturity() > 0)
        return 0;

    int64_t credit = 0;
    if (filter & ISMINE_SPENDABLE)
    {
        // GetBalance can assume transactions in mapWallet won't change
        if (fCreditCached)
            credit += nCreditCached;
        else
        {
            nCreditCached = pwallet->GetCredit(*this, ISMINE_SPENDABLE);
            fCreditCached = true;
            credit += nCreditCached;
        }
    }
    if (filter & ISMINE_WATCH_ONLY)
    {
        if (fWatchCreditCached)
            credit += nWatchCreditCached;
        else
        {
            nWatchCreditCached = pwallet->GetCredit(*this, ISMINE_WATCH_ONLY);
            fWatchCreditCached = true;
            credit += nWatchCreditCached;
        }
    }
    return credit;
}

CAmount CWalletTx::GetImmatureCredit(bool fUseCache) const
{
    if (IsCoinBase() && GetBlocksToMaturity() > 0 && IsInMainChain())
    {
        if (fUseCache && fImmatureCreditCached)
            return nImmatureCreditCached;
        nImmatureCreditCached = pwallet->GetCredit(*this, ISMINE_SPENDABLE);
        fImmatureCreditCached = true;
        return nImmatureCreditCached;
    }

    return 0;
}

CAmount CWalletTx::GetAvailableCredit(bool fUseCache) const
{
    if (pwallet == 0)
        return 0;

    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (IsCoinBase() && GetBlocksToMaturity() > 0)
        return 0;

    if (fUseCache && fAvailableCreditCached)
        return nAvailableCreditCached;

    CAmount nCredit = 0;
    uint256 hashTx = GetHash();
    for (unsigned int i = 0; i < vout.size(); i++)
    {
        if (!pwallet->IsSpent(hashTx, i))
        {
            nCredit += pwallet->GetCredit(*this, i, ISMINE_SPENDABLE);
            if (!MoneyRange(nCredit))
                throw std::runtime_error("CWalletTx::GetAvailableCredit() : value out of range");
        }
    }

    nAvailableCreditCached = nCredit;
    fAvailableCreditCached = true;
    return nCredit;
}

CAmount CWalletTx::GetImmatureWatchOnlyCredit(const bool& fUseCache) const
{
    if (IsCoinBase() && GetBlocksToMaturity() > 0 && IsInMainChain())
    {
        if (fUseCache && fImmatureWatchCreditCached)
            return nImmatureWatchCreditCached;
        nImmatureWatchCreditCached = pwallet->GetCredit(*this, ISMINE_WATCH_ONLY);
        fImmatureWatchCreditCached = true;
        return nImmatureWatchCreditCached;
    }

    return 0;
}

CAmount CWalletTx::GetAvailableWatchOnlyCredit(const bool& fUseCache) const
{
    if (pwallet == 0)
        return 0;

    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (IsCoinBase() && GetBlocksToMaturity() > 0)
        return 0;

    if (fUseCache && fAvailableWatchCreditCached)
        return nAvailableWatchCreditCached;

    CAmount nCredit = 0;
    for (unsigned int i = 0; i < vout.size(); i++)
    {
        if (!pwallet->IsSpent(GetHash(), i))
        {
            nCredit += pwallet->GetCredit(*this, i, ISMINE_WATCH_ONLY);
            if (!MoneyRange(nCredit))
                throw std::runtime_error("CWalletTx::GetAvailableCredit() : value out of range");
        }
    }

    nAvailableWatchCreditCached = nCredit;
    fAvailableWatchCreditCached = true;
    return nCredit;
}

CAmount CWalletTx::GetChange() const
{
    if (fChangeCached)
        return nChangeCached;
    nChangeCached = pwallet->GetChange(*this);
    fChangeCached = true;
    return nChangeCached;
}

bool CWalletTx::IsTrusted() const
{
    // Quick answer in most cases
    if (!CheckFinalTx(*this))
        return false;
    int nDepth = GetDepthInMainChain();
    if (nDepth >= 1)
        return true;
    if (nDepth < 0)
        return false;
    if (!bSpendZeroConfChange || !IsFromMe(ISMINE_ALL)) // using wtx's cached debit
        return false;

    // Trusted if all inputs are from us and are in the mempool:
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        // Transactions not sent by us: not trusted
        const CWalletTx* parent = pwallet->GetWalletTx(txin.prevout.hash);
        if (parent == NULL)
            return false;
        const CTxOut& parentOut = parent->vout[txin.prevout.n];
        if (pwallet->IsMine(parentOut) != ISMINE_SPENDABLE)
            return false;
    }
    return true;
}

std::vector<uint256> CWallet::ResendWalletTransactionsBefore(int64_t nTime)
{
    std::vector<uint256> result;

    LOCK(cs_wallet);
    // Sort them in chronological order
    multimap<unsigned int, CWalletTx*> mapSorted;
    uint32_t now = (uint32_t)time(NULL);
    LogPrintf("%s: nTime=%ld now=%d\n", __func__, nTime, now);

    // vector of wallet transactions to delete
    std::vector<uint256> vwtxh;
    uint32_t erased = 0, skipped = 0;

    BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet)
    {
        CWalletTx& wtx = item.second;
        // Don't rebroadcast if newer than nTime:
        if (wtx.nTimeReceived > nTime)
            continue;

        // Do not relay expired transactions, to avoid other nodes banning us
        // Current code will not ban nodes relaying expired txs but older nodes will
        if (wtx.nExpiryHeight > 0 && wtx.nExpiryHeight < chainActive.LastTip()->GetHeight()) {
            fprintf(stderr,"%s: ignoring expired tx %s with expiry %d at height %d\n", __func__, wtx.GetHash().ToString().c_str(), wtx.nExpiryHeight, chainActive.LastTip()->GetHeight() );
            // TODO: expired detection doesn't seem to work right
            // append to list of txs to delete
            // vwtxh.push_back(wtx.GetHash());
            continue;
        }

        if ( (wtx.nLockTime >= LOCKTIME_THRESHOLD && wtx.nLockTime < now-HUSH_MAXMEMPOOLTIME) )
        {
            if(fDebug) {
                LogPrintf("%s: skip Relaying wtx %s nLockTime %u vs now.%u\n", __func__, wtx.GetHash().ToString(),(uint32_t)wtx.nLockTime,now);
            }
            skipped++;
            // TODO: this does not seem to handle rescanning+finding old coinbase txs correctly
            //vwtxh.push_back(wtx.GetHash());
            continue;
        }
        mapSorted.insert(make_pair(wtx.nTimeReceived, &wtx));
    }

    BOOST_FOREACH(PAIRTYPE(const unsigned int, CWalletTx*)& item, mapSorted)
    {
        if ( item.second != 0 )
        {
            CWalletTx &wtx = *item.second;
            if (wtx.RelayWalletTransaction())
                result.push_back(wtx.GetHash());
        }
    }

    // Unless we remove these unconfirmed and/or expired txs from the wallet, they will
    // persist there forever. They are too old to be accepted by network
    // consensus rules, so we erase them.
    // Expired txs are always unconfirmed, but unconfirmed tx's could be expired or not,
    // i.e. expired txs are a subset of unconfirmed tx's. Expired tx's can never be included
    // in a block because they are against consensus rules. Unconfirmed tx's might still be
    // included in a future block.
    for (auto hash : vwtxh)
    {
        EraseFromWallet(hash);
        erased++;
    }

    if(erased > 0 || skipped > 0) {
        LogPrintf("%s: Prevented relaying %d and erased %d transactions which are too old\n", __func__, skipped, erased);
    }

    return result;
}

void CWallet::ResendWalletTransactions(int64_t nBestBlockTime)
{
    // Do this infrequently and randomly to avoid giving away
    // that these are our transactions.
    if (GetTime() < nNextResend || !fBroadcastTransactions)
        return;
    bool fFirst = (nNextResend == 0);
    // TODO: BTC Core changed this to be every 12 hours instead of every 30 mins
    nNextResend = GetTime() + GetRand(30 * 60);
    if (fFirst)
        return;

    // do not resend during IBD/rescan because some txs will be unconfirmed
    // until completion
    if(IsInitialBlockDownload()) {
        return;
    }
    if (pwalletMain->fRescanning) {
        return;
    }

    // do not resend during a reindex or initial loading of blocks
    if (HUSH_LOADINGBLOCKS) {
        return;
    }

    // Only do it if there's been a new block since last time
    if (nBestBlockTime < nLastResend)
        return;
    nLastResend = GetTime();

    LogPrintf("%s: nBestBlockTime=%ld nNextResend=%ld nLastResend=%ld time=%ld\n", __func__, nBestBlockTime, nNextResend, nLastResend, GetTime());
    // Rebroadcast unconfirmed txes older than 5 minutes before the last
    // block was found:
    std::vector<uint256> relayed = ResendWalletTransactionsBefore(nBestBlockTime-5*60);
    if (!relayed.empty())
        LogPrintf("%s: rebroadcast %u unconfirmed transactions\n", __func__, relayed.size());
}

/** @} */ // end of mapWallet

/** @defgroup Actions
 *
 * @{
 */

CAmount CWallet::GetBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted())
                nTotal += pcoin->GetAvailableCredit();
        }
    }

    return nTotal;
}

CAmount CWallet::GetUnconfirmedBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (!CheckFinalTx(*pcoin) || (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0))
                nTotal += pcoin->GetAvailableCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetImmatureBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            nTotal += pcoin->GetImmatureCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetWatchOnlyBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted())
                nTotal += pcoin->GetAvailableWatchOnlyCredit();
        }
    }

    return nTotal;
}

CAmount CWallet::GetUnconfirmedWatchOnlyBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (!CheckFinalTx(*pcoin) || (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0))
                nTotal += pcoin->GetAvailableWatchOnlyCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetImmatureWatchOnlyBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            nTotal += pcoin->GetImmatureWatchOnlyCredit();
        }
    }
    return nTotal;
}

/**
 * populate vCoins with vector of available COutputs.
 */

void CWallet::AvailableCoins(vector<COutput>& vCoins, bool fOnlyConfirmed, const CCoinControl *coinControl, bool fIncludeZeroValue, bool fIncludeCoinBase) const
{
    uint64_t *ptr;
    vCoins.clear();

    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const uint256& wtxid = it->first;
            const CWalletTx* pcoin = &(*it).second;

            if (!CheckFinalTx(*pcoin))
                continue;

            if (fOnlyConfirmed && !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && !fIncludeCoinBase)
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth < 0)
                continue;

            for (int i = 0; i < pcoin->vout.size(); i++)
            {
                isminetype mine = IsMine(pcoin->vout[i]);
                if (!(IsSpent(wtxid, i)) && mine != ISMINE_NO &&
                    !IsLockedCoin((*it).first, i) && (pcoin->vout[i].nValue > 0 || fIncludeZeroValue) &&
                    (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i)))
                {
                    ptr = (uint64_t *)&pcoin->vout[i].interest;
                    (*ptr) = 0;
                    vCoins.push_back(COutput(pcoin, i, nDepth, (mine & ISMINE_SPENDABLE) != ISMINE_NO));
                }
            }
        }
    }
}

static void ApproximateBestSubset(vector<pair<CAmount, pair<const CWalletTx*,unsigned int> > >vValue, const CAmount& nTotalLower, const CAmount& nTargetValue,vector<char>& vfBest, CAmount& nBest, int iterations = 1000)
{
    vector<char> vfIncluded;

    vfBest.assign(vValue.size(), true);
    nBest = nTotalLower;

    seed_insecure_rand();

    for (int nRep = 0; nRep < iterations && nBest != nTargetValue; nRep++)
    {
        vfIncluded.assign(vValue.size(), false);
        CAmount nTotal = 0;
        bool fReachedTarget = false;
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++)
        {
            for (unsigned int i = 0; i < vValue.size(); i++)
            {
                //The solver here uses a randomized algorithm,
                //the randomness serves no real security purpose but is just
                //needed to prevent degenerate behavior and it is important
                //that the rng is fast. We do not use a constant random sequence,
                //because there may be some privacy improvement by making
                //the selection random.
                if (nPass == 0 ? insecure_rand()&1 : !vfIncluded[i])
                {
                    nTotal += vValue[i].first;
                    vfIncluded[i] = true;
                    if (nTotal >= nTargetValue)
                    {
                        fReachedTarget = true;
                        if (nTotal < nBest)
                        {
                            nBest = nTotal;
                            vfBest = vfIncluded;
                        }
                        nTotal -= vValue[i].first;
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }
}

bool CWallet::SelectCoinsMinConf(const CAmount& nTargetValue, int nConfMine, int nConfTheirs, vector<COutput> vCoins,set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, CAmount& nValueRet) const
{
    int32_t count = 0; //uint64_t lowest_interest = 0;
    setCoinsRet.clear();
    //memset(interests,0,sizeof(interests));
    nValueRet = 0;
    // List of values less than target
    pair<CAmount, pair<const CWalletTx*,unsigned int> > coinLowestLarger;
    coinLowestLarger.first = std::numeric_limits<CAmount>::max();
    coinLowestLarger.second.first = NULL;
    vector<pair<CAmount, pair<const CWalletTx*,unsigned int> > > vValue;
    CAmount nTotalLower = 0;

    random_shuffle(vCoins.begin(), vCoins.end(), GetRandInt);

    BOOST_FOREACH(const COutput &output, vCoins)
    {
        if (!output.fSpendable)
            continue;

        const CWalletTx *pcoin = output.tx;

        if (output.nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? nConfMine : nConfTheirs))
            continue;

        int i = output.i;
        CAmount n = pcoin->vout[i].nValue;

        pair<CAmount,pair<const CWalletTx*,unsigned int> > coin = make_pair(n,make_pair(pcoin, i));

        if (n == nTargetValue)
        {
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
            return true;
        }
        else if (n < nTargetValue + CENT)
        {
            vValue.push_back(coin);
            nTotalLower += n;
            if ( nTotalLower > 4*nTargetValue + CENT )
            {
                //fprintf(stderr,"why bother with all the utxo if we have double what is needed?\n");
                break;
            }
        } else if (n < coinLowestLarger.first)
        {
            coinLowestLarger = coin;
        }
    }

    if (nTotalLower == nTargetValue)
    {
        for (unsigned int i = 0; i < vValue.size(); ++i)
        {
            setCoinsRet.insert(vValue[i].second);
            nValueRet += vValue[i].first;
        }
        return true;
    }

    if (nTotalLower < nTargetValue)
    {
        if (coinLowestLarger.second.first == NULL)
            return false;
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
        return true;
    }

    // Solve subset sum by stochastic approximation
    sort(vValue.rbegin(), vValue.rend(), CompareValueOnly());
    vector<char> vfBest;
    CAmount nBest;

    ApproximateBestSubset(vValue, nTotalLower, nTargetValue, vfBest, nBest, 1000);
    if (nBest != nTargetValue && nTotalLower >= nTargetValue + CENT)
        ApproximateBestSubset(vValue, nTotalLower, nTargetValue + CENT, vfBest, nBest, 1000);

    // If we have a bigger coin and (either the stochastic approximation didn't find a good solution,
    //                                   or the next bigger coin is closer), return the bigger coin
    if (coinLowestLarger.second.first &&
        ((nBest != nTargetValue && nBest < nTargetValue + CENT) || coinLowestLarger.first <= nBest))
    {
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
    } else {
        for (unsigned int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
            {
                setCoinsRet.insert(vValue[i].second);
                nValueRet += vValue[i].first;
            }

        LogPrint("selectcoins", "SelectCoins() best subset: ");
        for (unsigned int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
                LogPrint("selectcoins", "%s", FormatMoney(vValue[i].first));
        LogPrint("selectcoins", "total %s\n", FormatMoney(nBest));
    }

    return true;
}

bool CWallet::SelectCoins(const CAmount& nTargetValue, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, CAmount& nValueRet,  bool& fOnlyCoinbaseCoinsRet, bool& fNeedCoinbaseCoinsRet, const CCoinControl* coinControl) const
{
    // Output parameter fOnlyCoinbaseCoinsRet is set to true when the only available coins are coinbase utxos.
    uint64_t tmp; int32_t retval;
    vector<COutput> vCoinsNoCoinbase, vCoinsWithCoinbase;
    AvailableCoins(vCoinsNoCoinbase, true, coinControl, false, false);
    AvailableCoins(vCoinsWithCoinbase, true, coinControl, false, true);
    fOnlyCoinbaseCoinsRet = vCoinsNoCoinbase.size() == 0 && vCoinsWithCoinbase.size() > 0;

    // If coinbase utxos can only be sent to zaddrs, exclude any coinbase utxos from coin selection.
    bool fProtectCoinbase = Params().GetConsensus().fCoinbaseMustBeProtected;
    vector<COutput> vCoins = (fProtectCoinbase) ? vCoinsNoCoinbase : vCoinsWithCoinbase;

    // Output parameter fNeedCoinbaseCoinsRet is set to true if coinbase utxos need to be spent to meet target amount
    if (fProtectCoinbase && vCoinsWithCoinbase.size() > vCoinsNoCoinbase.size()) {
        CAmount value = 0;
        for (const COutput& out : vCoinsNoCoinbase) {
            if (!out.fSpendable) {
                continue;
            }
            value += out.tx->vout[out.i].nValue;
            value += out.tx->vout[out.i].interest;
        }
        if (value <= nTargetValue) {
            CAmount valueWithCoinbase = 0;
            for (const COutput& out : vCoinsWithCoinbase) {
                if (!out.fSpendable) {
                    continue;
                }
                valueWithCoinbase += out.tx->vout[out.i].nValue;
                valueWithCoinbase += out.tx->vout[out.i].interest;
            }
            fNeedCoinbaseCoinsRet = (valueWithCoinbase >= nTargetValue);
        }
    }
    // coin control -> return all selected outputs (we want all selected to go into the transaction for sure)
    if (coinControl && coinControl->HasSelected() && !coinControl->fAllowOtherInputs)
    {
        BOOST_FOREACH(const COutput& out, vCoins)
        {
            if (!out.fSpendable)
                 continue;
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.insert(make_pair(out.tx, out.i));
        }
        return (nValueRet >= nTargetValue);
    }
    // calculate value from preset inputs and store them
    set<pair<const CWalletTx*, uint32_t> > setPresetCoins;
    CAmount nValueFromPresetInputs = 0;

    std::vector<COutPoint> vPresetInputs;
    if (coinControl)
        coinControl->ListSelected(vPresetInputs);
    BOOST_FOREACH(const COutPoint& outpoint, vPresetInputs)
    {
        map<uint256, CWalletTx>::const_iterator it = mapWallet.find(outpoint.hash);
        if (it != mapWallet.end())
        {
            const CWalletTx* pcoin = &it->second;
            // Clearly invalid input, fail
            if (pcoin->vout.size() <= outpoint.n)
                return false;
            nValueFromPresetInputs += pcoin->vout[outpoint.n].nValue;
            nValueFromPresetInputs += pcoin->vout[outpoint.n].interest;
            setPresetCoins.insert(make_pair(pcoin, outpoint.n));
        } else
            return false; // TODO: Allow non-wallet inputs
    }

    // remove preset inputs from vCoins
    for (vector<COutput>::iterator it = vCoins.begin(); it != vCoins.end() && coinControl && coinControl->HasSelected();)
    {
        if (setPresetCoins.count(make_pair(it->tx, it->i)))
            it = vCoins.erase(it);
        else
            ++it;
    }
    retval = false;
    if ( nTargetValue <= nValueFromPresetInputs )
        retval = true;
    else if ( SelectCoinsMinConf(nTargetValue, 1, 6, vCoins, setCoinsRet, nValueRet) != 0 )
        retval = true;
    else if ( SelectCoinsMinConf(nTargetValue, 1, 1, vCoins, setCoinsRet, nValueRet) != 0 )
        retval = true;
    else if ( bSpendZeroConfChange && SelectCoinsMinConf(nTargetValue, 0, 1, vCoins, setCoinsRet, nValueRet) != 0 )
        retval = true;
    // because SelectCoinsMinConf clears the setCoinsRet, we now add the possible inputs to the coinset
    setCoinsRet.insert(setPresetCoins.begin(), setPresetCoins.end());
    // add preset inputs to the total value selected
    nValueRet += nValueFromPresetInputs;
    return retval;
}

bool CWallet::FundTransaction(CMutableTransaction& tx, CAmount &nFeeRet, int& nChangePosRet, std::string& strFailReason)
{
    vector<CRecipient> vecSend;

    // Turn the txout set into a CRecipient vector
    BOOST_FOREACH(const CTxOut& txOut, tx.vout)
    {
        CRecipient recipient = {txOut.scriptPubKey, txOut.nValue, false};
        vecSend.push_back(recipient);
    }

    CCoinControl coinControl;
    coinControl.fAllowOtherInputs = true;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        coinControl.Select(txin.prevout);

    CReserveKey reservekey(this);
    CWalletTx wtx;

    if (!CreateTransaction(vecSend, wtx, reservekey, nFeeRet, nChangePosRet, strFailReason, &coinControl, false))
        return false;

    if (nChangePosRet != -1)
        tx.vout.insert(tx.vout.begin() + nChangePosRet, wtx.vout[nChangePosRet]);

    // Add new txins (keeping original txin scriptSig/order)
    BOOST_FOREACH(const CTxIn& txin, wtx.vin)
    {
        bool found = false;
        BOOST_FOREACH(const CTxIn& origTxIn, tx.vin)
        {
            if (txin.prevout.hash == origTxIn.prevout.hash && txin.prevout.n == origTxIn.prevout.n)
            {
                found = true;
                break;
            }
        }
        if (!found)
            tx.vin.push_back(txin);
    }

    return true;
}

bool CWallet::CreateTransaction(const vector<CRecipient>& vecSend, CWalletTx& wtxNew, CReserveKey& reservekey, CAmount& nFeeRet,
                                int& nChangePosRet, std::string& strFailReason, const CCoinControl* coinControl, bool sign)
{
    uint64_t interest2 = 0; CAmount nValue = 0; unsigned int nSubtractFeeFromAmount = 0;
    BOOST_FOREACH (const CRecipient& recipient, vecSend)
    {
        if (nValue < 0 || recipient.nAmount < 0)
        {
            strFailReason = _("Transaction amounts must be positive");
            return false;
        }
        nValue += recipient.nAmount;

        if (recipient.fSubtractFeeFromAmount)
            nSubtractFeeFromAmount++;
    }
    if (vecSend.empty() || nValue < 0)
    {
        strFailReason = _("Transaction amounts must be positive");
        return false;
    }

    wtxNew.fTimeReceivedIsTxTime = true;
    wtxNew.BindWallet(this);
    int nextBlockHeight = chainActive.Height() + 1;
    CMutableTransaction txNew = CreateNewContextualCMutableTransaction(Params().GetConsensus(), nextBlockHeight);
    
    if ( !hush_hardfork_active((uint32_t)chainActive.LastTip()->nTime) )
        txNew.nLockTime = (uint32_t)chainActive.LastTip()->nTime + 1; // set to a time close to now
    else
        txNew.nLockTime = (uint32_t)chainActive.Tip()->GetMedianTimePast();

    // Activates after Overwinter network upgrade
    if (NetworkUpgradeActive(nextBlockHeight, Params().GetConsensus(), Consensus::UPGRADE_OVERWINTER)) {
        if (txNew.nExpiryHeight >= TX_EXPIRY_HEIGHT_THRESHOLD){
            strFailReason = _("nExpiryHeight must be less than TX_EXPIRY_HEIGHT_THRESHOLD.");
            return false;
        }
    }

    unsigned int max_tx_size = MAX_TX_SIZE_AFTER_SAPLING;
    if (!NetworkUpgradeActive(nextBlockHeight, Params().GetConsensus(), Consensus::UPGRADE_SAPLING)) {
        max_tx_size = MAX_TX_SIZE_BEFORE_SAPLING;
    }
/*
    // Discourage fee sniping.
    //
    // However because of a off-by-one-error in previous versions we need to
    // neuter it by setting nLockTime to at least one less than nBestHeight.
    // Secondly currently propagation of transactions created for block heights
    // corresponding to blocks that were just mined may be iffy - transactions
    // aren't re-accepted into the mempool - we additionally neuter the code by
    // going ten blocks back. Doesn't yet do anything for sniping, but does act
    // to shake out wallet bugs like not showing nLockTime'd transactions at
    // all.
    txNew.nLockTime = std::max(0, chainActive.Height() - 10);

    // Secondly occasionally randomly pick a nLockTime even further back, so
    // that transactions that are delayed after signing for whatever reason,
    // e.g. high-latency mix networks and some CoinJoin implementations, have
    // better privacy.
    if (GetRandInt(10) == 0)
        txNew.nLockTime = std::max(0, (int)txNew.nLockTime - GetRandInt(100));

    assert(txNew.nLockTime <= (unsigned int)chainActive.Height());
    assert(txNew.nLockTime < LOCKTIME_THRESHOLD);*/

    {
        LOCK2(cs_main, cs_wallet);
        {
            nFeeRet = 0;
            while (true)
            {
                //interest = 0;
                txNew.vin.clear();
                txNew.vout.clear();
                wtxNew.fFromMe = true;
                nChangePosRet = -1;
                bool fFirst = true;

                CAmount nTotalValue = nValue;
                if (nSubtractFeeFromAmount == 0)
                    nTotalValue += nFeeRet;
                double dPriority = 0;
                // vouts to the payees
                BOOST_FOREACH (const CRecipient& recipient, vecSend)
                {
                    CTxOut txout(recipient.nAmount, recipient.scriptPubKey);

                    if (recipient.fSubtractFeeFromAmount)
                    {
                        txout.nValue -= nFeeRet / nSubtractFeeFromAmount; // Subtract fee equally from each selected recipient

                        if (fFirst) // first receiver pays the remainder not divisible by output count
                        {
                            fFirst = false;
                            txout.nValue -= nFeeRet % nSubtractFeeFromAmount;
                        }
                    }

                    if (txout.IsDust(::minRelayTxFee))
                    {
                        if (recipient.fSubtractFeeFromAmount && nFeeRet > 0)
                        {
                            if (txout.nValue < 0)
                                strFailReason = _("The transaction amount is too small to pay the fee");
                            else
                                strFailReason = _("The transaction amount is too small to send after the fee has been deducted");
                        }
                        else
                            strFailReason = _("Transaction amount too small");
                        return false;
                    }
                    txNew.vout.push_back(txout);
                }

                // Choose coins to use
                set<pair<const CWalletTx*,unsigned int> > setCoins;
                CAmount nValueIn = 0;
                bool fOnlyCoinbaseCoins = false;
                bool fNeedCoinbaseCoins = false;
                interest2 = 0;
                if (!SelectCoins(nTotalValue, setCoins, nValueIn, fOnlyCoinbaseCoins, fNeedCoinbaseCoins, coinControl))
                {
                    if (fOnlyCoinbaseCoins && Params().GetConsensus().fCoinbaseMustBeProtected) {
                        strFailReason = _("Coinbase funds can only be sent to a zaddr");
                    } else if (fNeedCoinbaseCoins) {
                        strFailReason = _("Insufficient funds, coinbase funds can only be spent after they have been sent to a zaddr");
                    } else {
                        strFailReason = _("Insufficient funds");
                    }
                    return false;
                }
                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
                {
                    CAmount nCredit = pcoin.first->vout[pcoin.second].nValue;
                    //The coin age after the next block (depth+1) is used instead of the current,
                    //reflecting an assumption the user would accept a bit more delay for
                    //a chance at a free transaction.
                    //But mempool inputs might still be in the mempool, so their age stays 0
                    //fprintf(stderr,"nCredit %.8f interest %.8f\n",(double)nCredit/COIN,(double)pcoin.first->vout[pcoin.second].interest/COIN);
                    int age = pcoin.first->GetDepthInMainChain();
                    if (age != 0)
                        age += 1;
                    dPriority += (double)nCredit * age;
                }

                CAmount nChange = (nValueIn - nValue + interest2);
//fprintf(stderr,"wallet change %.8f (%.8f - %.8f) interest2 %.8f total %.8f\n",(double)nChange/COIN,(double)nValueIn/COIN,(double)nValue/COIN,(double)interest2/COIN,(double)nTotalValue/COIN);
                if (nSubtractFeeFromAmount == 0)
                    nChange -= nFeeRet;

                if (nChange > 0)
                {
                    // Fill a vout to ourself
                    // TODO: pass in scriptChange instead of reservekey so
                    // change transaction isn't always pay-to-bitcoin-address
                    CScript scriptChange;

                    // coin control: send change to custom address
                    if (coinControl && !boost::get<CNoDestination>(&coinControl->destChange))
                        scriptChange = GetScriptForDestination(coinControl->destChange);

                    // no coin control: send change to newly generated address
                    else
                    {
                        // Note: We use a new key here to keep it from being obvious which side is the change.
                        //  The drawback is that by not reusing a previous key, the change may be lost if a
                        //  backup is restored, if the backup doesn't have the new private key for the change.
                        //  If we reused the old key, it would be possible to add code to look for and
                        //  rediscover unknown transactions that were written with keys of ours to recover
                        //  post-backup change.

                        // Reserve a new key pair from key pool
                        CPubKey vchPubKey;
                        extern int32_t USE_EXTERNAL_PUBKEY; extern std::string NOTARY_PUBKEY;
                        if ( USE_EXTERNAL_PUBKEY == 0 )
                        {
                            bool ret;
                            ret = reservekey.GetReservedKey(vchPubKey);
                            assert(ret); // should never fail, as we just unlocked
                            scriptChange = GetScriptForDestination(vchPubKey.GetID());
                        }
                        else
                        {
                            //fprintf(stderr,"use notary pubkey\n");
                            scriptChange = CScript() << ParseHex(NOTARY_PUBKEY) << OP_CHECKSIG;
                        }
                    }

                    CTxOut newTxOut(nChange, scriptChange);

                    // We do not move dust-change to fees, because the sender would end up paying more than requested.
                    // This would be against the purpose of the all-inclusive feature.
                    // So instead we raise the change and deduct from the recipient.
                    if (nSubtractFeeFromAmount > 0 && newTxOut.IsDust(::minRelayTxFee))
                    {
                        CAmount nDust = newTxOut.GetDustThreshold(::minRelayTxFee) - newTxOut.nValue;
                        newTxOut.nValue += nDust; // raise change until no more dust
                        for (unsigned int i = 0; i < vecSend.size(); i++) // subtract from first recipient
                        {
                            if (vecSend[i].fSubtractFeeFromAmount)
                            {
                                txNew.vout[i].nValue -= nDust;
                                if (txNew.vout[i].IsDust(::minRelayTxFee))
                                {
                                    strFailReason = _("The transaction amount is too small to send after the fee has been deducted");
                                    return false;
                                }
                                break;
                            }
                        }
                    }

                    // Never create dust outputs; if we would, just
                    // add the dust to the fee.
                    if (newTxOut.IsDust(::minRelayTxFee))
                    {
                        nFeeRet += nChange;
                        reservekey.ReturnKey();
                    }
                    else
                    {
                        nChangePosRet = txNew.vout.size() - 1; // dont change first or last
                        vector<CTxOut>::iterator position = txNew.vout.begin()+nChangePosRet;
                        txNew.vout.insert(position, newTxOut);
                    }
                } else reservekey.ReturnKey();

                // Fill vin
                //
                // Note how the sequence number is set to max()-1 so that the
                // nLockTime set above actually works.
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& coin, setCoins)
                    txNew.vin.push_back(CTxIn(coin.first->GetHash(),coin.second,CScript(),
                                              std::numeric_limits<unsigned int>::max()-1));

                // Check mempooltxinputlimit to avoid creating a transaction which the local mempool rejects
                size_t limit = (size_t)GetArg("-mempooltxinputlimit", 0);
                {
                    LOCK(cs_main);
                    if (NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_OVERWINTER)) {
                        limit = 0;
                    }
                }
                if (limit > 0) {
                    size_t n = txNew.vin.size();
                    if (n > limit) {
                        strFailReason = _(strprintf("Too many transparent inputs %zu > limit %zu", n, limit).c_str());
                        return false;
                    }
                }

                // Grab the current consensus branch ID
                auto consensusBranchId = CurrentEpochBranchId(chainActive.Height() + 1, Params().GetConsensus());

                // Sign
                int nIn = 0;
                CTransaction txNewConst(txNew);
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& coin, setCoins)
                {
                    bool signSuccess;
                    const CScript& scriptPubKey = coin.first->vout[coin.second].scriptPubKey;
                    SignatureData sigdata;
                    if (sign)
                        signSuccess = ProduceSignature(TransactionSignatureCreator(this, &txNewConst, nIn, coin.first->vout[coin.second].nValue, SIGHASH_ALL), scriptPubKey, sigdata, consensusBranchId);
                    else
                        signSuccess = ProduceSignature(DummySignatureCreator(this), scriptPubKey, sigdata, consensusBranchId);

                    if (!signSuccess)
                    {
                        strFailReason = _("Signing transaction failed");
                        return false;
                    } else {
                        UpdateTransaction(txNew, nIn, sigdata);
                    }

                    nIn++;
                }

                unsigned int nBytes = ::GetSerializeSize(txNew, SER_NETWORK, PROTOCOL_VERSION);

                // Remove scriptSigs if we used dummy signatures for fee calculation
                if (!sign) {
                    BOOST_FOREACH (CTxIn& vin, txNew.vin)
                        vin.scriptSig = CScript();
                }

                // Embed the constructed transaction data in wtxNew.
                *static_cast<CTransaction*>(&wtxNew) = CTransaction(txNew);

                // Limit size
                if (nBytes >= max_tx_size)
                {
                    strFailReason = _("Transaction too large");
                    return false;
                }

                dPriority = wtxNew.ComputePriority(dPriority, nBytes);

                // Can we complete this as a free transaction?
                if (fSendFreeTransactions && nBytes <= MAX_FREE_TRANSACTION_CREATE_SIZE)
                {
                    // Not enough fee: enough priority?
                    double dPriorityNeeded = mempool.estimatePriority(nTxConfirmTarget);
                    // Not enough mempool history to estimate: use hard-coded AllowFree.
                    if (dPriorityNeeded <= 0 && AllowFree(dPriority))
                        break;

                    // Small enough, and priority high enough, to send for free
                    if (dPriorityNeeded > 0 && dPriority >= dPriorityNeeded)
                        break;
                }

                CAmount nFeeNeeded = GetMinimumFee(nBytes, nTxConfirmTarget, mempool);
                if ( nFeeNeeded < 5000 )
                    nFeeNeeded = 5000;

                // If we made it here and we aren't even able to meet the relay fee on the next pass, give up
                // because we must be at the maximum allowed fee.
                if (nFeeNeeded < ::minRelayTxFee.GetFee(nBytes))
                {
                    strFailReason = _("Transaction too large for fee policy");
                    return false;
                }

                if (nFeeRet >= nFeeNeeded)
                    break; // Done, enough fee included.

                // Include more fee and try again.
                nFeeRet = nFeeNeeded;
                continue;
            }
        }
    }

    return true;
}

// Call after CreateTransaction unless you want to abort
bool CWallet::CommitTransaction(CWalletTx& wtxNew, CReserveKey& reservekey)
{
    {
        LOCK2(cs_main, cs_wallet);
        LogPrintf("CommitTransaction:\n%s", wtxNew.ToString());
        {
            // This is only to keep the database open to defeat the auto-flush for the
            // duration of this scope.  This is the only place where this optimization
            // maybe makes sense; please don't do it anywhere else.
            CWalletDB* pwalletdb = fFileBacked ? new CWalletDB(strWalletFile,"r+") : NULL;

            // Take key pair from key pool so it won't be used again
            reservekey.KeepKey();

            // Add tx to wallet, because if it has change it's also ours,
            // otherwise just for transaction history.
            AddToWallet(wtxNew, false, pwalletdb);

            // Notify that old coins are spent
            set<CWalletTx*> setCoins;
            BOOST_FOREACH(const CTxIn& txin, wtxNew.vin)
            {
                CWalletTx &coin = mapWallet[txin.prevout.hash];
                coin.BindWallet(this);
                NotifyTransactionChanged(this, coin.GetHash(), CT_UPDATED);
            }

            if (fFileBacked)
                delete pwalletdb;
        }

        std::string strCmd = GetArg("-txsend", "");

        if (fBroadcastTransactions)
        {
            // Broadcast
            if (!wtxNew.AcceptToMemoryPool(false))
            {
                fprintf(stderr,"commit failed\n");
                // This must not fail. The transaction has already been signed and recorded.
                LogPrintf("CommitTransaction(): Error: Transaction not valid\n");
                return false;
            }
            wtxNew.RelayWalletTransaction();
        }
           // If we are configured to send transactions via an
        // external service instead of broadcasting, do that
        else if (!strCmd.empty()) {
            boost::replace_all(strCmd, "%s", EncodeHexTx(wtxNew));
            boost::thread t(runCommand, strCmd); // thread runs free
        }
    }
    return true;
}

CAmount CWallet::GetMinimumFee(unsigned int nTxBytes, unsigned int nConfirmTarget, const CTxMemPool& pool)
{
    // payTxFee is user-set "I want to pay this much"
    CAmount nFeeNeeded = payTxFee.GetFee(nTxBytes);
    // user selected total at least (default=true)
    if (fPayAtLeastCustomFee && nFeeNeeded > 0 && nFeeNeeded < payTxFee.GetFeePerK())
        nFeeNeeded = payTxFee.GetFeePerK();
    // User didn't set: use -txconfirmtarget to estimate...
    if (nFeeNeeded == 0)
        nFeeNeeded = pool.estimateFee(nConfirmTarget).GetFee(nTxBytes);
    // ... unless we don't have enough mempool data, in which case fall
    // back to a hard-coded fee
    if (nFeeNeeded == 0)
        nFeeNeeded = minTxFee.GetFee(nTxBytes);
    // prevent user from paying a non-sense fee (like 1 satoshi): 0 < fee < minRelayFee
    if (nFeeNeeded < ::minRelayTxFee.GetFee(nTxBytes))
        nFeeNeeded = ::minRelayTxFee.GetFee(nTxBytes);
    // But always obey the maximum
    if (nFeeNeeded > maxTxFee)
        nFeeNeeded = maxTxFee;
    return nFeeNeeded;
}


void hush_prefetch(FILE *fp);

DBErrors CWallet::LoadWallet(bool& fFirstRunRet)
{
    if (!fFileBacked)
        return DB_LOAD_OK;
    fFirstRunRet = false;
    if ( 0 ) // doesnt help
    {
        fprintf(stderr,"loading wallet %s %u\n",strWalletFile.c_str(),(uint32_t)time(NULL));
        FILE *fp;
        if ( (fp= fopen(strWalletFile.c_str(),"rb")) != 0 )
        {
            hush_prefetch(fp);
            fclose(fp);
        }
    }
    //fprintf(stderr,"prefetched wallet %s %u\n",strWalletFile.c_str(),(uint32_t)time(NULL));
    DBErrors nLoadWalletRet = CWalletDB(strWalletFile,"cr+").LoadWallet(this);
    //fprintf(stderr,"loaded wallet %s %u\n",strWalletFile.c_str(),(uint32_t)time(NULL));
    if (nLoadWalletRet == DB_NEED_REWRITE)
    {
        if (CDB::Rewrite(strWalletFile, "\x04pool"))
        {
            LOCK(cs_wallet);
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // that requires a new key.
        }
    }

    if (nLoadWalletRet != DB_LOAD_OK)
        return nLoadWalletRet;
    fFirstRunRet = !vchDefaultKey.IsValid();

    uiInterface.LoadWallet(this);

    return DB_LOAD_OK;
}


DBErrors CWallet::ZapWalletTx(std::vector<CWalletTx>& vWtx)
{
    if (!fFileBacked)
        return DB_LOAD_OK;
    DBErrors nZapWalletTxRet = CWalletDB(strWalletFile,"cr+").ZapWalletTx(this, vWtx);
    if (nZapWalletTxRet == DB_NEED_REWRITE)
    {
        if (CDB::Rewrite(strWalletFile, "\x04pool"))
        {
            LOCK(cs_wallet);
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // that requires a new key.
        }
    }

    if (nZapWalletTxRet != DB_LOAD_OK)
        return nZapWalletTxRet;

    return DB_LOAD_OK;
}


bool CWallet::SetAddressBook(const CTxDestination& address, const string& strName, const string& strPurpose)
{
    bool fUpdated = false;
    {
        LOCK(cs_wallet); // mapAddressBook
        std::map<CTxDestination, CAddressBookData>::iterator mi = mapAddressBook.find(address);
        fUpdated = mi != mapAddressBook.end();
        mapAddressBook[address].name = strName;
        if (!strPurpose.empty()) /* update purpose only if requested */
            mapAddressBook[address].purpose = strPurpose;
    }
    NotifyAddressBookChanged(this, address, strName, ::IsMine(*this, address) != ISMINE_NO,
                             strPurpose, (fUpdated ? CT_UPDATED : CT_NEW) );
    if (!fFileBacked)
        return false;
    if (!strPurpose.empty() && !CWalletDB(strWalletFile).WritePurpose(EncodeDestination(address), strPurpose))
        return false;
    return CWalletDB(strWalletFile).WriteName(EncodeDestination(address), strName);
}

bool CWallet::DelAddressBook(const CTxDestination& address)
{
    {
        LOCK(cs_wallet); // mapAddressBook

        if(fFileBacked)
        {
            // Delete destdata tuples associated with address
            std::string strAddress = EncodeDestination(address);
            BOOST_FOREACH(const PAIRTYPE(string, string) &item, mapAddressBook[address].destdata)
            {
                CWalletDB(strWalletFile).EraseDestData(strAddress, item.first);
            }
        }
        mapAddressBook.erase(address);
    }

    NotifyAddressBookChanged(this, address, "", ::IsMine(*this, address) != ISMINE_NO, "", CT_DELETED);

    if (!fFileBacked)
        return false;
    CWalletDB(strWalletFile).ErasePurpose(EncodeDestination(address));
    return CWalletDB(strWalletFile).EraseName(EncodeDestination(address));
}

bool CWallet::SetDefaultKey(const CPubKey &vchPubKey)
{
    if (fFileBacked)
    {
        if (!CWalletDB(strWalletFile).WriteDefaultKey(vchPubKey))
            return false;
    }
    vchDefaultKey = vchPubKey;
    return true;
}

/**
 * Mark old keypool keys as used,
 * and generate all new keys
 */
bool CWallet::NewKeyPool()
{
    {
        LOCK(cs_wallet);
        CWalletDB walletdb(strWalletFile);
        BOOST_FOREACH(int64_t nIndex, setKeyPool)
            walletdb.ErasePool(nIndex);
        setKeyPool.clear();

        if (IsLocked())
            return false;

        int64_t nKeys = max(GetArg("-keypool", 100), (int64_t)0);
        for (int i = 0; i < nKeys; i++)
        {
            int64_t nIndex = i+1;
            walletdb.WritePool(nIndex, CKeyPool(GenerateNewKey()));
            setKeyPool.insert(nIndex);
        }
        LogPrintf("CWallet::NewKeyPool wrote %d new keys\n", nKeys);
    }
    return true;
}

bool CWallet::TopUpKeyPool(unsigned int kpSize)
{
    {
        LOCK(cs_wallet);

        if (IsLocked())
            return false;

        CWalletDB walletdb(strWalletFile);

        // Top up key pool
        unsigned int nTargetSize;
        if (kpSize > 0)
            nTargetSize = kpSize;
        else
            nTargetSize = max(GetArg("-keypool", 100), (int64_t) 0);

        while (setKeyPool.size() < (nTargetSize + 1))
        {
            int64_t nEnd = 1;
            if (!setKeyPool.empty())
                nEnd = *(--setKeyPool.end()) + 1;
            if (!walletdb.WritePool(nEnd, CKeyPool(GenerateNewKey())))
                throw runtime_error("TopUpKeyPool(): writing generated key failed");
            setKeyPool.insert(nEnd);
            LogPrintf("keypool added key %d, size=%u\n", nEnd, setKeyPool.size());
        }
    }
    return true;
}

void CWallet::ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool)
{
    nIndex = -1;
    keypool.vchPubKey = CPubKey();
    {
        LOCK(cs_wallet);

        if (!IsLocked())
            TopUpKeyPool();

        // Get the oldest key
        if(setKeyPool.empty())
            return;

        CWalletDB walletdb(strWalletFile);

        nIndex = *(setKeyPool.begin());
        setKeyPool.erase(setKeyPool.begin());
        if (!walletdb.ReadPool(nIndex, keypool))
            throw runtime_error("ReserveKeyFromKeyPool(): read failed");
        if (!HaveKey(keypool.vchPubKey.GetID()))
            throw runtime_error("ReserveKeyFromKeyPool(): unknown key in key pool");
        assert(keypool.vchPubKey.IsValid());
        //LogPrintf("keypool reserve %d\n", nIndex);
    }
}

void CWallet::KeepKey(int64_t nIndex)
{
    // Remove from key pool
    if (fFileBacked)
    {
        CWalletDB walletdb(strWalletFile);
        walletdb.ErasePool(nIndex);
    }
    LogPrintf("keypool keep %d\n", nIndex);
}

void CWallet::ReturnKey(int64_t nIndex)
{
    // Return to key pool
    {
        LOCK(cs_wallet);
        setKeyPool.insert(nIndex);
    }
    //LogPrintf("keypool return %d\n", nIndex);
}

bool CWallet::GetKeyFromPool(CPubKey& result)
{
    int64_t nIndex = 0;
    CKeyPool keypool;
    {
        LOCK(cs_wallet);
        ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex == -1)
        {
            if (IsLocked()) return false;
            result = GenerateNewKey();
            return true;
        }
        KeepKey(nIndex);
        result = keypool.vchPubKey;
    }
    return true;
}

int64_t CWallet::GetOldestKeyPoolTime()
{
    int64_t nIndex = 0;
    CKeyPool keypool;
    ReserveKeyFromKeyPool(nIndex, keypool);
    if (nIndex == -1)
        return GetTime();
    ReturnKey(nIndex);
    return keypool.nTime;
}

std::map<CTxDestination, CAmount> CWallet::GetAddressBalances()
{
    map<CTxDestination, CAmount> balances;

    {
        LOCK(cs_wallet);
        BOOST_FOREACH(PAIRTYPE(uint256, CWalletTx) walletEntry, mapWallet)
        {
            CWalletTx *pcoin = &walletEntry.second;

            if (!CheckFinalTx(*pcoin) || !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? 0 : 1))
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            {
                CTxDestination addr;
                if (!IsMine(pcoin->vout[i]))
                    continue;
                if(!ExtractDestination(pcoin->vout[i].scriptPubKey, addr))
                    continue;

                CAmount n = IsSpent(walletEntry.first, i) ? 0 : pcoin->vout[i].nValue;

                if (!balances.count(addr))
                    balances[addr] = 0;
                balances[addr] += n;
            }
        }
    }

    return balances;
}

set< set<CTxDestination> > CWallet::GetAddressGroupings()
{
    AssertLockHeld(cs_wallet); // mapWallet
    set< set<CTxDestination> > groupings;
    set<CTxDestination> grouping;

    BOOST_FOREACH(PAIRTYPE(uint256, CWalletTx) walletEntry, mapWallet)
    {
        CWalletTx *pcoin = &walletEntry.second;

        if (pcoin->vin.size() > 0)
        {
            bool any_mine = false;
            // group all input addresses with each other
            BOOST_FOREACH(CTxIn txin, pcoin->vin)
            {
                CTxDestination address;
                if(!IsMine(txin)) /* If this input isn't mine, ignore it */
                    continue;
                if(!ExtractDestination(mapWallet[txin.prevout.hash].vout[txin.prevout.n].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                any_mine = true;
            }

            // group change with input addresses
            if (any_mine)
            {
               BOOST_FOREACH(CTxOut txout, pcoin->vout)
                   if (IsChange(txout))
                   {
                       CTxDestination txoutAddr;
                       if(!ExtractDestination(txout.scriptPubKey, txoutAddr))
                           continue;
                       grouping.insert(txoutAddr);
                   }
            }
            if (grouping.size() > 0)
            {
                groupings.insert(grouping);
                grouping.clear();
            }
        }

        // group lone addrs by themselves
        for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            if (IsMine(pcoin->vout[i]))
            {
                CTxDestination address;
                if(!ExtractDestination(pcoin->vout[i].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                groupings.insert(grouping);
                grouping.clear();
            }
    }

    set< set<CTxDestination>* > uniqueGroupings; // a set of pointers to groups of addresses
    map< CTxDestination, set<CTxDestination>* > setmap;  // map addresses to the unique group containing it
    BOOST_FOREACH(set<CTxDestination> grouping, groupings)
    {
        // make a set of all the groups hit by this new group
        set< set<CTxDestination>* > hits;
        map< CTxDestination, set<CTxDestination>* >::iterator it;
        BOOST_FOREACH(CTxDestination address, grouping)
            if ((it = setmap.find(address)) != setmap.end())
                hits.insert((*it).second);

        // merge all hit groups into a new single group and delete old groups
        set<CTxDestination>* merged = new set<CTxDestination>(grouping);
        BOOST_FOREACH(set<CTxDestination>* hit, hits)
        {
            merged->insert(hit->begin(), hit->end());
            uniqueGroupings.erase(hit);
            delete hit;
        }
        uniqueGroupings.insert(merged);

        // update setmap
        BOOST_FOREACH(CTxDestination element, *merged)
            setmap[element] = merged;
    }

    set< set<CTxDestination> > ret;
    BOOST_FOREACH(set<CTxDestination>* uniqueGrouping, uniqueGroupings)
    {
        ret.insert(*uniqueGrouping);
        delete uniqueGrouping;
    }

    return ret;
}

std::set<CTxDestination> CWallet::GetAccountAddresses(const std::string& strAccount) const
{
    LOCK(cs_wallet);
    set<CTxDestination> result;
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, CAddressBookData)& item, mapAddressBook)
    {
        const CTxDestination& address = item.first;
        const string& strName = item.second.name;
        if (strName == strAccount)
            result.insert(address);
    }
    return result;
}

bool CReserveKey::GetReservedKey(CPubKey& pubkey)
{
    if (nIndex == -1)
    {
        CKeyPool keypool;
        pwallet->ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex != -1)
            vchPubKey = keypool.vchPubKey;
        else {
            return false;
        }
    }
    assert(vchPubKey.IsValid());
    pubkey = vchPubKey;
    return true;
}

void CReserveKey::KeepKey()
{
    if (nIndex != -1)
        pwallet->KeepKey(nIndex);
    nIndex = -1;
    vchPubKey = CPubKey();
}

void CReserveKey::ReturnKey()
{
    if (nIndex != -1)
        pwallet->ReturnKey(nIndex);
    nIndex = -1;
    vchPubKey = CPubKey();
}

void CWallet::GetAllReserveKeys(set<CKeyID>& setAddress) const
{
    setAddress.clear();

    CWalletDB walletdb(strWalletFile);

    LOCK2(cs_main, cs_wallet);
    BOOST_FOREACH(const int64_t& id, setKeyPool)
    {
        CKeyPool keypool;
        if (!walletdb.ReadPool(id, keypool))
            throw runtime_error("GetAllReserveKeyHashes(): read failed");
        assert(keypool.vchPubKey.IsValid());
        CKeyID keyID = keypool.vchPubKey.GetID();
        if (!HaveKey(keyID))
            throw runtime_error("GetAllReserveKeyHashes(): unknown key in key pool");
        setAddress.insert(keyID);
    }
}

void CWallet::UpdatedTransaction(const uint256 &hashTx)
{
    {
        LOCK(cs_wallet);
        // Only notify UI if this transaction is in this wallet
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(hashTx);
        if (mi != mapWallet.end())
            NotifyTransactionChanged(this, hashTx, CT_UPDATED);
    }
}

void CWallet::LockCoin(COutPoint& output)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.insert(output);
}

void CWallet::UnlockCoin(COutPoint& output)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.erase(output);
}

void CWallet::UnlockAllCoins()
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.clear();
}

bool CWallet::IsLockedCoin(uint256 hash, unsigned int n) const
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    COutPoint outpt(hash, n);

    return (setLockedCoins.count(outpt) > 0);
}

void CWallet::ListLockedCoins(std::vector<COutPoint>& vOutpts)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    for (std::set<COutPoint>::iterator it = setLockedCoins.begin();
         it != setLockedCoins.end(); it++) {
        COutPoint outpt = (*it);
        vOutpts.push_back(outpt);
    }
}


// Note Locking Operations

void CWallet::LockNote(const SaplingOutPoint& output)
{
    AssertLockHeld(cs_wallet);
    setLockedSaplingNotes.insert(output);
}

void CWallet::UnlockNote(const SaplingOutPoint& output)
{
    AssertLockHeld(cs_wallet);
    setLockedSaplingNotes.erase(output);
}

void CWallet::UnlockAllSaplingNotes()
{
    AssertLockHeld(cs_wallet);
    setLockedSaplingNotes.clear();
}

bool CWallet::IsLockedNote(const SaplingOutPoint& output) const
{
    AssertLockHeld(cs_wallet);
    return (setLockedSaplingNotes.count(output) > 0);
}

std::vector<SaplingOutPoint> CWallet::ListLockedSaplingNotes()
{
    AssertLockHeld(cs_wallet);
    std::vector<SaplingOutPoint> vOutputs(setLockedSaplingNotes.begin(), setLockedSaplingNotes.end());
    return vOutputs;
}

/** @} */ // end of Actions

class CAffectedKeysVisitor : public boost::static_visitor<void> {
private:
    const CKeyStore &keystore;
    std::vector<CKeyID> &vKeys;

public:
    CAffectedKeysVisitor(const CKeyStore &keystoreIn, std::vector<CKeyID> &vKeysIn) : keystore(keystoreIn), vKeys(vKeysIn) {}

    void Process(const CScript &script) {
        txnouttype type;
        std::vector<CTxDestination> vDest;
        int nRequired;
        if (ExtractDestinations(script, type, vDest, nRequired)) {
            BOOST_FOREACH(const CTxDestination &dest, vDest)
                boost::apply_visitor(*this, dest);
        }
    }

    void operator()(const CKeyID &keyId) {
        if (keystore.HaveKey(keyId))
            vKeys.push_back(keyId);
    }

    void operator()(const CPubKey &key) {
        CKeyID keyId = key.GetID();
        if (keystore.HaveKey(keyId))
            vKeys.push_back(keyId);
    }

    void operator()(const CScriptID &scriptId) {
        CScript script;
        if (keystore.GetCScript(scriptId, script))
            Process(script);
    }

    void operator()(const CNoDestination &none) {}
};

void CWallet::GetKeyBirthTimes(std::map<CKeyID, int64_t> &mapKeyBirth) const {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    mapKeyBirth.clear();

    // get birth times for keys with metadata
    for (std::map<CKeyID, CKeyMetadata>::const_iterator it = mapKeyMetadata.begin(); it != mapKeyMetadata.end(); it++)
        if (it->second.nCreateTime)
            mapKeyBirth[it->first] = it->second.nCreateTime;

    // map in which we'll infer heights of other keys
    CBlockIndex *pindexMax = chainActive[std::max(0, chainActive.Height() - 144)]; // the tip can be reorganised; use a 144-block safety margin
    std::map<CKeyID, CBlockIndex*> mapKeyFirstBlock;
    std::set<CKeyID> setKeys;
    GetKeys(setKeys);
    BOOST_FOREACH(const CKeyID &keyid, setKeys) {
        if (mapKeyBirth.count(keyid) == 0)
            mapKeyFirstBlock[keyid] = pindexMax;
    }
    setKeys.clear();

    // if there are no such keys, we're done
    if (mapKeyFirstBlock.empty())
        return;

    // find first block that affects those keys, if there are any left
    std::vector<CKeyID> vAffected;
    for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); it++) {
        // iterate over all wallet transactions...
        const CWalletTx &wtx = (*it).second;
        BlockMap::const_iterator blit = mapBlockIndex.find(wtx.hashBlock);
        if (blit != mapBlockIndex.end() && chainActive.Contains(blit->second)) {
            // ... which are already in a block
            int nHeight = blit->second->GetHeight();
            BOOST_FOREACH(const CTxOut &txout, wtx.vout) {
                // iterate over all their outputs
                CAffectedKeysVisitor(*this, vAffected).Process(txout.scriptPubKey);
                BOOST_FOREACH(const CKeyID &keyid, vAffected) {
                    // ... and all their affected keys
                    std::map<CKeyID, CBlockIndex*>::iterator rit = mapKeyFirstBlock.find(keyid);
                    if (rit != mapKeyFirstBlock.end() && nHeight < rit->second->GetHeight())
                        rit->second = blit->second;
                }
                vAffected.clear();
            }
        }
    }

    // Extract block timestamps for those keys
    for (std::map<CKeyID, CBlockIndex*>::const_iterator it = mapKeyFirstBlock.begin(); it != mapKeyFirstBlock.end(); it++)
        mapKeyBirth[it->first] = it->second->GetBlockTime() - 7200; // block times can be 2h off
}

bool CWallet::AddDestData(const CTxDestination &dest, const std::string &key, const std::string &value)
{
    if (boost::get<CNoDestination>(&dest))
        return false;

    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteDestData(EncodeDestination(dest), key, value);
}

bool CWallet::EraseDestData(const CTxDestination &dest, const std::string &key)
{
    if (!mapAddressBook[dest].destdata.erase(key))
        return false;
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).EraseDestData(EncodeDestination(dest), key);
}

bool CWallet::LoadDestData(const CTxDestination &dest, const std::string &key, const std::string &value)
{
    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    return true;
}

bool CWallet::GetDestData(const CTxDestination &dest, const std::string &key, std::string *value) const
{
    std::map<CTxDestination, CAddressBookData>::const_iterator i = mapAddressBook.find(dest);
    if(i != mapAddressBook.end())
    {
        CAddressBookData::StringMap::const_iterator j = i->second.destdata.find(key);
        if(j != i->second.destdata.end())
        {
            if(value)
                *value = j->second;
            return true;
        }
    }
    return false;
}

CKeyPool::CKeyPool()
{
    nTime = GetTime();
}

CKeyPool::CKeyPool(const CPubKey& vchPubKeyIn)
{
    nTime = GetTime();
    vchPubKey = vchPubKeyIn;
}

CWalletKey::CWalletKey(int64_t nExpires)
{
    nTimeCreated = (nExpires ? GetTime() : 0);
    nTimeExpires = nExpires;
}

void CMerkleTx::SetMerkleBranch(const CBlock& block)
{
    CBlock blockTmp;

    // Update the tx's hashBlock
    hashBlock = block.GetHash();

    // Locate the transaction
    for (nIndex = 0; nIndex < (int)block.vtx.size(); nIndex++)
        if (block.vtx[nIndex] == *(CTransaction*)this)
            break;
    if (nIndex == (int)block.vtx.size())
    {
        vMerkleBranch.clear();
        nIndex = -1;
        LogPrintf("ERROR: SetMerkleBranch(): couldn't find tx in block\n");
    }

    // Fill in merkle branch
    vMerkleBranch = block.GetMerkleBranch(nIndex);
}

int CMerkleTx::GetDepthInMainChainINTERNAL(const CBlockIndex* &pindexRet) const
{
    if (hashBlock.IsNull() || nIndex == -1)
        return 0;
    AssertLockHeld(cs_main);

    // Find the block it claims to be in
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !chainActive.Contains(pindex))
        return 0;

    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified)
    {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;
        fMerkleVerified = true;
    }

    pindexRet = pindex;
    return chainActive.Height() - pindex->GetHeight() + 1;
}

int CMerkleTx::GetDepthInMainChain(const CBlockIndex* &pindexRet) const
{
    AssertLockHeld(cs_main);
    int nResult = GetDepthInMainChainINTERNAL(pindexRet);
    if (nResult == 0 && !mempool.exists(GetHash()))
        return -1; // Not in chain, not in mempool

    return nResult;
}

int CMerkleTx::GetBlocksToMaturity() const
{
    if ( SMART_CHAIN_SYMBOL[0] == 0 )
        COINBASE_MATURITY = _COINBASE_MATURITY;
    if (!IsCoinBase())
        return 0;
    int32_t depth = GetDepthInMainChain();
    int32_t ut = UnlockTime(0);
    int32_t toMaturity = (ut - chainActive.Height()) < 0 ? 0 : ut - chainActive.Height();
    //printf("depth.%i, unlockTime.%i, toMaturity.%i\n", depth, ut, toMaturity);
    ut = (COINBASE_MATURITY - depth) < 0 ? 0 : COINBASE_MATURITY - depth;
    return(ut < toMaturity ? toMaturity : ut);
}

bool CMerkleTx::AcceptToMemoryPool(bool fLimitFree, bool fRejectAbsurdFee)
{
    CValidationState state;
    return ::AcceptToMemoryPool(mempool, state, *this, fLimitFree, NULL, fRejectAbsurdFee);
}

/**
 * Find notes in the wallet filtered by payment address, min depth and ability to spend.
 * These notes are decrypted and added to the output parameter vector, outEntries.
 */
void CWallet::GetFilteredNotes(
    std::vector<SaplingNoteEntry>& saplingEntries,
    std::string address,
    int minDepth,
    bool ignoreSpent,
    bool requireSpendingKey)
{
    std::set<PaymentAddress> filterAddresses;

    if (address.length() > 0) {
        filterAddresses.insert(DecodePaymentAddress(address));
    }

    GetFilteredNotes(saplingEntries, filterAddresses, minDepth, INT_MAX, ignoreSpent, requireSpendingKey);
}

/**
 * Find notes in the wallet filtered by payment addresses, min depth, max depth,
 * if the note is spent, if a spending key is required, and if the notes are locked.
 * These notes are decrypted and added to the output parameter vector, outEntries.
 */
void CWallet::GetFilteredNotes(
    std::vector<SaplingNoteEntry>& saplingEntries,
    std::set<PaymentAddress>& filterAddresses,
    int minDepth,
    int maxDepth,
    bool ignoreSpent,
    bool requireSpendingKey,
    bool ignoreLocked)
{
    LOCK2(cs_main, cs_wallet);

    for (auto & p : mapWallet) {
        CWalletTx wtx = p.second;

        // Filter the transactions before checking for notes
        if (!CheckFinalTx(wtx) || wtx.GetBlocksToMaturity() > 0)
            continue;

        if (minDepth > 1) {
            int nHeight    = tx_height(wtx.GetHash());
            if ( nHeight == 0 ) {
                continue;
            }
            int nDepth     = wtx.GetDepthInMainChain();
            int dpowconfs  = hush_dpowconfs(nHeight,nDepth);
            if ( dpowconfs < minDepth || dpowconfs > maxDepth) {
                continue;
            }
        } else {
            if ( wtx.GetDepthInMainChain() < minDepth ||
                wtx.GetDepthInMainChain() > maxDepth) {
                continue;
            }
        }

        for (auto & pair : wtx.mapSaplingNoteData) {
            SaplingOutPoint op = pair.first;
            SaplingNoteData nd = pair.second;

            auto maybe_pt = SaplingNotePlaintext::decrypt(
                wtx.vShieldedOutput[op.n].encCiphertext,
                nd.ivk,
                wtx.vShieldedOutput[op.n].ephemeralKey,
                wtx.vShieldedOutput[op.n].cm);
            assert(static_cast<bool>(maybe_pt));
            auto notePt = maybe_pt.get();

            auto maybe_pa = nd.ivk.address(notePt.d);
            assert(static_cast<bool>(maybe_pa));
            auto pa = maybe_pa.get();

            // skip notes which belong to a different payment address in the wallet
            if (!(filterAddresses.empty() || filterAddresses.count(pa))) {
                continue;
            }

            if (ignoreSpent && nd.nullifier && IsSaplingSpent(*nd.nullifier)) {
                continue;
            }

            // skip notes which cannot be spent
            if (requireSpendingKey) {
                libzcash::SaplingIncomingViewingKey ivk;
                libzcash::SaplingFullViewingKey fvk;
                if (!(GetSaplingIncomingViewingKey(pa, ivk) &&
                    GetSaplingFullViewingKey(ivk, fvk) &&
                    HaveSaplingSpendingKey(fvk))) {
                    continue;
                }
            }

            // skip locked notes
            // TODO: Add locking for Sapling notes -> done
             if (ignoreLocked && IsLockedNote(op)) {
                 continue;
             }

            auto note = notePt.note(nd.ivk).get();
            saplingEntries.push_back(SaplingNoteEntry {
                op, pa, note, notePt.memo(), wtx.GetDepthInMainChain() });
        }
    }
}


//
// Shielded key and address generalizations
//

bool IncomingViewingKeyBelongsToWallet::operator()(const libzcash::SaplingPaymentAddress &zaddr) const
{
    libzcash::SaplingIncomingViewingKey ivk;
    return m_wallet->GetSaplingIncomingViewingKey(zaddr, ivk);
}

bool IncomingViewingKeyBelongsToWallet::operator()(const libzcash::InvalidEncoding& no) const
{
    return false;
}

bool PaymentAddressBelongsToWallet::operator()(const libzcash::SaplingPaymentAddress &zaddr) const
{
    libzcash::SaplingIncomingViewingKey ivk;

    // If we have a SaplingExtendedSpendingKey in the wallet, then we will
    // also have the corresponding SaplingFullViewingKey.
    return m_wallet->GetSaplingIncomingViewingKey(zaddr, ivk) &&
        m_wallet->HaveSaplingFullViewingKey(ivk);
}

bool PaymentAddressBelongsToWallet::operator()(const libzcash::InvalidEncoding& no) const
{
    return false;
}

bool HaveSpendingKeyForPaymentAddress::operator()(const libzcash::SaplingPaymentAddress &zaddr) const
{
    libzcash::SaplingIncomingViewingKey ivk;
    libzcash::SaplingFullViewingKey fvk;

    return m_wallet->GetSaplingIncomingViewingKey(zaddr, ivk) &&
        m_wallet->GetSaplingFullViewingKey(ivk, fvk) &&
        m_wallet->HaveSaplingSpendingKey(fvk);
}

bool HaveSpendingKeyForPaymentAddress::operator()(const libzcash::InvalidEncoding& no) const
{
    return false;
}

boost::optional<libzcash::SpendingKey> GetSpendingKeyForPaymentAddress::operator()(
    const libzcash::SaplingPaymentAddress &zaddr) const
{
    libzcash::SaplingExtendedSpendingKey extsk;
    if (m_wallet->GetSaplingExtendedSpendingKey(zaddr, extsk)) {
        return libzcash::SpendingKey(extsk);
    } else {
        return boost::none;
    }
}

boost::optional<libzcash::SpendingKey> GetSpendingKeyForPaymentAddress::operator()(
    const libzcash::InvalidEncoding& no) const
{
    // Defaults to InvalidEncoding
    return libzcash::SpendingKey();
}


SpendingKeyAddResult AddSpendingKeyToWallet::operator()(const libzcash::SaplingExtendedSpendingKey &sk) const {
    auto fvk = sk.expsk.full_viewing_key();
    auto ivk = fvk.in_viewing_key();
    auto addr = sk.DefaultAddress();
    {
        if (log){
            LogPrint("zrpc", "Importing zaddr %s...\n", EncodePaymentAddress(addr));
        }
        // Don't throw error in case a key is already there
        if (m_wallet->HaveSaplingSpendingKey(fvk)) {
            return KeyAlreadyExists;
        } else {
            if (!m_wallet-> AddSaplingZKey(sk, addr)) {
                return KeyNotAdded;
            }

            // Sapling addresses can't have been used in transactions prior to activation.
            if (params.vUpgrades[Consensus::UPGRADE_SAPLING].nActivationHeight == Consensus::NetworkUpgrade::ALWAYS_ACTIVE) {
                m_wallet->mapSaplingZKeyMetadata[ivk].nCreateTime = nTime;
            } else {
                // 154051200 seconds from epoch is Friday, 26 October 2018 00:00:00 GMT - definitely before Sapling activates
                m_wallet->mapSaplingZKeyMetadata[ivk].nCreateTime = std::max((int64_t) 154051200, nTime);
            }
            if (hdKeypath) {
                m_wallet->mapSaplingZKeyMetadata[ivk].hdKeypath = hdKeypath.get();
            }
            if (seedFpStr) {
                uint256 seedFp;
                seedFp.SetHex(seedFpStr.get());
                m_wallet->mapSaplingZKeyMetadata[ivk].seedFp = seedFp;
            }
            return KeyAdded;
        }
    }
}

SpendingKeyAddResult AddSpendingKeyToWallet::operator()(const libzcash::InvalidEncoding& no) const {
    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid spending key");
}
