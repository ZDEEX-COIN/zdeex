// Copyright (c) 2016-2023 The Hush developers
// Distributed under the GPLv3 software license, see the accompanying
// file COPYING or https://www.gnu.org/licenses/gpl-3.0.en.html
/*
 * Generated by asn1c-0.9.28 (http://lionet.info/asn1c)
 * From ASN.1 module "Crypto-Conditions"
 * 	found in "CryptoConditions.asn"
 */

#ifndef	_ThresholdFingerprintContents_H_
#define	_ThresholdFingerprintContents_H_


#include "asn_application.h"

/* Including external dependencies */
#include <NativeInteger.h>
#include <asn_SET_OF.h>
#include <constr_SET_OF.h>
#include <constr_SEQUENCE.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct Condition;

/* ThresholdFingerprintContents */
typedef struct ThresholdFingerprintContents {
	long	 threshold;
	struct subconditions2 {
		A_SET_OF(struct Condition) list;
		
		/* Context for parsing across buffer boundaries */
		asn_struct_ctx_t _asn_ctx;
	} subconditions2;
	
	/* Context for parsing across buffer boundaries */
	asn_struct_ctx_t _asn_ctx;
} ThresholdFingerprintContents_t;

/* Implementation */
extern asn_TYPE_descriptor_t asn_DEF_ThresholdFingerprintContents;

#ifdef __cplusplus
}
#endif

/* Referred external types */
#include "Condition.h"

#endif	/* _ThresholdFingerprintContents_H_ */
#include <asn_internal.h>
