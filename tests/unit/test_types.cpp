// Shared core types (contracts/core-interfaces.md section 1): Error, Result, Clock, Random, Cancel.
#include <cstring>
#include <set>
#include <string>

#include "pbc/types.h"
#include "test_support.h"

PB_TEST(a_default_error_is_ok) {
    pbc::Error e;
    PB_CHECK(e.ok());
    PB_CHECK(e.cls == pbc::ErrorClass::None);
    PB_CHECK(e.key.empty());
}

PB_TEST(only_the_none_class_is_ok) {
    const pbc::ErrorClass all[] = {pbc::ErrorClass::Network,     pbc::ErrorClass::Tls,         pbc::ErrorClass::Auth,
                                   pbc::ErrorClass::Server,      pbc::ErrorClass::Protocol,    pbc::ErrorClass::Unsupported,
                                   pbc::ErrorClass::Storage,     pbc::ErrorClass::Cancelled};
    for (pbc::ErrorClass c : all) {
        pbc::Error e;
        e.cls = c;
        PB_CHECK(!e.ok());
    }
}

PB_TEST(make_error_carries_class_key_and_detail) {
    pbc::Error e = pbc::make_error(pbc::ErrorClass::Tls, "cert_expired", "certificate has expired");
    PB_CHECK(!e.ok());
    PB_CHECK_EQ(e.key, std::string("cert_expired"));
    PB_CHECK_EQ(e.detail, std::string("certificate has expired"));
}

PB_TEST(result_carries_error_and_value) {
    pbc::Result<int> good;
    good.value = 7;
    PB_CHECK(good.ok());
    PB_CHECK_EQ(good.value, 7);

    pbc::Result<int> bad;
    bad.error = pbc::make_error(pbc::ErrorClass::Network, "timeout", "");
    PB_CHECK(!bad.ok());
}

PB_TEST(system_random_returns_different_16_byte_blocks) {
    pbc::SystemRandom rng;
    std::set<std::string> seen;
    for (int i = 0; i < 20; ++i) {
        uint8_t block[16];
        std::memset(block, 0, sizeof block);
        rng.bytes(block, sizeof block);
        seen.insert(std::string(reinterpret_cast<const char*>(block), sizeof block));
    }
    PB_CHECK_EQ(seen.size(), size_t(20));
}

PB_TEST(system_clock_returns_a_plausible_epoch_time) {
    pbc::SystemClock clock;
    PB_CHECK(clock.now() > 1700000000);  // after November 2023
}

namespace {
struct FlagCancel : pbc::Cancel {
    bool flag = false;
    bool requested() const override { return flag; }
};
}  // namespace

PB_TEST(a_cancel_fake_flips_requested) {
    FlagCancel c;
    pbc::Cancel& base = c;
    PB_CHECK(!base.requested());
    c.flag = true;
    PB_CHECK(base.requested());
}

PB_TEST_MAIN()
