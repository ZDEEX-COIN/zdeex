// Copyright (c) 2016-2023 The Hush developers
// Distributed under the GPLv3 software license, see the accompanying
// file COPYING or https://www.gnu.org/licenses/gpl-3.0.en.html
/*
 * Generated by asn1c-0.9.28 (http://lionet.info/asn1c)
 * From ASN.1 module "Crypto-Conditions"
 * 	found in "CryptoConditions.asn"
 */

#include "ThresholdFingerprintContents.h"

static int
memb_threshold_constraint_1(asn_TYPE_descriptor_t *td, const void *sptr,
			asn_app_constraint_failed_f *ctfailcb, void *app_key) {
	long value;
	
	if(!sptr) {
		ASN__CTFAIL(app_key, td, sptr,
			"%s: value not given (%s:%d)",
			td->name, __FILE__, __LINE__);
		return -1;
	}
	
	value = *(const long *)sptr;
	
	if((value >= 1 && value <= 65535)) {
		/* Constraint check succeeded */
		return 0;
	} else {
		ASN__CTFAIL(app_key, td, sptr,
			"%s: constraint failed (%s:%d)",
			td->name, __FILE__, __LINE__);
		return -1;
	}
}

static asn_TYPE_member_t asn_MBR_subconditions2_3[] = {
	{ ATF_POINTER, 0, 0,
		-1 /* Ambiguous tag (CHOICE?) */,
		0,
		&asn_DEF_Condition,
		0,	/* Defer constraints checking to the member type */
		0,	/* PER is not compiled, use -gen-PER */
		0,
		""
		},
};
static const ber_tlv_tag_t asn_DEF_subconditions2_tags_3[] = {
	(ASN_TAG_CLASS_CONTEXT | (1 << 2)),
	(ASN_TAG_CLASS_UNIVERSAL | (17 << 2))
};
static asn_SET_OF_specifics_t asn_SPC_subconditions2_specs_3 = {
	sizeof(struct subconditions2),
	offsetof(struct subconditions2, _asn_ctx),
	2,	/* XER encoding is XMLValueList */
};
static /* Use -fall-defs-global to expose */
asn_TYPE_descriptor_t asn_DEF_subconditions2_3 = {
	"subconditions2",
	"subconditions2",
	SET_OF_free,
	SET_OF_print,
	SET_OF_constraint,
	SET_OF_decode_ber,
	SET_OF_encode_der,
	SET_OF_decode_xer,
	SET_OF_encode_xer,
	0, 0,	/* No PER support, use "-gen-PER" to enable */
	0,	/* Use generic outmost tag fetcher */
	asn_DEF_subconditions2_tags_3,
	sizeof(asn_DEF_subconditions2_tags_3)
		/sizeof(asn_DEF_subconditions2_tags_3[0]) - 1, /* 1 */
	asn_DEF_subconditions2_tags_3,	/* Same as above */
	sizeof(asn_DEF_subconditions2_tags_3)
		/sizeof(asn_DEF_subconditions2_tags_3[0]), /* 2 */
	0,	/* No PER visible constraints */
	asn_MBR_subconditions2_3,
	1,	/* Single element */
	&asn_SPC_subconditions2_specs_3	/* Additional specs */
};

static asn_TYPE_member_t asn_MBR_ThresholdFingerprintContents_1[] = {
	{ ATF_NOFLAGS, 0, offsetof(struct ThresholdFingerprintContents, threshold),
		(ASN_TAG_CLASS_CONTEXT | (0 << 2)),
		-1,	/* IMPLICIT tag at current level */
		&asn_DEF_NativeInteger,
		memb_threshold_constraint_1,
		0,	/* PER is not compiled, use -gen-PER */
		0,
		"threshold"
		},
	{ ATF_NOFLAGS, 0, offsetof(struct ThresholdFingerprintContents, subconditions2),
		(ASN_TAG_CLASS_CONTEXT | (1 << 2)),
		0,
		&asn_DEF_subconditions2_3,
		0,	/* Defer constraints checking to the member type */
		0,	/* PER is not compiled, use -gen-PER */
		0,
		"subconditions2"
		},
};
static const ber_tlv_tag_t asn_DEF_ThresholdFingerprintContents_tags_1[] = {
	(ASN_TAG_CLASS_UNIVERSAL | (16 << 2))
};
static const asn_TYPE_tag2member_t asn_MAP_ThresholdFingerprintContents_tag2el_1[] = {
    { (ASN_TAG_CLASS_CONTEXT | (0 << 2)), 0, 0, 0 }, /* threshold */
    { (ASN_TAG_CLASS_CONTEXT | (1 << 2)), 1, 0, 0 } /* subconditions2 */
};
static asn_SEQUENCE_specifics_t asn_SPC_ThresholdFingerprintContents_specs_1 = {
	sizeof(struct ThresholdFingerprintContents),
	offsetof(struct ThresholdFingerprintContents, _asn_ctx),
	asn_MAP_ThresholdFingerprintContents_tag2el_1,
	2,	/* Count of tags in the map */
	0, 0, 0,	/* Optional elements (not needed) */
	-1,	/* Start extensions */
	-1	/* Stop extensions */
};
asn_TYPE_descriptor_t asn_DEF_ThresholdFingerprintContents = {
	"ThresholdFingerprintContents",
	"ThresholdFingerprintContents",
	SEQUENCE_free,
	SEQUENCE_print,
	SEQUENCE_constraint,
	SEQUENCE_decode_ber,
	SEQUENCE_encode_der,
	SEQUENCE_decode_xer,
	SEQUENCE_encode_xer,
	0, 0,	/* No PER support, use "-gen-PER" to enable */
	0,	/* Use generic outmost tag fetcher */
	asn_DEF_ThresholdFingerprintContents_tags_1,
	sizeof(asn_DEF_ThresholdFingerprintContents_tags_1)
		/sizeof(asn_DEF_ThresholdFingerprintContents_tags_1[0]), /* 1 */
	asn_DEF_ThresholdFingerprintContents_tags_1,	/* Same as above */
	sizeof(asn_DEF_ThresholdFingerprintContents_tags_1)
		/sizeof(asn_DEF_ThresholdFingerprintContents_tags_1[0]), /* 1 */
	0,	/* No PER visible constraints */
	asn_MBR_ThresholdFingerprintContents_1,
	2,	/* Elements count */
	&asn_SPC_ThresholdFingerprintContents_specs_1	/* Additional specs */
};

