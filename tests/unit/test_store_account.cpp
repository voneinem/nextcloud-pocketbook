// The account row (data-model.md "account"): one account, validated on the way in, and the sign-out order that keeps the
// password file from outliving the account (constitution Principle IV).
#include <string>

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

pbc::AccountRow row() {
    pbc::AccountRow a;
    a.base_url = "https://cloud.example.org";
    a.login_name = "demo";
    a.user_id = "demo";
    a.server_name = "Example Cloud";
    a.server_version = "31.0.4";
    a.quota_used = 1000;
    a.quota_total = 10000;
    a.checked_at = 1792497600;
    a.state = "ok";
    return a;
}

}  // namespace

PB_TEST(there_is_no_account_at_first_and_a_saved_one_comes_back_unchanged) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(!s->account().has_value());
    PB_CHECK(s->save_account(row()).ok());
    const auto a = s->account();
    PB_CHECK(a.has_value());
    PB_CHECK_EQ(a->base_url, std::string("https://cloud.example.org"));
    PB_CHECK_EQ(a->login_name, std::string("demo"));
    PB_CHECK_EQ(a->user_id, std::string("demo"));
    PB_CHECK_EQ(a->server_name, std::string("Example Cloud"));
    PB_CHECK_EQ(*a->quota_used, int64_t(1000));
    PB_CHECK_EQ(*a->quota_total, int64_t(10000));
    PB_CHECK_EQ(a->state, std::string("ok"));
}

PB_TEST(saving_again_replaces_the_single_row) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->save_account(row()).ok());
    pbc::AccountRow b = row();
    b.login_name = "other";
    b.quota_total = -1;   // unlimited
    b.quota_used = std::nullopt;
    PB_CHECK(s->save_account(b).ok());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM account"), std::string("1"));
    PB_CHECK_EQ(s->account()->login_name, std::string("other"));
    PB_CHECK_EQ(*s->account()->quota_total, int64_t(-1));
    PB_CHECK(!s->account()->quota_used.has_value());
}

PB_TEST(an_address_that_is_not_https_is_refused_and_nothing_is_saved) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    for (const char* bad : {"http://cloud.example.org", "cloud.example.org", "", "ftp://cloud.example.org", "https://u:p@cloud.example.org"}) {
        pbc::AccountRow a = row();
        a.base_url = bad;
        PB_CHECK_EQ(s->save_account(a).key, std::string("address_invalid"));
    }
    PB_CHECK(!s->account().has_value());
}

PB_TEST(a_user_id_that_could_build_a_hostile_path_is_refused) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    for (const std::string bad : {std::string(""), std::string("a/b"), std::string(".."), std::string("."), std::string("a\nb"),
                                  std::string("a\x01"), std::string("a\0b", 3), std::string(300, 'x')}) {
        pbc::AccountRow a = row();
        a.user_id = bad;
        PB_CHECK_EQ(s->save_account(a).key, std::string("protocol_error"));
    }
    pbc::AccountRow ok = row();
    ok.user_id = "alice.smith@example.org";   // an e-mail style id is a normal user id
    PB_CHECK(s->save_account(ok).ok());
    PB_CHECK(!s->account().has_value() == false);
}

PB_TEST(a_login_name_is_required_and_the_server_name_is_cut_at_64_characters) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    pbc::AccountRow a = row();
    a.login_name = "";
    PB_CHECK(!s->save_account(a).ok());
    a = row();
    a.server_name = std::string(100, 'n');
    PB_CHECK(s->save_account(a).ok());
    PB_CHECK_EQ(s->account()->server_name.size(), size_t(64));
}

PB_TEST(the_state_can_be_updated_and_only_to_a_known_state) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(!s->set_account_state("ok", "").ok());   // no account yet
    PB_CHECK(s->save_account(row()).ok());
    PB_CHECK(s->set_account_state("needs_signin", "auth_failed").ok());
    PB_CHECK_EQ(s->account()->state, std::string("needs_signin"));
    PB_CHECK_EQ(s->account()->problem_key, std::string("auth_failed"));
    PB_CHECK(!s->set_account_state("weird", "").ok());
    PB_CHECK_EQ(s->account()->state, std::string("needs_signin"));   // unchanged
}

PB_TEST(clearing_the_account_removes_the_row_but_not_the_downloaded_copies_or_marks) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->save_account(row()).ok());
    PB_CHECK(s->debug_exec("INSERT INTO local_files(remote_path, local_path, size, mtime, remote_etag, downloaded_at) VALUES('/a', 'a', 1, 2, '\"e\"', 3)").empty());
    PB_CHECK(s->debug_exec("INSERT INTO offline_marks(remote_path, kind, marked_at) VALUES('/Books', 'folder', 1)").empty());
    PB_CHECK(s->clear_account().ok());
    PB_CHECK(!s->account().has_value());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM local_files"), std::string("1"));    // the user's files stay on the reader
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM offline_marks"), std::string("1"));
    PB_CHECK(s->clear_account().ok());   // idempotent
}

PB_TEST(the_cache_survives_a_sign_out_and_a_sign_in_to_the_same_server_but_not_a_different_one) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->save_account(row()).ok());
    PB_CHECK(s->debug_exec("INSERT INTO folders(path, last_access_at) VALUES('/', 1)").empty());
    PB_CHECK(s->debug_exec("INSERT INTO entries(parent, name, kind) VALUES('/', 'a', 'file')").empty());
    PB_CHECK(s->debug_exec("INSERT INTO favorites(remote_path, kind, position) VALUES('/a', 'file', 0)").empty());
    PB_CHECK(s->clear_account().ok());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM entries"), std::string("1"));     // signed out: the reader still shows what it knew
    PB_CHECK(s->save_account(row()).ok());                                              // the same server and user again
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM entries"), std::string("1"));
    pbc::AccountRow other = row();
    other.base_url = "https://other.example.org";
    PB_CHECK(s->save_account(other).ok());                                              // another server: its listings are not this one's
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM folders"), std::string("0"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM entries"), std::string("0"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM favorites"), std::string("0"));
}

PB_TEST_MAIN()
