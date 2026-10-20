// After two consecutive 401 answers the app stops sending requests for the account until the user tests again
// (research R-12, api-notes section 9: wrong passwords are what triggers the server's brute-force protection).
#include <string>

#include "pbc/auth_guard.h"
#include "pbc/store.h"
#include "pbc/store_files.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;

std::unique_ptr<pbc::Store> open_store(const pbtest::TmpDir& d) {
    auto r = pbc::Store::open(d.file("data"), g_clock);
    if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
    return std::move(r.value);
}

void with_account(pbc::Store& s) {
    pbc::AccountRow a;
    a.base_url = "https://cloud.example.org";
    a.login_name = "demo";
    a.user_id = "demo";
    a.state = "ok";
    PB_CHECK(s.save_account(a).ok());
}

const pbc::Error kAuth = pbc::make_error(pbc::ErrorClass::Auth, "auth_failed");
const pbc::Error kDown = pbc::make_error(pbc::ErrorClass::Network, "server_unreachable");

}  // namespace

PB_TEST(one_failed_login_does_not_block_but_two_in_a_row_do) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    with_account(*s);
    pbc::AuthGuard guard(*s);
    PB_CHECK(!guard.blocked());
    guard.record(kAuth);
    PB_CHECK(!guard.blocked());
    PB_CHECK_EQ(s->account()->state, std::string("ok"));
    guard.record(kAuth);
    PB_CHECK(guard.blocked());
    PB_CHECK_EQ(s->account()->state, std::string("needs_signin"));
    PB_CHECK_EQ(s->account()->problem_key, std::string("auth_failed"));
}

PB_TEST(a_success_clears_the_count_and_other_errors_do_not_count_or_clear) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    with_account(*s);
    pbc::AuthGuard guard(*s);
    guard.record(kAuth);
    guard.record(kDown);          // no answer from the server: not a 401, and it does not undo the first one
    PB_CHECK(!guard.blocked());
    guard.record(pbc::Error());   // a success
    guard.record(kAuth);
    PB_CHECK(!guard.blocked());   // the count started again
    guard.record(kDown);
    guard.record(kAuth);
    PB_CHECK(guard.blocked());
}

PB_TEST(the_block_survives_a_restart_and_a_test_by_the_user_lifts_it) {
    pbtest::TmpDir d;
    {
        auto s = open_store(d);
        with_account(*s);
        pbc::AuthGuard guard(*s);
        guard.record(kAuth);
        guard.record(kAuth);
        PB_CHECK(guard.blocked());
    }
    auto s = open_store(d);
    pbc::AuthGuard guard(*s);
    PB_CHECK(guard.blocked());    // read from the account row
    guard.reset();                // the user tapped Test
    PB_CHECK(!guard.blocked());
    PB_CHECK_EQ(s->account()->state, std::string("untested"));
    guard.record(kAuth);
    PB_CHECK(!guard.blocked());   // and the count starts again at zero
}

PB_TEST(without_an_account_there_is_nothing_to_guard) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    pbc::AuthGuard guard(*s);
    guard.record(kAuth);
    guard.record(kAuth);
    PB_CHECK(!guard.blocked());   // the first Test of a new account is always allowed
}

PB_TEST_MAIN()
