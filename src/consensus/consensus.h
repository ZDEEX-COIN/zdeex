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
#ifndef HUSH_CONSENSUS_CONSENSUS_H
#define HUSH_CONSENSUS_CONSENSUS_H

/** The minimum allowed block version (network rule) */
static const int32_t MIN_BLOCK_VERSION = 4;
/** The minimum allowed transaction version (network rule) */
static const int32_t SPROUT_MIN_TX_VERSION = 1;
/** The minimum allowed Overwinter transaction version (network rule) */
static const int32_t OVERWINTER_MIN_TX_VERSION = 3;
/** The maximum allowed Overwinter transaction version (network rule) */
static const int32_t OVERWINTER_MAX_TX_VERSION = 3;
/** The minimum allowed Sapling transaction version (network rule) */
static const int32_t SAPLING_MIN_TX_VERSION = 4;
/** The maximum allowed Sapling transaction version (network rule) */
static const int32_t SAPLING_MAX_TX_VERSION = 4;
/** The maximum allowed size for a serialized block, in bytes (network rule) */
//static const unsigned int MAX_BLOCK_SIZE = 2000000;
/** The maximum allowed number of signature check operations in a block (network rule) */
extern unsigned int MAX_BLOCK_SIGOPS;
/** The maximum size of a transaction (network rule) */
static const unsigned int MAX_TX_SIZE_BEFORE_SAPLING = 100000;
static const unsigned int MAX_TX_SIZE_AFTER_SAPLING = (2 * MAX_TX_SIZE_BEFORE_SAPLING); //MAX_BLOCK_SIZE;
/** Coinbase transaction outputs can only be spent after this number of new blocks (network rule) */
extern int COINBASE_MATURITY;
/** The minimum value which is invalid for expiry height, used by CTransaction and CMutableTransaction */
static constexpr uint32_t TX_EXPIRY_HEIGHT_THRESHOLD = 500000000;

/** Flags for LockTime() */
enum {
    /* Use GetMedianTimePast() instead of nTime for end point timestamp. */
    LOCKTIME_MEDIAN_TIME_PAST = (1 << 1),
};

/** Used as the flags parameter to CheckFinalTx() in non-consensus code */
static const unsigned int STANDARD_LOCKTIME_VERIFY_FLAGS = LOCKTIME_MEDIAN_TIME_PAST;

#endif // HUSH_CONSENSUS_CONSENSUS_H
