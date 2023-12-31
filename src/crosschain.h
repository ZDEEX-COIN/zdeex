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
#ifndef HUSH_CROSSCHAIN_H
#define HUSH_CROSSCHAIN_H
#include "cc/eval.h"

const int CROSSCHAIN_HUSH = 1;

typedef struct CrosschainAuthority {
    uint8_t notaries[64][33];
    int8_t size;
    int8_t requiredSigs;
} CrosschainAuthority;

int GetSymbolAuthority(const char* symbol);
bool CheckTxAuthority(const CTransaction &tx, CrosschainAuthority auth);

/* On assetchain */
TxProof GetAssetchainProof(uint256 hash,CTransaction burnTx);

/* On HUSH */
uint256 CalculateProofRoot(const char* symbol, uint32_t targetCCid, int hushHeight,
        std::vector<uint256> &moms, uint256 &destNotarizationTxid);
TxProof GetCrossChainProof(const uint256 txid, const char* targetSymbol, uint32_t targetCCid,
        const TxProof assetChainProof,int32_t offset);
void CompleteImportTransaction(CTransaction &importTx,int32_t offset);

/* On assetchain */
bool CheckMoMoM(uint256 hushNotarizationHash, uint256 momom);
bool CheckNotariesApproval(uint256 burntxid, const std::vector<uint256> & notaryTxids);

#endif /* HUSH_CROSSCHAIN_H */
