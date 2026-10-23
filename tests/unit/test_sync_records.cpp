// What "unchanged" means for the records of a pair (specs/003-folder-sync, research R-3): the reader side by size, time and - when the record kept it - the
// SHA-256, the server side by the ETag only, and the first-sync identity by equal size and checksum.
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <fstream>
#include <string>

#include <openssl/evp.h>

#include "pbc/hash.h"
#include "pbc/sync_scan.h"
#include "pbc/sync_types.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct NoCancel : pbc::Cancel {
    bool requested() const override { return false; }
};

std::string hex_of(const std::string& data, const EVP_MD* md) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), out, &len, md, nullptr);
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (unsigned int i = 0; i < len; ++i) {
        s.push_back(digits[out[i] >> 4]);
        s.push_back(digits[out[i] & 15]);
    }
    return s;
}

struct Env {
    pbtest::TmpDir tmp;
    std::string path;
    Env() { path = tmp.file("a.epub"); }
    void write(const std::string& data, int64_t mtime) {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << data;
        utimbuf t;
        t.actime = t.modtime = static_cast<time_t>(mtime);
        PB_CHECK(::utime(path.c_str(), &t) == 0);
    }
    pbc::SyncRecord record(const std::string& data, int64_t mtime, bool with_hash) const {
        pbc::SyncRecord r;
        r.rel = "a.epub";
        r.local_rel = "a.epub";
        r.origin = "download";
        r.local_size = static_cast<int64_t>(data.size());
        r.local_mtime = mtime;
        if (with_hash) r.local_sha256 = hex_of(data, EVP_sha256());
        return r;
    }
};

}  // namespace

PB_TEST(the_reader_file_is_unchanged_for_equal_size_and_time_with_the_resolution_of_fat) {
    Env e;
    e.write("hello", 1700000000);
    const auto rec = e.record("hello", 1700000000, false);
    PB_CHECK(!pbc::reader_changed(e.path, rec));
    pbc::SyncRecord one_off = rec;
    one_off.local_mtime = 1700000001;   // FAT keeps even seconds: one second apart is the same time
    PB_CHECK(!pbc::reader_changed(e.path, one_off));
    pbc::SyncRecord far = rec;
    far.local_mtime = 1700000010;
    PB_CHECK(pbc::reader_changed(e.path, far));
}

PB_TEST(a_different_size_a_link_a_folder_or_a_missing_file_counts_as_changed) {
    Env e;
    e.write("hello", 1700000000);
    pbc::SyncRecord rec = e.record("hello", 1700000000, true);
    rec.local_size = 6;
    PB_CHECK(pbc::reader_changed(e.path, rec));
    rec.local_size.reset();
    PB_CHECK(pbc::reader_changed(e.path, rec));   // nothing recorded: not known to be unchanged
    rec = e.record("hello", 1700000000, false);
    PB_CHECK(::symlink(e.path.c_str(), e.tmp.file("link").c_str()) == 0);
    PB_CHECK(pbc::reader_changed(e.tmp.file("link"), rec));
    PB_CHECK(::mkdir(e.tmp.file("folder").c_str(), 0700) == 0);
    PB_CHECK(pbc::reader_changed(e.tmp.file("folder"), rec));
    PB_CHECK(pbc::reader_changed(e.tmp.file("gone"), rec));
}

PB_TEST(a_time_only_difference_is_changed_without_a_stored_hash_and_unchanged_with_one_that_still_matches) {
    Env e;
    e.write("hello", 1700000000);
    const auto without = e.record("hello", 1600000000, false);   // the card shows another time than the record
    PB_CHECK(pbc::reader_changed(e.path, without));
    const auto with = e.record("hello", 1600000000, true);
    PB_CHECK(!pbc::reader_changed(e.path, with));                // same bytes: the time alone does not make it changed
    // same size and time as recorded but other bytes: the time is equal, the size too: it is unchanged by the rule (a hash is only a tie-break)
    e.write("hellO", 1700000000);
    PB_CHECK(!pbc::reader_changed(e.path, e.record("hello", 1700000000, true)));
    // a different time and a hash that no longer matches: changed
    e.write("hellO", 1700009999);
    PB_CHECK(pbc::reader_changed(e.path, e.record("hello", 1700000000, true)));
}

PB_TEST(the_hash_tie_break_can_be_cancelled) {
    Env e;
    e.write("hello", 1700000000);
    struct Stop : pbc::Cancel {
        bool requested() const override { return true; }
    } stop;
    // cancelled while hashing: the file is not known to be unchanged, so it counts as changed (the safe direction)
    PB_CHECK(pbc::reader_changed(e.path, e.record("hello", 1600000000, true), &stop));
}

PB_TEST(the_server_side_is_changed_exactly_when_the_etag_differs_or_is_missing) {
    pbc::SyncRecord rec;
    rec.server_etag = "\"abc\"";
    rec.server_size = 10;
    pbc::ScanEntry same;
    same.etag = "\"abc\"";
    same.size = 10;
    PB_CHECK(!pbc::server_changed(same, rec));
    pbc::ScanEntry other = same;
    other.etag = "\"abd\"";
    PB_CHECK(pbc::server_changed(other, rec));
    pbc::ScanEntry weak = same;
    weak.etag = "W/\"abc\"";
    PB_CHECK(pbc::server_changed(weak, rec));          // byte-wise, no interpretation
    pbc::ScanEntry none = same;
    none.etag.clear();
    PB_CHECK(pbc::server_changed(none, rec));          // cannot be compared: treated as changed
    // size and date never decide
    pbc::ScanEntry size_changed = same;
    size_changed.size = 99999;
    size_changed.mtime = 5;
    PB_CHECK(!pbc::server_changed(size_changed, rec));
}

PB_TEST(a_first_sync_match_needs_equal_size_and_an_equal_checksum_and_the_strongest_one_wins) {
    Env e;
    const std::string data = "the same bytes";
    e.write(data, 1700000000);
    NoCancel c;
    pbc::ScanEntry entry;
    entry.size = static_cast<int64_t>(data.size());
    entry.checksum = "SHA256:" + hex_of(data, EVP_sha256());
    PB_CHECK(pbc::identical_by_checksum(e.path, entry, c));
    entry.checksum = "SHA1:" + hex_of(data, EVP_sha1());
    PB_CHECK(pbc::identical_by_checksum(e.path, entry, c));
    entry.checksum = "MD5:" + hex_of(data, EVP_md5());
    PB_CHECK(pbc::identical_by_checksum(e.path, entry, c));
    // the strongest listed checksum decides: a matching SHA-1 beside a wrong SHA-256 is not a match
    entry.checksum = "SHA256:" + std::string(64, '0') + " SHA1:" + hex_of(data, EVP_sha1());
    PB_CHECK(!pbc::identical_by_checksum(e.path, entry, c));
    entry.checksum = "SHA256:" + hex_of(data, EVP_sha256()) + " SHA1:" + std::string(40, '0');
    PB_CHECK(pbc::identical_by_checksum(e.path, entry, c));
    // equal size without any checksum is not enough
    entry.checksum.clear();
    PB_CHECK(!pbc::identical_by_checksum(e.path, entry, c));
    entry.checksum = "ADLER32:01020304";   // an algorithm that is not used
    PB_CHECK(!pbc::identical_by_checksum(e.path, entry, c));
    // a different checksum
    entry.checksum = "SHA256:" + hex_of("other bytes!!!!", EVP_sha256());
    PB_CHECK(!pbc::identical_by_checksum(e.path, entry, c));
}

PB_TEST(a_different_size_is_decided_without_reading_the_file) {
    Env e;
    e.write("0123456789", 1700000000);
    NoCancel c;
    pbc::ScanEntry entry;
    entry.size = 11;
    entry.checksum = "SHA256:" + hex_of("0123456789", EVP_sha256());   // the checksum would match, the size does not
    PB_CHECK(!pbc::identical_by_checksum(e.path, entry, c));
    // proof that nothing was read: a file that cannot be read but has the right size still answers false for another size, and a missing one too
    PB_CHECK(!pbc::identical_by_checksum(e.tmp.file("missing"), entry, c));
    struct Stop : pbc::Cancel {
        bool requested() const override { return true; }
    } stop;
    entry.size = 10;
    PB_CHECK(!pbc::identical_by_checksum(e.path, entry, stop));   // cancelled while hashing: not known to be identical
}

PB_TEST(join_path_checked_refuses_everything_that_is_not_a_plain_relative_path) {
    PB_CHECK_EQ(pbc::join_path_checked("/mnt/ext1/Books", "a/b.epub"), std::string("/mnt/ext1/Books/a/b.epub"));
    PB_CHECK_EQ(pbc::join_path_checked("/mnt/ext1/Books/", "a.epub"), std::string("/mnt/ext1/Books/a.epub"));
    for (const char* bad : {"", "/etc/passwd", "../x", "a/../b", "a/./b", "a//b", "a/", "a\x01" "b", "."})
        PB_CHECK_EQ(pbc::join_path_checked("/mnt/ext1/Books", bad), std::string(""));
    PB_CHECK_EQ(pbc::join_path_checked("/mnt/ext1/Books", std::string(4001, 'a')), std::string(""));
    PB_CHECK_EQ(pbc::join_path_checked("", "a"), std::string(""));
}

PB_TEST_MAIN()
