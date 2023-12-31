// Copyright (c) 2016-2023 The Hush developers
// Distributed under the GPLv3 software license, see the accompanying
// file COPYING or https://www.gnu.org/licenses/gpl-3.0.en.html

#include "Address.hpp"
#include "NoteEncryption.hpp"
#include "hash.h"
#include "prf.h"
#include "streams.h"

#include <librustzcash.h>

const unsigned char ZCASH_SAPLING_FVFP_PERSONALIZATION[crypto_generichash_blake2b_PERSONALBYTES] =
    {'Z', 'c', 'a', 's', 'h', 'S', 'a', 'p', 'l', 'i', 'n', 'g', 'F', 'V', 'F', 'P'};

const uint32_t SAPLING_BRANCH_ID = 0x76b809bb;

namespace libzcash {

//! Sapling
uint256 SaplingPaymentAddress::GetHash() const {
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << *this;
    return Hash(ss.begin(), ss.end());
}

SaplingFullViewingKey SaplingExpandedSpendingKey::full_viewing_key() const {
    uint256 ak;
    uint256 nk;
    librustzcash_ask_to_ak(ask.begin(), ak.begin());
    librustzcash_nsk_to_nk(nsk.begin(), nk.begin());
    return SaplingFullViewingKey(ak, nk, ovk);
}

SaplingExpandedSpendingKey SaplingSpendingKey::expanded_spending_key() const {
    return SaplingExpandedSpendingKey(PRF_ask(*this), PRF_nsk(*this), PRF_ovk(*this));
}

SaplingFullViewingKey SaplingSpendingKey::full_viewing_key() const {
    return expanded_spending_key().full_viewing_key();
}

SaplingIncomingViewingKey SaplingFullViewingKey::in_viewing_key() const {
    uint256 ivk;
    librustzcash_crh_ivk(ak.begin(), nk.begin(), ivk.begin());
    return SaplingIncomingViewingKey(ivk);
}

bool SaplingFullViewingKey::is_valid() const {
    uint256 ivk;
    librustzcash_crh_ivk(ak.begin(), nk.begin(), ivk.begin());
    return !ivk.IsNull();
}

uint256 SaplingFullViewingKey::GetFingerprint() const {
    CBLAKE2bWriter ss(SER_GETHASH, 0, ZCASH_SAPLING_FVFP_PERSONALIZATION);
    ss << *this;
    return ss.GetHash();
}


SaplingSpendingKey SaplingSpendingKey::random() {
    while (true) {
        auto sk = SaplingSpendingKey(random_uint256());
        if (sk.full_viewing_key().is_valid()) {
            return sk;
        }
    }
}

boost::optional<SaplingPaymentAddress> SaplingIncomingViewingKey::address(diversifier_t d) const {
    uint256 pk_d;
    if (librustzcash_check_diversifier(d.data())) {
        librustzcash_ivk_to_pkd(this->begin(), d.data(), pk_d.begin());
        return SaplingPaymentAddress(d, pk_d);
    } else {
        return boost::none;
    }
}

SaplingPaymentAddress SaplingSpendingKey::default_address() const {
    // Iterates within default_diversifier to ensure a valid address is returned
    auto addrOpt = full_viewing_key().in_viewing_key().address(default_diversifier(*this));
    assert(addrOpt != boost::none);
    return addrOpt.value();
}

}

class IsValidAddressForNetwork : public boost::static_visitor<bool> {
    private:
        uint32_t branchId;
    public:
        IsValidAddressForNetwork(uint32_t consensusBranchId) : branchId(consensusBranchId) {}

        bool operator()(const libzcash::InvalidEncoding &addr) const {
            return false;
        }

        bool operator()(const libzcash::SaplingPaymentAddress &addr) const {
            if (SAPLING_BRANCH_ID == branchId)
                return true;
            else
                return false;
        }
};

bool IsValidPaymentAddress(const libzcash::PaymentAddress& zaddr, uint32_t consensusBranchId) {
    return boost::apply_visitor(IsValidAddressForNetwork(consensusBranchId), zaddr);
}

bool IsValidViewingKey(const libzcash::ViewingKey& vk) {
    return vk.which() != 0;
}
