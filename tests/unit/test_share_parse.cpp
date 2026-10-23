// The tolerant parsers of the share answers (specs/004-share-links, contracts/core-interfaces.md "share_parse.h", research R-5, R-6): total and
// bounded on hostile input, and a list can never carry a link (the row type has no member for one).
#include <string>
#include <vector>

#include "fixtures.h"
#include "pbc/share_parse.h"
#include "test_support.h"

namespace {

const char* kMe = "alice";
const char* kHost = "cloud.example.org";

std::string fx(const std::string& name) { return pbtest::fixture("nextcloud/shares/" + name); }

pbc::ShareList list_of(const std::string& body, bool inherited = false, bool folder = true) {
    const auto r = pbc::parse_share_list(body, kMe, inherited, folder);
    if (!r.ok()) PB_FAIL_AT("the list was refused: " + r.error.key + " " + r.error.detail);
    return r.value;
}

// every string a ShareRow holds, in one text, so that a test can prove what is absent
std::string everything(const pbc::ShareList& l) {
    std::string all;
    for (const auto& r : l.rows) {
        all += r.label + "|" + r.with_name + "|" + r.expires.value_or("") + "|" + std::to_string(r.id) + "|";
    }
    return all;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string row_json(int id, const std::string& extra = "") {
    return "{\"id\":\"" + std::to_string(id) + "\",\"share_type\":3,\"permissions\":1,\"uid_owner\":\"alice\",\"item_type\":\"folder\","
           "\"token\":\"TOK" + std::to_string(id) + "\",\"url\":\"https://cloud.example.org/s/TOK" + std::to_string(id) + "\"" + extra + "}";
}

std::string envelope(const std::string& data) { return "{\"ocs\":{\"meta\":{\"statuscode\":100},\"data\":" + data + "}}"; }

pbc::ShareDetail detail_of(const std::string& url_member, const char* host = kHost, bool folder = true) {
    const auto r = pbc::parse_share_detail(envelope("[{\"id\":\"9\",\"share_type\":3,\"permissions\":1,\"uid_owner\":\"alice\"" + url_member + "}]"), kMe, host, folder);
    if (!r.ok()) PB_FAIL_AT("the detail was refused: " + r.error.key);
    return r.value;
}

}  // namespace

PB_TEST(a_mixed_list_gives_kinds_permissions_expiry_owner_and_password_state) {
    const auto l = list_of(fx("lists/mixed.unverified.json"));
    PB_CHECK_EQ(l.rows.size(), size_t(6));
    PB_CHECK_EQ(l.dropped, 0);
    PB_CHECK(!l.truncated);
    const auto& a = l.rows[0];
    PB_CHECK_EQ(a.id, int64_t(11));
    PB_CHECK(a.kind == pbc::ShareKind::PublicLink && a.permission == pbc::LinkPermission::ReadOnly);
    PB_CHECK_EQ(a.permission_bits, 1);
    PB_CHECK(a.expires == std::optional<std::string>("2026-12-31"));
    PB_CHECK(!a.has_password && a.owned_by_me && !a.inherited && !a.limited);
    PB_CHECK_EQ(a.label, std::string("For Bob"));
    const auto& b = l.rows[1];
    PB_CHECK(b.permission == pbc::LinkPermission::ReadUpload && b.has_password && !b.expires.has_value());
    const auto& u = l.rows[2];
    PB_CHECK(u.kind == pbc::ShareKind::User && u.with_name == "Bob Example" && u.permission == pbc::LinkPermission::Other);   // 19 = read+update+reshare
    const auto& g = l.rows[3];
    PB_CHECK(g.kind == pbc::ShareKind::Group && g.with_name == "Team Example" && g.permission == pbc::LinkPermission::ReadOnly);
    PB_CHECK(g.expires == std::optional<std::string>("2027-01-15"));
    const auto& o = l.rows[4];
    PB_CHECK(o.kind == pbc::ShareKind::Other && !o.owned_by_me && o.label.empty() && o.with_name.empty());
    const auto& c = l.rows[5];
    PB_CHECK(c.kind == pbc::ShareKind::PublicLink && !c.owned_by_me && c.label == "Carol's link");
}

PB_TEST(a_list_never_holds_a_link_a_token_or_a_password_hash) {
    const auto l = list_of(fx("lists/mixed.unverified.json"));
    const std::string all = everything(l);
    for (const char* needle : {"TOKEN0011", "TOKEN0012", "TOKEN0016", "MAILTOKEN15", "https://", "cloud.example.org", "/s/", "$2y$", "examplehash"})
        if (contains(all, needle)) PB_FAIL_AT(std::string("a list row holds: ") + needle);
    // and the type itself has no member that could hold one (checked here by size: nothing but the declared fields)
    static_assert(sizeof(pbc::ShareRow) < 200, "ShareRow must stay a small value without a buffer for a link");
}

PB_TEST(inherited_rows_are_marked) {
    const auto l = list_of(fx("lists/inherited.unverified.json"), true);
    PB_CHECK_EQ(l.rows.size(), size_t(1));
    PB_CHECK(l.rows[0].inherited && l.rows[0].owned_by_me);
    PB_CHECK(!list_of(fx("lists/inherited.unverified.json"), false).rows[0].inherited);
}

PB_TEST(an_empty_list_is_fine) {
    const auto l = list_of(fx("lists/empty.unverified.json"));
    PB_CHECK(l.rows.empty() && l.dropped == 0 && !l.truncated);
}

PB_TEST(rows_without_a_positive_integer_id_are_dropped_and_counted) {
    const auto l = list_of(fx("hostile/wrong-types.unverified.json"));
    PB_CHECK_EQ(l.rows.size(), size_t(2));
    PB_CHECK_EQ(l.dropped, 7);   // object id, -5, 0, "abc", a string, a number, null
    PB_CHECK_EQ(l.rows[0].id, int64_t(41));
    PB_CHECK(l.rows[0].limited && l.rows[0].kind == pbc::ShareKind::Other);   // share_type and permissions are the wrong type
    PB_CHECK(l.rows[0].expires_unreadable);                                    // expiration is a number
    PB_CHECK_EQ(l.rows[1].id, int64_t(42));
    PB_CHECK(!l.rows[1].limited && l.rows[1].permission == pbc::LinkPermission::ReadOnly && l.rows[1].label == "ok");
}

PB_TEST(a_duplicate_id_keeps_the_first) {
    const auto l = list_of(fx("hostile/duplicate-ids.unverified.json"));
    PB_CHECK_EQ(l.rows.size(), size_t(2));
    PB_CHECK_EQ(l.dropped, 1);
    PB_CHECK(l.rows[0].id == 61 && l.rows[0].label.empty() && l.rows[0].permission == pbc::LinkPermission::ReadOnly);
    PB_CHECK_EQ(l.rows[1].id, int64_t(62));
}

PB_TEST(labels_and_names_lose_control_and_bidi_characters) {
    const auto l = list_of(fx("hostile/control-chars.unverified.json"));
    PB_CHECK_EQ(l.rows.size(), size_t(2));
    PB_CHECK_EQ(l.rows[0].label, std::string("BadLabelevil"));
    PB_CHECK_EQ(l.rows[1].with_name, std::string("GroupX"));
}

PB_TEST(labels_are_cut_at_64_bytes_never_inside_a_character) {
    std::string label;
    for (int i = 0; i < 40; ++i) label += "\xc3\xa4";   // 80 bytes of 'ä'
    const auto l = list_of(envelope("[" + row_json(5, ",\"label\":\"" + label + "\"") + "]"));
    PB_CHECK_EQ(l.rows.size(), size_t(1));
    PB_CHECK(l.rows[0].label.size() <= 64);
    PB_CHECK_EQ(l.rows[0].label.size() % 2, size_t(0));   // two bytes per character: no split character
    PB_CHECK(l.rows[0].label.size() >= 62);
}

PB_TEST(invalid_utf8_in_a_label_is_dropped) {
    const auto l = list_of(envelope("[" + row_json(6, ",\"label\":\"ab\xff\xfe" "cd\"") + "]"));
    PB_CHECK_EQ(l.rows[0].label, std::string("abcd"));
}

PB_TEST(rows_that_lack_the_fields_to_show_them_are_limited) {
    const auto l = list_of(fx("hostile/limited.unverified.json"));
    PB_CHECK_EQ(l.rows.size(), size_t(3));
    for (const auto& r : l.rows) PB_CHECK(r.limited);
    PB_CHECK(l.rows[0].kind == pbc::ShareKind::Other && l.rows[0].permission_bits == 0 && l.rows[0].owned_by_me);
    PB_CHECK(l.rows[1].kind == pbc::ShareKind::PublicLink);
    PB_CHECK(l.rows[2].kind == pbc::ShareKind::Other && l.rows[2].permission_bits == 1);
}

PB_TEST(a_malformed_expiration_is_unreadable_not_a_date) {
    const auto l = list_of(envelope("[" + row_json(7, ",\"expiration\":\"soon\"") + "," + row_json(8, ",\"expiration\":\"2026-02-30 00:00:00\"") + "," +
                                    row_json(9, ",\"expiration\":\"\"") + "]"));
    PB_CHECK(l.rows[0].expires_unreadable && !l.rows[0].expires.has_value());
    PB_CHECK(l.rows[1].expires_unreadable && !l.rows[1].expires.has_value());
    PB_CHECK(!l.rows[2].expires_unreadable && !l.rows[2].expires.has_value());   // empty = no expiry
}

PB_TEST(a_password_state_comes_from_password_or_share_with_of_a_link) {
    const auto l = list_of(envelope("[" + row_json(10, ",\"password\":\"x\"") + "," + row_json(11, ",\"share_with\":\"hash\"") + "," +
                                    row_json(12, ",\"password\":null,\"share_with\":null") + "," + row_json(13, ",\"password\":\"\"") + "]"));
    PB_CHECK(l.rows[0].has_password && l.rows[1].has_password && !l.rows[2].has_password && !l.rows[3].has_password);
    // share_with of a user share is a user id, not a password
    const auto u = list_of(envelope("[{\"id\":\"1\",\"share_type\":0,\"permissions\":1,\"share_with\":\"bob\",\"uid_owner\":\"alice\"}]"));
    PB_CHECK(!u.rows[0].has_password && u.rows[0].with_name == "bob");
}

PB_TEST(a_permission_of_a_file_link_is_read_only_or_other) {
    const auto l = list_of(envelope("[" + row_json(14, ",\"item_type\":\"file\"") + "]"), false, true);
    PB_CHECK(l.rows[0].permission == pbc::LinkPermission::ReadOnly);
    const std::string edit = "{\"id\":\"15\",\"share_type\":3,\"permissions\":15,\"item_type\":\"file\",\"uid_owner\":\"alice\"}";
    PB_CHECK(list_of(envelope("[" + edit + "]"), false, true).rows[0].permission == pbc::LinkPermission::Other);   // a file never offers upload
    const std::string drop = "{\"id\":\"16\",\"share_type\":3,\"permissions\":4,\"item_type\":\"folder\",\"uid_owner\":\"alice\"}";
    PB_CHECK(list_of(envelope("[" + drop + "]"), false, false).rows[0].permission == pbc::LinkPermission::UploadOnly);
}

PB_TEST(more_than_500_rows_keep_500_and_say_so) {
    std::string rows = "[";
    for (int i = 1; i <= 600; ++i) rows += (i > 1 ? "," : "") + row_json(i);
    rows += "]";
    const auto l = list_of(envelope(rows));
    PB_CHECK_EQ(l.rows.size(), size_t(500));
    PB_CHECK(l.truncated);
    PB_CHECK(!contains(everything(l), "TOK"));
}

PB_TEST(hostile_bodies_give_protocol_error_or_a_bounded_result) {
    const std::string good = fx("lists/mixed.unverified.json");
    std::vector<std::pair<std::string, std::string>> bad = {
        {"empty", ""},
        {"truncated", fx("hostile/truncated.unverified.json")},
        {"data is an object", fx("hostile/data-not-an-array.unverified.json")},
        {"no data", fx("hostile/no-data.unverified.json")},
        {"top level array", fx("hostile/top-level-array.unverified.json")},
        {"over 2 MiB", std::string(3u << 20, ' ') + good},
        {"NUL byte", std::string("{\"ocs\":{\"data\":[]}") + std::string(1, '\0') + "}"},
        {"not json", "<html>nope</html>"},
    };
    std::string deep = "[";
    for (int i = 0; i < 40; ++i) deep += "[";
    for (int i = 0; i < 41; ++i) deep += "]";
    bad.push_back({"depth 40", envelope(deep)});
    std::string many = "[";
    for (int i = 0; i < 50000; ++i) many += (i ? ",1" : "1");
    many += "]";
    bad.push_back({"50,000 nodes", envelope(many)});
    for (const auto& [what, body] : bad) {
        const auto r = pbc::parse_share_list(body, kMe, false, true);
        if (r.ok()) PB_FAIL_AT("accepted: " + what);
        PB_CHECK_EQ(r.error.key, std::string("protocol_error"));
        if (what == "data is an object") continue;   // the detail parser takes one object as the share (a list must be an array)
        const auto d = pbc::parse_share_detail(body, kMe, kHost, true);
        if (d.ok()) PB_FAIL_AT("detail accepted: " + what);
    }
}

PB_TEST(a_detail_gives_the_link_of_the_answer) {
    const auto d = pbc::parse_share_detail(fx("details/link-readonly-file.unverified.json"), kMe, kHost, false);
    PB_CHECK(d.ok());
    PB_CHECK_EQ(d.value.link, std::string("https://cloud.example.org/s/TOKEN0031"));
    PB_CHECK(d.value.link_state == pbc::LinkState::Ok);
    PB_CHECK(d.value.row.permission == pbc::LinkPermission::ReadOnly && d.value.row.label == "Readme" && d.value.row.owned_by_me);
    const auto f = pbc::parse_share_detail(fx("details/link-folder-password.unverified.json"), kMe, kHost, true);
    PB_CHECK(f.ok() && f.value.row.permission == pbc::LinkPermission::ReadUpload && f.value.row.has_password);
    PB_CHECK(f.value.row.expires == std::optional<std::string>("2026-11-30"));
    PB_CHECK(f.value.link_state == pbc::LinkState::Ok);
    const auto o = pbc::parse_share_detail(fx("details/link-as-object.unverified.json"), kMe, kHost, false);   // data is one object
    PB_CHECK(o.ok() && o.value.link == "https://cloud.example.org/s/TOKEN0033");
}

PB_TEST(the_password_hash_of_a_detail_is_never_copied) {
    const auto f = pbc::parse_share_detail(fx("details/link-folder-password.unverified.json"), kMe, kHost, true);
    PB_CHECK(f.ok());
    const auto& r = f.value.row;
    const std::string all = r.label + r.with_name + r.expires.value_or("") + f.value.link;
    PB_CHECK(!contains(all, "$2y$") && !contains(all, "examplehash"));
}

PB_TEST(a_link_on_another_host_is_flagged_not_hidden) {
    const auto d = pbc::parse_share_detail(fx("details/link-readonly-file.unverified.json"), kMe, "other.example.org", false);
    PB_CHECK(d.ok());
    PB_CHECK(d.value.link_state == pbc::LinkState::DifferentAddress);
    PB_CHECK_EQ(d.value.link, std::string("https://cloud.example.org/s/TOKEN0031"));
    // the host is compared without regard to case and without the port
    PB_CHECK(detail_of(",\"url\":\"https://CLOUD.example.org:8443/s/abc\"").link_state == pbc::LinkState::Ok);
    PB_CHECK(detail_of(",\"url\":\"https://cloud.example.org.evil.example.net/s/abc\"").link_state == pbc::LinkState::DifferentAddress);
    PB_CHECK(detail_of(",\"url\":\"https://cloud.example.org/s/abc\"", "").link_state == pbc::LinkState::DifferentAddress);   // no connected host known
}

PB_TEST(an_http_link_is_not_secure_but_shown) {
    const auto d = detail_of(",\"url\":\"http://cloud.example.org/s/abc\"");
    PB_CHECK(d.link_state == pbc::LinkState::NotSecure);
    PB_CHECK_EQ(d.link, std::string("http://cloud.example.org/s/abc"));
}

PB_TEST(unusable_links_are_unreadable_and_empty) {
    std::vector<std::string> bad = {
        ",\"url\":\"https://cloud.example.org/s/a b\"",          // a blank
        ",\"url\":\"https://cloud.example.org/s/a\\u0001b\"",     // a control character
        ",\"url\":\"https://cloud.example.org/s/\xc3\xa4\"",       // non-ASCII
        ",\"url\":\"https://user@cloud.example.org/s/x\"",        // user information
        ",\"url\":\"javascript:alert(1)\"",
        ",\"url\":\"ftp://cloud.example.org/s/x\"",
        ",\"url\":\"\"",
        ",\"url\":null",
        ",\"url\":42",
        "",                                                       // no url at all
        ",\"url\":\"https://\"",
        ",\"url\":\"https://cloud.example.org/s/" + std::string(600, 'a') + "\"",   // too long
    };
    for (const auto& u : bad) {
        const auto d = detail_of(u);
        if (d.link_state != pbc::LinkState::Unreadable || !d.link.empty()) PB_FAIL_AT("shown although unusable: " + u.substr(0, 60));
    }
}

PB_TEST(a_link_of_exactly_512_bytes_is_shown_and_513_is_not) {
    const std::string prefix = "https://cloud.example.org/s/";
    const auto ok = detail_of(",\"url\":\"" + prefix + std::string(512 - prefix.size(), 'a') + "\"");
    PB_CHECK(ok.link_state == pbc::LinkState::Ok && ok.link.size() == 512);
    const auto too_long = detail_of(",\"url\":\"" + prefix + std::string(513 - prefix.size(), 'a') + "\"");
    PB_CHECK(too_long.link_state == pbc::LinkState::Unreadable && too_long.link.empty());
}

PB_TEST(a_detail_of_a_user_share_has_no_link) {
    const auto r = pbc::parse_share_detail(envelope("[{\"id\":\"3\",\"share_type\":0,\"permissions\":1,\"share_with\":\"bob\",\"url\":\"https://cloud.example.org/s/x\"}]"), kMe, kHost, true);
    PB_CHECK(r.ok());
    PB_CHECK(r.value.link.empty() && r.value.link_state == pbc::LinkState::Unreadable);
}

PB_TEST(an_empty_detail_array_is_a_protocol_error) {
    const auto r = pbc::parse_share_detail(envelope("[]"), kMe, kHost, true);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("protocol_error"));
}

PB_TEST(the_ocs_status_is_the_integer_and_never_the_message) {
    const auto ok = pbc::parse_ocs_status("{\"ocs\":{\"meta\":{\"status\":\"ok\",\"statuscode\":100,\"message\":\"https://leak.example.org/s/x\"},\"data\":[]}}");
    PB_CHECK(ok.ok() && ok.value.ok && ok.value.statuscode == 100);
    PB_CHECK(pbc::parse_ocs_status("{\"ocs\":{\"meta\":{\"statuscode\":200},\"data\":[]}}").value.ok);
    const auto bad = pbc::parse_ocs_status("{\"ocs\":{\"meta\":{\"statuscode\":403,\"message\":\"Password is required\"},\"data\":[]}}");
    PB_CHECK(bad.ok() && !bad.value.ok && bad.value.statuscode == 403);
    for (const char* broken : {"", "{}", "{\"ocs\":{}}", "{\"ocs\":{\"meta\":{}}}", "{\"ocs\":{\"meta\":{\"statuscode\":\"100\"}}}", "[1]", "{\"ocs\":{\"meta\":{\"statuscode\":1.5}}}"})
        PB_CHECK_EQ(pbc::parse_ocs_status(broken).error.key, std::string("protocol_error"));
}

PB_TEST_MAIN()
