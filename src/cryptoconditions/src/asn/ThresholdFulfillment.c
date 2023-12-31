// Copyright (c) 2016-2023 The Hush developers
// Distributed under the GPLv3 software license, see the accompanying
// file COPYING or https://www.gnu.org/licenses/gpl-3.0.en.html
/*
 * Generated by asn1c-0.9.28 (http://lionet.info/asn1c)
 * From ASN.1 module "Crypto-Conditions"
 * 	found in "CryptoConditions.asn"
 */

#include "ThresholdFulfillment.h"

static asn_TYPE_member_t asn_MBR_subfulfillments_2[] = {
	{ ATF_POINTER, 0, 0,
		-1 /* Ambiguous tag (CHOICE?) */,
		0,
		&asn_DEF_Fulfillment,
		0,	/* Defer constraints checking to the member type */
		0,	/* PER is not compiled, use -gen-PER */
		0,
		""
		},
};
static const ber_tlv_tag_t asn_DEF_subfulfillments_tags_2[] = {
	(ASN_TAG_CLASS_CONTEXT | (0 << 2)),
	(ASN_TAG_CLASS_UNIVERSAL | (17 << 2))
};
static asn_SET_OF_specifics_t asn_SPC_subfulfillments_specs_2 = {
	sizeof(struct subfulfillments),
	offsetof(struct subfulfillments, _asn_ctx),
	2,	/* XER encoding is XMLValueList */
};
static /* Use -fall-defs-global to expose */
asn_TYPE_descriptor_t asn_DEF_subfulfillments_2 = {
	"subfulfillments",
	"subfulfillments",
	SET_OF_free,
	SET_OF_print,
	SET_OF_constraint,
	SET_OF_decode_ber,
	SET_OF_encode_der,
	SET_OF_decode_xer,
	SET_OF_encode_xer,
	0, 0,	/* No PER support, use "-gen-PER" to enable */
	0,	/* Use generic outmost tag fetcher */
	asn_DEF_subfulfillments_tags_2,
	sizeof(asn_DEF_subfulfillments_tags_2)
		/sizeof(asn_DEF_subfulfillments_tags_2[0]) - 1, /* 1 */
	asn_DEF_subfulfillments_tags_2,	/* Same as above */
	sizeof(asn_DEF_subfulfillments_tags_2)
		/sizeof(asn_DEF_subfulfillments_tags_2[0]), /* 2 */
	0,	/* No PER visible constraints */
	asn_MBR_subfulfillments_2,
	1,	/* Single element */
	&asn_SPC_subfulfillments_specs_2	/* Additional specs */
};

static asn_TYPE_member_t asn_MBR_subconditions_4[] = {
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
static const ber_tlv_tag_t asn_DEF_subconditions_tags_4[] = {
	(ASN_TAG_CLASS_CONTEXT | (1 << 2)),
	(ASN_TAG_CLASS_UNIVERSAL | (17 << 2))
};
static asn_SET_OF_specifics_t asn_SPC_subconditions_specs_4 = {
	sizeof(struct subconditions),
	offsetof(struct subconditions, _asn_ctx),
	2,	/* XER encoding is XMLValueList */
};
static /* Use -fall-defs-global to expose */
asn_TYPE_descriptor_t asn_DEF_subconditions_4 = {
	"subconditions",
	"subconditions",
	SET_OF_free,
	SET_OF_print,
	SET_OF_constraint,
	SET_OF_decode_ber,
	SET_OF_encode_der,
	SET_OF_decode_xer,
	SET_OF_encode_xer,
	0, 0,	/* No PER support, use "-gen-PER" to enable */
	0,	/* Use generic outmost tag fetcher */
	asn_DEF_subconditions_tags_4,
	sizeof(asn_DEF_subconditions_tags_4)
		/sizeof(asn_DEF_subconditions_tags_4[0]) - 1, /* 1 */
	asn_DEF_subconditions_tags_4,	/* Same as above */
	sizeof(asn_DEF_subconditions_tags_4)
		/sizeof(asn_DEF_subconditions_tags_4[0]), /* 2 */
	0,	/* No PER visible constraints */
	asn_MBR_subconditions_4,
	1,	/* Single element */
	&asn_SPC_subconditions_specs_4	/* Additional specs */
};

static asn_TYPE_member_t asn_MBR_ThresholdFulfillment_1[] = {
	{ ATF_NOFLAGS, 0, offsetof(struct ThresholdFulfillment, subfulfillments),
		(ASN_TAG_CLASS_CONTEXT | (0 << 2)),
		0,
		&asn_DEF_subfulfillments_2,
		0,	/* Defer constraints checking to the member type */
		0,	/* PER is not compiled, use -gen-PER */
		0,
		"subfulfillments"
		},
	{ ATF_NOFLAGS, 0, offsetof(struct ThresholdFulfillment, subconditions),
		(ASN_TAG_CLASS_CONTEXT | (1 << 2)),
		0,
		&asn_DEF_subconditions_4,
		0,	/* Defer constraints checking to the member type */
		0,	/* PER is not compiled, use -gen-PER */
		0,
		"subconditions"
		},
};
static const ber_tlv_tag_t asn_DEF_ThresholdFulfillment_tags_1[] = {
	(ASN_TAG_CLASS_UNIVERSAL | (16 << 2))
};
static const asn_TYPE_tag2member_t asn_MAP_ThresholdFulfillment_tag2el_1[] = {
    { (ASN_TAG_CLASS_CONTEXT | (0 << 2)), 0, 0, 0 }, /* subfulfillments */
    { (ASN_TAG_CLASS_CONTEXT | (1 << 2)), 1, 0, 0 } /* subconditions */
};
static asn_SEQUENCE_specifics_t asn_SPC_ThresholdFulfillment_specs_1 = {
	sizeof(struct ThresholdFulfillment),
	offsetof(struct ThresholdFulfillment, _asn_ctx),
	asn_MAP_ThresholdFulfillment_tag2el_1,
	2,	/* Count of tags in the map */
	0, 0, 0,	/* Optional elements (not needed) */
	-1,	/* Start extensions */
	-1	/* Stop extensions */
};
asn_TYPE_descriptor_t asn_DEF_ThresholdFulfillment = {
	"ThresholdFulfillment",
	"ThresholdFulfillment",
	SEQUENCE_free,
	SEQUENCE_print,
	SEQUENCE_constraint,
	SEQUENCE_decode_ber,
	SEQUENCE_encode_der,
	SEQUENCE_decode_xer,
	SEQUENCE_encode_xer,
	0, 0,	/* No PER support, use "-gen-PER" to enable */
	0,	/* Use generic outmost tag fetcher */
	asn_DEF_ThresholdFulfillment_tags_1,
	sizeof(asn_DEF_ThresholdFulfillment_tags_1)
		/sizeof(asn_DEF_ThresholdFulfillment_tags_1[0]), /* 1 */
	asn_DEF_ThresholdFulfillment_tags_1,	/* Same as above */
	sizeof(asn_DEF_ThresholdFulfillment_tags_1)
		/sizeof(asn_DEF_ThresholdFulfillment_tags_1[0]), /* 1 */
	0,	/* No PER visible constraints */
	asn_MBR_ThresholdFulfillment_1,
	2,	/* Elements count */
	&asn_SPC_ThresholdFulfillment_specs_1	/* Additional specs */
};

