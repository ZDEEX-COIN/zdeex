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
#include "cc/eval.h"
#include "crosschain.h"
#include "importcoin.h"
#include "main.h"
#include "notarizationdb.h"
#include "merkleblock.h"
#include "cc/CCinclude.h"
/*
 * The crosschain workflow.
 *
 * 3 chains, A, B, and HUSH. We would like to prove TX on B.
 * There is a notarization, nA0, which will include TX via an MoM.
 * The notarization nA0 must fall between 2 notarizations of B,
 * ie, nB0 and nB1. An MoMoM including this range is propagated to
 * B in notarization receipt (backnotarization) bnB2.
 *
 * A:                 TX   bnA0
 *                     \   /
 * HUSH:      nB0        nA0     nB1      nB2
 *              \                 \       \
 * B:          bnB0              bnB1     bnB2
 */

// XXX: There are potential crashes wherever we access chainActive without a lock,
// because it might be disconnecting blocks at the same time.
// TODO: this assumes a blocktime of 75 seconds for HUSH and 60 seconds for other chains
int NOTARISATION_SCAN_LIMIT_BLOCKS = strncmp(SMART_CHAIN_SYMBOL, "HUSH3",5) == 0 ? 1152 : 1440;
CBlockIndex *hush_getblockindex(uint256 hash);

/* On HUSH */
uint256 CalculateProofRoot(const char* symbol, uint32_t targetCCid, int hushHeight,
        std::vector<uint256> &moms, uint256 &destNotarizationTxid)
{
    /*
     * Notaries don't wait for confirmation on HUSH before performing a backnotarization,
     * but we need a determinable range that will encompass all merkle roots. Include MoMs
     * including the block height of the last notarization until the height before the
     * previous notarization.
     *
     *    hushHeight      notarizations-0      notarizations-1
     *                         *********************|
     *        > scan backwards >
     */

    if (targetCCid < 2)
        return uint256();

    if (hushHeight < 0 || hushHeight > chainActive.Height())
        return uint256();

    int seenOwnNotarizations = 0, i = 0;

    int authority = GetSymbolAuthority(symbol);
    std::set<uint256> tmp_moms;

    for (i=0; i<NOTARISATION_SCAN_LIMIT_BLOCKS; i++) {
        if (i > hushHeight) break;
        NotarizationsInBlock notarizations;
        uint256 blockHash = *chainActive[hushHeight-i]->phashBlock;
        if (!GetBlockNotarizations(blockHash, notarizations))
            continue;

        // See if we have an own notarization in this block
        BOOST_FOREACH(Notarization& nota, notarizations) {
            if (strcmp(nota.second.symbol, symbol) == 0)
            {
                seenOwnNotarizations++;
                if (seenOwnNotarizations == 1)
                    destNotarizationTxid = nota.first;
                else if (seenOwnNotarizations == 7)
                    goto end;
                //break;
            }
        }

        if (seenOwnNotarizations >= 1) {
            BOOST_FOREACH(Notarization& nota, notarizations) {
                if (GetSymbolAuthority(nota.second.symbol) == authority)
                    if (nota.second.ccId == targetCCid) {
                      tmp_moms.insert(nota.second.MoM);
                      //fprintf(stderr, "added mom: %s\n",nota.second.MoM.GetHex().data());
                    }
            }
        }
    }

    // Not enough own notarizations found to return determinate MoMoM
    destNotarizationTxid = uint256();
    moms.clear();
    return uint256();

end:
    // add set to vector. Set makes sure there are no dupes included. 
    moms.clear();
    std::copy(tmp_moms.begin(), tmp_moms.end(), std::back_inserter(moms));
    //fprintf(stderr, "SeenOwnNotarizations.%i moms.size.%li blocks scanned.%i\n",seenOwnNotarizations, moms.size(), i);
    return GetMerkleRoot(moms);
}


/*
 * Get a notarization from a given height
 *
 * Will scan notarizations leveldb up to a limit
 */
template <typename IsTarget>
int ScanNotarizationsFromHeight(int nHeight, const IsTarget f, Notarization &found)
{
    int limit = std::min(nHeight + NOTARISATION_SCAN_LIMIT_BLOCKS, chainActive.Height());
    int start = std::max(nHeight, 1);

    for (int h=start; h<limit; h++) {
        NotarizationsInBlock notarizations;

        if (!GetBlockNotarizations(*chainActive[h]->phashBlock, notarizations))
            continue;

        BOOST_FOREACH(found, notarizations) {
            if (f(found)) {
                return h;
            }
        }
    }
    return 0;
}


/* On HUSH */
TxProof GetCrossChainProof(const uint256 txid, const char* targetSymbol, uint32_t targetCCid,
        const TxProof assetChainProof, int32_t offset)
{
    /*
     * Here we are given a proof generated by an assetchain A which goes from given txid to
     * an assetchain MoM. We need to go from the notarizationTxid for A to the MoMoM range of the
     * backnotarization for B (given by hushHeight of notarization), find the MoM within the MoMs for
     * that range, and finally extend the proof to lead to the MoMoM (proof root).
     */
    EvalRef eval;
    uint256 MoM = assetChainProof.second.Exec(txid);

    // Get a Hush height for given notarization Txid
    int hushHeight;
    {
        CTransaction sourceNotarization;
        uint256 hashBlock;
        CBlockIndex blockIdx;
        if (!eval->GetTxConfirmed(assetChainProof.first, sourceNotarization, blockIdx))
            throw std::runtime_error("Notarization not found");
        hushHeight = blockIdx.GetHeight();
    }

    // We now have a hushHeight of the notarization from chain A. So we know that a MoM exists
    // at that height.
    // If we call CalculateProofRoot with that height, it'll scan backwards, until it finds
    // a notarization from B, and it might not include our notarization from A
    // at all. So, the thing we need to do is scan forwards to find the notarization for B,
    // that is inclusive of A.
    Notarization nota;
    auto isTarget = [&](Notarization &nota) {
        return strcmp(nota.second.symbol, targetSymbol) == 0;
    };
    hushHeight = ScanNotarizationsFromHeight(hushHeight, isTarget, nota);
    if (!hushHeight)
        throw std::runtime_error("Cannot find notarization for target inclusive of source");
        
    if ( offset != 0 )
        hushHeight += offset;

    // Get MoMs for Hush height and symbol
    std::vector<uint256> moms;
    uint256 targetChainNotarizationTxid;
    uint256 MoMoM = CalculateProofRoot(targetSymbol, targetCCid, hushHeight, moms, targetChainNotarizationTxid);
    if (MoMoM.IsNull())
        throw std::runtime_error("No MoMs found");

    // Find index of source MoM in MoMoM
    int nIndex;
    for (nIndex=0; nIndex<moms.size(); nIndex++) {
        if (moms[nIndex] == MoM)
            goto cont;
    }
    throw std::runtime_error("Couldn't find MoM within MoMoM set");
cont:

    // Create a branch
    std::vector<uint256> vBranch;
    {
        CBlock fakeBlock;
        for (int i=0; i<moms.size(); i++) {
            CTransaction fakeTx;
            // first value in CTransaction memory is it's hash
            memcpy((void*)&fakeTx, moms[i].begin(), 32);
            fakeBlock.vtx.push_back(fakeTx);
        }
        vBranch = fakeBlock.GetMerkleBranch(nIndex);
    }

    // Concatenate branches
    MerkleBranch newBranch = assetChainProof.second;
    newBranch << MerkleBranch(nIndex, vBranch);

    // Check proof
    if (newBranch.Exec(txid) != MoMoM)
        throw std::runtime_error("Proof check failed");

    return std::make_pair(targetChainNotarizationTxid,newBranch);
}


/*
 * Takes an importTx that has proof leading to assetchain root
 * and extends proof to cross chain root
 */
void CompleteImportTransaction(CTransaction &importTx, int32_t offset)
{
    ImportProof proof; CTransaction burnTx; std::vector<CTxOut> payouts; std::vector<uint8_t> rawproof;
    if (!UnmarshalImportTx(importTx, proof, burnTx, payouts))
        throw std::runtime_error("Couldn't unmarshal importTx");

    std::string targetSymbol;
    uint32_t targetCCid;
    uint256 payoutsHash;
    if (!UnmarshalBurnTx(burnTx, targetSymbol, &targetCCid, payoutsHash, rawproof))
        throw std::runtime_error("Couldn't unmarshal burnTx");

    TxProof merkleBranch;
    if( !proof.IsMerkleBranch(merkleBranch) )
        throw std::runtime_error("Incorrect import tx proof");
    TxProof newMerkleBranch = GetCrossChainProof(burnTx.GetHash(), targetSymbol.data(), targetCCid, merkleBranch, offset);
    ImportProof newProof(newMerkleBranch);

    importTx = MakeImportCoinTransaction(newProof, burnTx, payouts);
}

bool IsSameAssetChain(const Notarization &nota) {
    return strcmp(nota.second.symbol, SMART_CHAIN_SYMBOL) == 0;
};

/* On assetchain */
bool GetNextBacknotarization(uint256 hushNotarizationTxid, Notarization &out)
{
    /*
     * Here we are given a txid, and a proof.
     * We go from the HUSH notarization txid to the backnotarization,
     * then jump to the next backnotarization, which contains the corresponding MoMoM.
     */
    Notarization bn;
    if (!GetBackNotarization(hushNotarizationTxid, bn))
        return false;

    // Need to get block height of that backnotarization
    EvalRef eval;
    CBlockIndex block;
    CTransaction tx;
    if (!eval->GetTxConfirmed(bn.first, tx, block)){
        fprintf(stderr, "Can't get height of backnotarization, this should not happen\n");
        return false;
    }

    return (bool) ScanNotarizationsFromHeight(block.GetHeight()+1, &IsSameAssetChain, out);
}

bool CheckMoMoM(uint256 hushNotarizationHash, uint256 momom)
{
    /*
     * Given a notarization hash and an MoMoM. Backnotarizations may arrive out of order
     * or multiple in the same block. So dereference the notarization hash to the corresponding
     * backnotarization and scan around the hushheight to see if the MoMoM is a match.
     * This is a sledgehammer approach...
     */

    Notarization bn;
    if (!GetBackNotarization(hushNotarizationHash, bn))
        return false;

    // Need to get block height of that backnotarization
    EvalRef eval;
    CBlockIndex block;
    CTransaction tx;
    if (!eval->GetTxConfirmed(bn.first, tx, block)){
        fprintf(stderr, "Can't get height of backnotarization, this should not happen\n");
        return false;
    }

    Notarization nota;
    auto checkMoMoM = [&](Notarization &nota) {
        return nota.second.MoMoM == momom;
    };

    return (bool) ScanNotarizationsFromHeight(block.GetHeight()-100, checkMoMoM, nota);

}

/*
* Check notaries approvals for the txoutproofs of burn tx
* (alternate check if MoMoM check has failed)
* Params:
* burntxid - txid of burn tx on the source chain
* rawproof - array of txids of notaries' proofs
*/
bool CheckNotariesApproval(uint256 burntxid, const std::vector<uint256> & notaryTxids) 
{
    int count = 0;

    // get notaries:
    uint8_t notaries_pubkeys[64][33];
    std::vector< std::vector<uint8_t> > alreadySigned;

    //unmarshal notaries approval txids
    for(auto notarytxid : notaryTxids ) {
        EvalRef eval;
        CBlockIndex block;
        CTransaction notarytx;  // tx with notary approval of txproof existence

        // get notary approval tx
        if (eval->GetTxConfirmed(notarytxid, notarytx, block)) {
            
            std::vector<uint8_t> vopret;
            if (!notarytx.vout.empty() && GetOpReturnData(notarytx.vout.back().scriptPubKey, vopret)) {
                std::vector<uint8_t> txoutproof;

                if (E_UNMARSHAL(vopret, ss >> txoutproof)) {
                    CMerkleBlock merkleBlock;
                    std::vector<uint256> prooftxids;
                    // extract block's merkle tree
                    if (E_UNMARSHAL(txoutproof, ss >> merkleBlock)) {

                        // extract proven txids:
                        merkleBlock.txn.ExtractMatches(prooftxids);
                        if (merkleBlock.txn.ExtractMatches(prooftxids) != merkleBlock.header.hashMerkleRoot ||  // check block merkle root is correct
                            std::find(prooftxids.begin(), prooftxids.end(), burntxid) != prooftxids.end()) {    // check burn txid is in proven txids list
                            
                            if (hush_notaries(notaries_pubkeys, block.GetHeight(), block.GetBlockTime()) >= 0) {
                                // check it is a notary who signed approved tx:
                                int i;
                                for (i = 0; i < sizeof(notaries_pubkeys) / sizeof(notaries_pubkeys[0]); i++) {
                                    std::vector<uint8_t> vnotarypubkey(notaries_pubkeys[i], notaries_pubkeys[i] + 33);
#ifdef TESTMODE
                                    char test_notary_pubkey_hex[] = "029fa302968bbae81f41983d2ec20445557b889d31227caec5d910d19b7510ef86";
                                    uint8_t test_notary_pubkey33[33];
                                    decode_hex(test_notary_pubkey33, 33, test_notary_pubkey_hex);
#endif
                                    if (CheckVinPubKey(notarytx, 0, notaries_pubkeys[i])   // is signed by a notary?
                                        && std::find(alreadySigned.begin(), alreadySigned.end(), vnotarypubkey) == alreadySigned.end()   // check if notary not re-used
#ifdef TESTMODE                                        
                                        || CheckVinPubKey(notarytx, 0, test_notary_pubkey33)  // test
#endif
                                    )   
                                    {
                                        alreadySigned.push_back(vnotarypubkey);
                                        count++;
                                        LOGSTREAM("importcoin", CCLOG_DEBUG1, stream << "CheckNotariesApproval() notary approval checked, count=" << count << std::endl);
                                        break;
                                    }
                                }
                                if (i == sizeof(notaries_pubkeys) / sizeof(notaries_pubkeys[0]))
                                    LOGSTREAM("importcoin", CCLOG_DEBUG1, stream << "CheckNotariesApproval() txproof not signed by a notary or reused" << std::endl);
                            }
                            else {
                                LOGSTREAM("importcoin", CCLOG_INFO, stream << "CheckNotariesApproval() cannot get current notaries pubkeys" << std::endl);
                            }
                        }
                        else  {
                            LOGSTREAM("importcoin", CCLOG_INFO, stream << "CheckNotariesApproval() burntxid not found in txoutproof or incorrect txoutproof" << std::endl);
                        }
                    }
                    else {
                        LOGSTREAM("importcoin", CCLOG_INFO, stream << "CheckNotariesApproval() could not unmarshal merkleBlock" << std::endl);
                    }
                }
                else {
                    LOGSTREAM("importcoin", CCLOG_INFO, stream << "CheckNotariesApproval() could not unmarshal txoutproof" << std::endl);
                }
            }
            else {
                LOGSTREAM("importcoin", CCLOG_INFO, stream << "CheckNotariesApproval() no opret in the notary tx" << std::endl);
            }
        }
        else {
            LOGSTREAM("importcoin", CCLOG_INFO, stream << "CheckNotariesApproval() could not load notary tx" << std::endl);
        }
    }

    bool retcode;
#ifdef TESTMODE
    if (count < 1) { // 1 for test
#else
    if (count < 5) { 
#endif
        LOGSTREAM("importcoin", CCLOG_INFO, stream << "CheckNotariesApproval() not enough signed notary transactions=" << count << std::endl);
        retcode = false;
    }
    else
        retcode = true;

    return retcode;
}


/*
 * On assetchain
 * in: txid
 * out: pair<notarizationTxHash,merkleBranch>
 */

TxProof GetAssetchainProof(uint256 hash,CTransaction burnTx)
{
    int nIndex;
    CBlockIndex* blockIndex;
    Notarization nota;
    std::vector<uint256> branch;
    {
        uint256 blockHash;
        CTransaction tx;
        if (!GetTransaction(hash, tx, blockHash, true))
            throw std::runtime_error("cannot find transaction");

        if (blockHash.IsNull())
            throw std::runtime_error("tx still in mempool");

        blockIndex = hush_getblockindex(blockHash);
        int h = blockIndex->GetHeight();
        // The assumption here is that the first notarization for a height GTE than
        // the transaction block height will contain the corresponding MoM. If there
        // are sequence issues with the notarizations this may fail.
        auto isTarget = [&](Notarization &nota) {
            if (!IsSameAssetChain(nota)) return false;
            return nota.second.height >= blockIndex->GetHeight();
        };
        if (!ScanNotarizationsFromHeight(blockIndex->GetHeight(), isTarget, nota))
            throw std::runtime_error("backnotarization not yet confirmed");

        // index of block in MoM leaves
        nIndex = nota.second.height - blockIndex->GetHeight();
    }

    // build merkle chain from blocks to MoM
    {
        std::vector<uint256> leaves, tree;
        for (int i=0; i<nota.second.MoMDepth; i++) {
            uint256 mRoot = chainActive[nota.second.height - i]->hashMerkleRoot;
            leaves.push_back(mRoot);
        }
        bool fMutated;
        BuildMerkleTree(&fMutated, leaves, tree);
        branch = GetMerkleBranch(nIndex, leaves.size(), tree);

        // Check branch
        uint256 ourResult = SafeCheckMerkleBranch(blockIndex->hashMerkleRoot, branch, nIndex);
        if (nota.second.MoM != ourResult)
            throw std::runtime_error("Failed merkle block->MoM");
    }

    // Now get the tx merkle branch
    {
        CBlock block;

        if (fHavePruned && !(blockIndex->nStatus & BLOCK_HAVE_DATA) && blockIndex->nTx > 0)
            throw std::runtime_error("Block not available (pruned data)");

        if(!ReadBlockFromDisk(block, blockIndex,1))
            throw std::runtime_error("Can't read block from disk");

        // Locate the transaction in the block
        int nTxIndex;
        for (nTxIndex = 0; nTxIndex < (int)block.vtx.size(); nTxIndex++)
            if (block.vtx[nTxIndex].GetHash() == hash)
                break;

        if (nTxIndex == (int)block.vtx.size())
            throw std::runtime_error("Error locating tx in block");

        std::vector<uint256> txBranch = block.GetMerkleBranch(nTxIndex);

        // Check branch
        if (block.hashMerkleRoot != CBlock::CheckMerkleBranch(hash, txBranch, nTxIndex))
            throw std::runtime_error("Failed merkle tx->block");

        // concatenate branches
        nIndex = (nIndex << txBranch.size()) + nTxIndex;
        branch.insert(branch.begin(), txBranch.begin(), txBranch.end());
    }

    // Check the proof
    if (nota.second.MoM != CBlock::CheckMerkleBranch(hash, branch, nIndex))
        throw std::runtime_error("Failed validating MoM");

    // All done!
    CDataStream ssProof(SER_NETWORK, PROTOCOL_VERSION);
    return std::make_pair(nota.second.txHash, MerkleBranch(nIndex, branch));
}
