// The tolerant parser of the trash list (specs/006-trash-and-versions, contracts/core-interfaces.md "trash_parse.h", research R-3):
// total and bounded on hostile input; an item whose name cannot be one path segment is listed as unreadable and can neither be
// restored nor deleted; a location that is unsafe or too long makes the original place unknown; the sort is made here.
#include <string>
#include <vector>

#include "fixtures.h"
#include "pbc/http_transport.h"
#include "pbc/trash_parse.h"
#include "test_support.h"

namespace {

const char* kUser = "demo";
const int64_t kNow = 1760100000;   // after every deletion time of the fixtures

std::string fx(const std::string& name) { return pbtest::fixture("nextcloud/trash/" + name); }

pbc::Response multistatus(const std::string& body) {
    pbc::Response r;
    r.status = 207;
    r.body = body;
    return r;
}

pbc::TrashList list_of(const std::string& body) {
    const auto r = pbc::parse_trash(multistatus(body), kUser, kNow);
    if (!r.ok()) PB_FAIL_AT("the list was refused: " + r.error.key + " " + r.error.detail);
    return r.value;
}

const pbc::TrashItem* find(const pbc::TrashList& l, const std::string& trash_name) {
    for (const auto& i : l.items)
        if (i.trash_name == trash_name) return &i;
    return nullptr;
}

std::string many_items(int n) {
    std::string b = "<?xml version=\"1.0\"?>\n<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\" xmlns:nc=\"http://nextcloud.org/ns\">"
                    "<d:response><d:href>/remote.php/dav/trashbin/demo/trash/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype>"
                    "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    for (int i = 0; i < n; ++i) {
        const std::string id = std::to_string(1000 + i);
        b += "<d:response><d:href>/remote.php/dav/trashbin/demo/trash/f" + id + ".d" + id + "</d:href><d:propstat><d:prop>"
             "<nc:trashbin-filename>f" + id + "</nc:trashbin-filename><nc:trashbin-original-location>Books/f" + id + "</nc:trashbin-original-location>"
             "<nc:trashbin-deletion-time>" + std::to_string(1700000000 + i) + "</nc:trashbin-deletion-time><d:getcontentlength>10</d:getcontentlength>"
             "<d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    }
    return b + "</d:multistatus>";
}

}  // namespace

PB_TEST(a_normal_trash_gives_one_item_per_member_newest_first) {
    const auto l = list_of(fx("trash_list_ok.unverified.xml"));
    PB_CHECK_EQ(l.items.size(), size_t(4));   // the trash collection itself is not an item
    PB_CHECK_EQ(l.total_seen, 4);
    PB_CHECK(!l.truncated && !l.partial);
    PB_CHECK_EQ(l.skipped_hostile, 0);
    const auto& a = l.items[0];
    PB_CHECK_EQ(a.trash_name, std::string("story.epub.d1760000000"));
    PB_CHECK_EQ(a.name, std::string("story.epub"));
    PB_CHECK_EQ(a.original_dir, std::string("/Books"));
    PB_CHECK_EQ(a.original_name, std::string("story.epub"));
    PB_CHECK(a.original_known && !a.unreadable);
    PB_CHECK(a.deleted_at == std::optional<int64_t>(1760000000));
    PB_CHECK(a.kind == pbc::Kind::File && a.size == std::optional<int64_t>(1234));
    PB_CHECK(a.availability == pbc::Availability::Ok);
    const auto& f = l.items[1];
    PB_CHECK_EQ(f.trash_name, std::string("Old Notes.d1759000000"));   // decoded once
    PB_CHECK_EQ(f.name, std::string("Old Notes"));
    PB_CHECK(f.kind == pbc::Kind::Folder && !f.size.has_value());
    PB_CHECK_EQ(f.original_dir, std::string("/Documents"));
    PB_CHECK_EQ(f.original_name, std::string("Old Notes"));
    // two files of the same name stay two items, each with its own deletion time
    PB_CHECK_EQ(l.items[2].trash_name, std::string("report.pdf.d1758000000"));
    PB_CHECK_EQ(l.items[3].trash_name, std::string("report.pdf.d1757000000"));
    PB_CHECK_EQ(l.items[2].name, l.items[3].name);
}

PB_TEST(an_empty_trash_gives_an_empty_list) {
    const auto l = list_of(fx("trash_list_empty.unverified.xml"));
    PB_CHECK(l.items.empty());
    PB_CHECK_EQ(l.total_seen, 0);
    PB_CHECK(!l.truncated);
}

PB_TEST(the_request_body_asks_for_the_trash_properties) {
    const std::string b = pbc::trash_request_body();
    for (const char* p : {"propfind", "trashbin-filename", "trashbin-original-location", "trashbin-deletion-time", "getcontentlength", "resourcetype",
                          "fileid", "permissions", "is-encrypted"})
        PB_CHECK(b.find(p) != std::string::npos);
}

PB_TEST(a_name_that_cannot_be_one_segment_is_listed_as_unreadable) {
    const auto l = list_of(fx("trash_list_hostile.unverified.xml"));
    // 15 listed items: the 17 members minus the failed one and the foreign one, and the duplicate counted once
    PB_CHECK_EQ(l.items.size(), size_t(15));
    int unreadable = 0;
    for (const auto& i : l.items) {
        if (!i.unreadable) continue;
        ++unreadable;
        PB_CHECK(i.trash_name.empty());   // an unsafe name never becomes an address
        PB_CHECK(!i.original_known);
        PB_CHECK(i.availability == pbc::Availability::Ok || i.availability == pbc::Availability::Unreadable);
    }
    PB_CHECK_EQ(unreadable, 4);   // "../evil", "a/b", a control character, 303 bytes
    PB_CHECK_EQ(l.skipped_hostile, 2);   // the foreign account and the second copy of one address
    PB_CHECK(l.partial);                 // the member with the 403 failed
}

PB_TEST(an_unsafe_or_overlong_location_makes_the_original_place_unknown) {
    const auto l = list_of(fx("trash_list_hostile.unverified.xml"));
    for (const char* name : {"dots.d5", "gap.d6", "big.d7"}) {
        const auto* i = find(l, name);
        PB_CHECK(i != nullptr);
        PB_CHECK(!i->unreadable);            // restorable, but only to a chosen place
        PB_CHECK(!i->original_known);
        PB_CHECK(i->original_dir.empty() && i->original_name.empty());
    }
}

PB_TEST(a_deletion_time_outside_the_range_is_unknown_and_sorts_last) {
    const auto l = list_of(fx("trash_list_hostile.unverified.xml"));
    for (const char* name : {"negative.d8", "huge.d9", "missing.d10"}) {
        const auto* i = find(l, name);
        PB_CHECK(i != nullptr);
        PB_CHECK(!i->deleted_at.has_value());
    }
    // the sort: deletion time descending, unknown last, then the trash name
    bool seen_unknown = false;
    for (size_t k = 0; k < l.items.size(); ++k) {
        if (!l.items[k].deleted_at) {
            seen_unknown = true;
            continue;
        }
        PB_CHECK(!seen_unknown);   // no known time after an unknown one
        if (k > 0 && l.items[k - 1].deleted_at) {
            PB_CHECK(*l.items[k - 1].deleted_at >= *l.items[k].deleted_at);
            if (*l.items[k - 1].deleted_at == *l.items[k].deleted_at) PB_CHECK(l.items[k - 1].trash_name <= l.items[k].trash_name);
        }
    }
}

PB_TEST(the_size_is_a_non_negative_integer_for_files_only) {
    const auto l = list_of(fx("trash_list_hostile.unverified.xml"));
    PB_CHECK(!find(l, "badsize.d17")->size.has_value());
    PB_CHECK(!find(l, "crypt.d15")->size.has_value());   // a folder has no size here
}

PB_TEST(availability_comes_from_encryption_permissions_and_type_never_from_a_missing_property) {
    const auto l = list_of(fx("trash_list_hostile.unverified.xml"));
    PB_CHECK(find(l, "crypt.d15")->availability == pbc::Availability::Encrypted);
    PB_CHECK(find(l, "locked.d14")->availability == pbc::Availability::NoPermission);   // "G" without "D"
    PB_CHECK(find(l, "weird.d16")->availability == pbc::Availability::Unsupported);     // a resource type that is neither empty nor a collection
    PB_CHECK(find(l, "dots.d5")->availability == pbc::Availability::Ok);                // no permissions property at all
    const auto ok = list_of(fx("trash_list_ok.unverified.xml"));
    for (const auto& i : ok.items) PB_CHECK(i.availability == pbc::Availability::Ok);   // "GD"
}

PB_TEST(a_member_that_failed_is_skipped_and_the_rest_is_intact) {
    const auto l = list_of(fx("trash_list_hostile.unverified.xml"));
    PB_CHECK(find(l, "forbidden.d11") == nullptr);
    PB_CHECK(find(l, "foreign.d12") == nullptr);
    PB_CHECK(find(l, "dup.d13") != nullptr);
    int dup = 0;
    for (const auto& i : l.items)
        if (i.trash_name == "dup.d13") ++dup;
    PB_CHECK_EQ(dup, 1);
}

PB_TEST(more_than_the_limit_keeps_the_newest_and_says_so) {
    const auto l = list_of(many_items(1200));
    PB_CHECK_EQ(l.items.size(), pbc::kMaxTrashRows);
    PB_CHECK_EQ(l.total_seen, 1200);
    PB_CHECK(l.truncated);
    PB_CHECK_EQ(l.items.front().trash_name, std::string("f2199.d2199"));   // the newest deletion is kept first
    PB_CHECK(l.items.front().deleted_at == std::optional<int64_t>(1700000000 + 1199));
    PB_CHECK(l.items.back().deleted_at == std::optional<int64_t>(1700000000 + 200));   // the 200 oldest are cut
    const auto exact = list_of(many_items(1000));
    PB_CHECK_EQ(exact.items.size(), size_t(1000));
    PB_CHECK(!exact.truncated);
}

PB_TEST(an_answer_above_the_limit_is_refused) {
    pbc::Response r;
    r.status = 207;
    r.body_too_large = true;
    PB_CHECK_EQ(pbc::parse_trash(r, kUser, kNow).error.key, std::string("listing_too_large"));
    pbc::Response big = multistatus(std::string(pbc::kMaxTrashBody + 1, ' '));
    PB_CHECK_EQ(pbc::parse_trash(big, kUser, kNow).error.key, std::string("listing_too_large"));
}

PB_TEST(a_broken_answer_gives_a_key_and_never_a_crash) {
    PB_CHECK_EQ(pbc::parse_trash(multistatus("<d:multistatus xmlns:d=\"DAV:\"><d:response>"), kUser, kNow).error.key, std::string("malformed_xml"));
    PB_CHECK_EQ(pbc::parse_trash(multistatus(""), kUser, kNow).error.key, std::string("malformed_xml"));
    pbc::Response not207;
    not207.status = 200;
    not207.body = fx("trash_list_ok.unverified.xml");
    PB_CHECK_EQ(pbc::parse_trash(not207, kUser, kNow).error.key, std::string("protocol_error"));
    pbc::Response wrong_root = multistatus("<a xmlns=\"DAV:\"/>");
    PB_CHECK_EQ(pbc::parse_trash(wrong_root, kUser, kNow).error.key, std::string("malformed_xml"));
}

PB_TEST(mutated_copies_of_the_fixtures_never_crash_the_parser) {
    for (const char* name : {"trash_list_ok.unverified.xml", "trash_list_hostile.unverified.xml"}) {
        const std::string good = fx(name);
        for (size_t cut = 1; cut < good.size(); cut += 37) {   // truncated everywhere
            const auto r = pbc::parse_trash(multistatus(good.substr(0, cut)), kUser, kNow);
            if (r.ok()) PB_CHECK(r.value.items.size() <= pbc::kMaxTrashRows);
        }
        for (size_t at = 0; at < good.size(); at += 53) {      // one byte changed everywhere
            std::string bad = good;
            bad[at] = static_cast<char>(bad[at] ^ 0x5a);
            const auto r = pbc::parse_trash(multistatus(bad), kUser, kNow);
            if (r.ok())
                for (const auto& i : r.value.items) PB_CHECK(i.unreadable || pbc::is_safe_segment(i.trash_name));
        }
    }
}

PB_TEST(a_safe_segment_is_one_path_segment_of_valid_text) {
    PB_CHECK(pbc::is_safe_segment("story.epub.d1760000000"));
    PB_CHECK(pbc::is_safe_segment("Old Notes.d1"));
    PB_CHECK(pbc::is_safe_segment(std::string(255, 'a')));
    PB_CHECK(!pbc::is_safe_segment(std::string(256, 'a')));
    for (const char* bad : {"", ".", "..", "a/b", "a\\b", "..\\x"}) PB_CHECK(!pbc::is_safe_segment(bad));
    PB_CHECK(!pbc::is_safe_segment(std::string("a") + '\x01' + "b"));
    PB_CHECK(!pbc::is_safe_segment(std::string("a") + '\x7f'));
    PB_CHECK(!pbc::is_safe_segment(std::string("a") + '\0' + "b"));
    PB_CHECK(!pbc::is_safe_segment("\xff\xfe"));   // invalid UTF-8
    PB_CHECK(pbc::is_safe_segment("\xc3\xa4.txt"));
}

PB_TEST(a_display_name_loses_control_characters_and_is_cut_on_a_character_boundary) {
    PB_CHECK_EQ(pbc::display_name(std::string("a") + '\x01' + "b\nc"), std::string("abc"));
    std::string two_byte;
    for (int i = 0; i < 200; ++i) two_byte += "\xc3\xa9";   // 400 bytes
    const std::string cut = pbc::display_name(two_byte);
    PB_CHECK(cut.size() <= 255);
    PB_CHECK_EQ(cut.size(), size_t(254));   // never half a character
    PB_CHECK_EQ(pbc::display_name("plain"), std::string("plain"));
}

PB_TEST(an_original_location_is_split_into_folder_and_name_or_unknown) {
    auto p = pbc::parse_original_location("Books/story.epub");
    PB_CHECK(p.known && p.dir == "/Books" && p.name == "story.epub");
    p = pbc::parse_original_location("story.epub");
    PB_CHECK(p.known && p.dir == "/" && p.name == "story.epub");
    p = pbc::parse_original_location("/A/B/C.txt");
    PB_CHECK(p.known && p.dir == "/A/B" && p.name == "C.txt");
    for (const char* bad : {"", "/", "Books/", "Books/../x", "../x", "A//B", "./x", "A/./b"}) PB_CHECK(!pbc::parse_original_location(bad).known);
    PB_CHECK(!pbc::parse_original_location(std::string("A/") + '\x02' + "b").known);
    PB_CHECK(!pbc::parse_original_location(std::string("a/") + std::string(pbc::kMaxLocationBytes, 'x')).known);
    PB_CHECK(!pbc::parse_original_location("A/\xff").known);   // invalid UTF-8
    auto q = pbc::parse_original_location(std::string(255, 'y'));
    PB_CHECK(q.known && q.dir == "/" && q.name.size() == 255);   // a name of exactly 255 bytes
    PB_CHECK(!pbc::parse_original_location(std::string(256, 'y')).known);   // one byte more is not a name
    std::string five;   // every segment is fine, the whole is above the limit of 1,024 bytes
    for (int i = 0; i < 5; ++i) five += (i ? "/" : "") + std::string(250, 'z');
    PB_CHECK(five.size() > pbc::kMaxLocationBytes);
    PB_CHECK(!pbc::parse_original_location(five).known);
}

PB_TEST_MAIN()
