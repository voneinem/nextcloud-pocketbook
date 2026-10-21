// What the account check learns about the server for the write side (specs/002-manage-server-files, contracts/server-requests.md W1, research
// R-9 and R-11): the capabilities are untrusted, bounded and never guessed. A missing value is "unknown", never "nothing is forbidden".
#include <string>
#include <vector>

#include "fake_transport.h"
#include "fixtures.h"
#include "pbc/json_min.h"
#include "pbc/nc_server.h"
#include "pbc/server_caps.h"
#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;

pbc::ServerCaps caps_of(const std::string& fixture_name) {
    const auto doc = pbc::parse_json(pbtest::fixture("nextcloud/write/capabilities/" + fixture_name));
    if (!doc.ok()) PB_FAIL_AT("the fixture is not JSON: " + fixture_name);
    const pbc::JsonValue* v = &doc.value;
    for (const char* k : {"ocs", "data", "capabilities"}) {
        v = v->get(k);
        if (!v) PB_FAIL_AT("no capabilities in " + fixture_name);
    }
    return pbc::server_caps_from_capabilities(*v);
}

pbc::AccountRow account(const std::string& user) {
    pbc::AccountRow a;
    a.base_url = "https://cloud.example.org";
    a.login_name = user;
    a.user_id = user;
    return a;
}

std::unique_ptr<pbc::Store> open_store(const pbtest::TmpDir& d) {
    auto r = pbc::Store::open(d.file("data"), g_clock);
    if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
    return std::move(r.value);
}

}  // namespace

PB_TEST(the_capabilities_of_a_server_with_trash_and_chunking_are_read) {
    const pbc::ServerCaps c = caps_of("capabilities_write.unverified.json");
    PB_CHECK(c.trash.has_value() && *c.trash);
    PB_CHECK(c.chunking.has_value() && *c.chunking);
    PB_CHECK_EQ(c.max_chunk.value_or(-1), 104857600LL);
    PB_CHECK_EQ(c.forbidden_chars, std::string("\\/<>:\"|?*"));
    PB_CHECK((c.forbidden_names == std::vector<std::string>{".htaccess", "CON", "AUX"}));
    PB_CHECK((c.forbidden_basenames == std::vector<std::string>{"con", "prn", "aux"}));
    PB_CHECK((c.forbidden_extensions == std::vector<std::string>{".part", ".filepart"}));
    PB_CHECK(pbc::delete_offered(c));
    PB_CHECK(pbc::chunked_available(c));
}

PB_TEST(a_missing_key_is_unknown_and_unknown_never_offers_delete) {
    const pbc::ServerCaps none = pbc::server_caps_from_capabilities(pbc::JsonValue());
    PB_CHECK(!none.trash.has_value());
    PB_CHECK(!none.chunking.has_value());
    PB_CHECK(!none.max_chunk.has_value());
    PB_CHECK(none.forbidden_chars.empty() && none.forbidden_names.empty() && none.forbidden_basenames.empty() && none.forbidden_extensions.empty());
    PB_CHECK(!pbc::delete_offered(none));
    PB_CHECK(!pbc::chunked_available(none));

    const pbc::ServerCaps minimal = caps_of("capabilities_minimal.unverified.json");
    PB_CHECK(minimal.trash.has_value() && !*minimal.trash);   // the server said no
    PB_CHECK(!pbc::delete_offered(minimal));
    PB_CHECK(!minimal.chunking.has_value());
}

PB_TEST(hostile_or_oversized_values_are_skipped_and_bounded) {
    const pbc::ServerCaps c = caps_of("capabilities_hostile.unverified.json");
    PB_CHECK(!c.trash.has_value());       // "yes" is not a boolean
    PB_CHECK(!c.chunking.has_value());    // a number is not a version string
    PB_CHECK(!c.max_chunk.has_value());   // negative
    PB_CHECK_EQ(c.forbidden_chars.size(), size_t(64));
    PB_CHECK(c.forbidden_chars.compare(0, 2, "\\<") == 0);   // numbers, null, objects, control characters and long strings were skipped
    for (char ch : c.forbidden_chars) PB_CHECK(static_cast<unsigned char>(ch) >= 0x20);
    PB_CHECK_EQ(c.forbidden_names.size(), size_t(64));
    PB_CHECK_EQ(c.forbidden_names[0], std::string("ok.txt"));   // the 40-byte name and the one with a newline were skipped
    PB_CHECK_EQ(c.forbidden_names[1], std::string("n0"));
    for (const std::string& n : c.forbidden_names) PB_CHECK(n.size() <= 32 && n.find('\n') == std::string::npos);
    PB_CHECK(c.forbidden_basenames.empty());    // a string where an array is expected
    PB_CHECK(c.forbidden_extensions.empty());   // an object where an array is expected
    PB_CHECK(!pbc::delete_offered(c));
}

PB_TEST(a_maximum_chunk_size_beyond_reason_is_unknown) {
    const pbc::ServerCaps c = caps_of("capabilities_huge_max.unverified.json");
    PB_CHECK(!c.max_chunk.has_value());
    PB_CHECK_EQ(pbc::chunk_size_for(c), 10ll << 20);
}

PB_TEST(the_chunk_size_is_ten_mib_or_the_servers_smaller_maximum_never_below_five) {
    pbc::ServerCaps c;
    PB_CHECK_EQ(pbc::chunk_size_for(c), 10ll << 20);                 // nothing stated
    c.max_chunk = 104857600;
    PB_CHECK_EQ(pbc::chunk_size_for(c), 10ll << 20);                 // a large maximum does not make chunks larger
    c.max_chunk = 7340032;
    PB_CHECK_EQ(pbc::chunk_size_for(c), 7340032LL);                  // a maximum between 5 and 10 MiB is honored
    c.max_chunk = 5ll << 20;
    PB_CHECK_EQ(pbc::chunk_size_for(c), 5ll << 20);
    c.max_chunk = 1ll << 20;
    PB_CHECK_EQ(pbc::chunk_size_for(c), 5ll << 20);                  // the protocol minimum
    c.max_chunk = 10ll << 20;
    PB_CHECK_EQ(pbc::chunk_size_for(c), 10ll << 20);
    const pbc::ServerCaps small = caps_of("capabilities_small_max.unverified.json");
    PB_CHECK_EQ(pbc::chunk_size_for(small), 7340032LL);
}

PB_TEST(the_store_keeps_the_capabilities_replaces_them_as_a_whole_and_forgets_them_with_the_account) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(!s->server_caps().has_value());
    PB_CHECK(s->save_account(account("demo")).ok());

    pbc::ServerCaps c = caps_of("capabilities_write.unverified.json");
    c.checked_at = 1790000000;
    PB_CHECK(s->save_server_caps(c).ok());
    auto back = s->server_caps();
    PB_CHECK(back.has_value());
    PB_CHECK(back->trash.has_value() && *back->trash);
    PB_CHECK_EQ(back->max_chunk.value_or(-1), 104857600LL);
    PB_CHECK_EQ(back->forbidden_chars, c.forbidden_chars);
    PB_CHECK((back->forbidden_names == c.forbidden_names));
    PB_CHECK((back->forbidden_extensions == c.forbidden_extensions));
    PB_CHECK_EQ(back->checked_at.value_or(-1), 1790000000LL);

    pbc::ServerCaps other = caps_of("capabilities_minimal.unverified.json");
    PB_CHECK(s->save_server_caps(other).ok());
    back = s->server_caps();
    PB_CHECK(back.has_value() && back->trash.has_value() && !*back->trash);
    PB_CHECK(back->forbidden_names.empty() && back->forbidden_chars.empty() && !back->max_chunk.has_value());   // nothing of the old row is left
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM server_caps"), std::string("1"));

    // the same account again keeps them; another user or server drops them; so does signing out
    PB_CHECK(s->save_server_caps(c).ok());
    PB_CHECK(s->save_account(account("demo")).ok());
    PB_CHECK(s->server_caps().has_value());
    PB_CHECK(s->save_account(account("someone")).ok());
    PB_CHECK(!s->server_caps().has_value());
    PB_CHECK(s->save_server_caps(c).ok());
    PB_CHECK(s->clear_account().ok());
    PB_CHECK(!s->server_caps().has_value());
}

PB_TEST(the_account_check_returns_the_capabilities_it_read) {
    pbtest::FakeTransport http;
    const std::string body = pbtest::fixture("nextcloud/write/capabilities/capabilities_write.unverified.json");
    http.set_handler([body](const pbc::Request&) { return pbtest::FakeTransport::status(200, body, {{"Date", "Wed, 14 Oct 2026 12:00:00 GMT"}}); });
    const auto r = pbc::quick_check(http, "https://cloud.example.org", "demo", "app-pass-0001", g_clock);
    PB_CHECK(r.ok());
    PB_CHECK(r.value.caps.trash.has_value() && *r.value.caps.trash);
    PB_CHECK(pbc::chunked_available(r.value.caps));
    PB_CHECK_EQ(r.value.caps.forbidden_names.size(), size_t(3));
    // a server that says nothing about the write side leaves everything unknown
    http.set_handler([](const pbc::Request&) {
        return pbtest::FakeTransport::status(200, "{\"ocs\":{\"meta\":{\"statuscode\":100},\"data\":{\"version\":{\"string\":\"30.0.0\"},\"capabilities\":{}}}}");
    });
    const auto bare = pbc::quick_check(http, "https://cloud.example.org", "demo", "app-pass-0001", g_clock);
    PB_CHECK(bare.ok());
    PB_CHECK(!bare.value.caps.trash.has_value() && !pbc::delete_offered(bare.value.caps));
}

PB_TEST_MAIN()
