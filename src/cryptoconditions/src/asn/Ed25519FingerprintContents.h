// Copyright (c) 2016-2023 The Hush developers
// Distributed under the GPLv3 software license, see the accompanying
// file COPYING or https://www.gnu.org/licenses/gpl-3.0.en.html
/*
 * Generated by asn1c-0.9.28 (http://lionet.info/asn1c)
 * From ASN.1 module "Crypto-Conditions"
 * 	found in "CryptoConditions.asn"
 */

#ifndef	_Ed25519FingerprintContents_H_
#define	_Ed25519FingerprintContents_H_


#include "asn_application.h"

/* Including external dependencies */
#include <OCTET_STRING.h>
#include <constr_SEQUENCE.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ed25519FingerprintContents */
typedef struct Ed25519FingerprintContents {
	OCTET_STRING_t	 publicKey;
	
	/* Context for parsing across buffer boundaries */
	asn_struct_ctx_t _asn_ctx;
} Ed25519FingerprintContents_t;

/* Implementation */
extern asn_TYPE_descriptor_t asn_DEF_Ed25519FingerprintContents;

#ifdef __cplusplus
}
#endif

#endif	/* _Ed25519FingerprintContents_H_ */
#include <asn_internal.h>
