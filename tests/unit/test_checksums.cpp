// The checksums a server lists and the check of a finished download (research R-8 point 5).
#include <fstream>
#include <string>

#include "pbc/hash.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct NeverCancel : pbc::Cancel {
    bool requested() const override { return false; }
};
struct CancelAfter : pbc::Cancel {
    mutable int calls = 0;
    int after;
    explicit CancelAfter(int n) : after(n) {}
    bool requested() const override { return ++calls > after; }
};

std::string hash_of(const pbtest::TmpDir& d, const std::string& content, pbc::HashAlgo algo) {
    d.write("f", content);
    NeverCancel c;
    const auto r = pbc::hash_file(d.file("f"), algo, c);
    return r.ok() ? r.value : "ERROR:" + r.error.key;
}

}  // namespace

PB_TEST(the_listed_checksums_are_read_and_unknown_or_broken_ones_ignored) {
    const auto v = pbc::parse_checksums("SHA1:a9993e364706816aba3e25717850c26c9cd0d89d MD5:900150983cd24fb0d6963f7d28e17f72 ADLER32:024d0127");
    PB_CHECK_EQ(v.size(), size_t(2));
    PB_CHECK(v[0].algo == pbc::HashAlgo::Sha1);
    PB_CHECK_EQ(v[0].hex, std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    PB_CHECK(v[1].algo == pbc::HashAlgo::Md5);

    const auto upper = pbc::parse_checksums("sha256:BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    PB_CHECK_EQ(upper.size(), size_t(1));
    PB_CHECK(upper[0].algo == pbc::HashAlgo::Sha256);
    PB_CHECK_EQ(upper[0].hex, std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));   // stored in lower case

    PB_CHECK(pbc::parse_checksums("").empty());
    PB_CHECK(pbc::parse_checksums("SHA1:zz MD5:12 SHA256:abc").empty());                              // wrong length or not hex
    PB_CHECK(pbc::parse_checksums("SHA1:a9993e364706816aba3e25717850c26c9cd0d89dXX").empty());
    PB_CHECK(pbc::parse_checksums("MD5 900150983cd24fb0d6963f7d28e17f72").empty());                    // no colon
    PB_CHECK(pbc::parse_checksums("CRC32:deadbeef FOO:bar :: SHA1:").empty());
    PB_CHECK_EQ(pbc::parse_checksums("MD5:900150983cd24fb0d6963f7d28e17f72,SHA1:a9993e364706816aba3e25717850c26c9cd0d89d").size(), size_t(2));   // a comma separates too
}

PB_TEST(the_strongest_available_checksum_is_chosen) {
    auto best = pbc::strongest(pbc::parse_checksums("MD5:900150983cd24fb0d6963f7d28e17f72 SHA1:a9993e364706816aba3e25717850c26c9cd0d89d"));
    PB_CHECK(best.has_value());
    PB_CHECK(best->algo == pbc::HashAlgo::Sha1);
    best = pbc::strongest(pbc::parse_checksums("SHA1:a9993e364706816aba3e25717850c26c9cd0d89d SHA256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    PB_CHECK(best->algo == pbc::HashAlgo::Sha256);
    PB_CHECK(!pbc::strongest({}).has_value());
}

PB_TEST(file_digests_match_the_known_values) {
    pbtest::TmpDir d;
    PB_CHECK_EQ(hash_of(d, "abc", pbc::HashAlgo::Sha256), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    PB_CHECK_EQ(hash_of(d, "abc", pbc::HashAlgo::Sha1), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    PB_CHECK_EQ(hash_of(d, "abc", pbc::HashAlgo::Md5), std::string("900150983cd24fb0d6963f7d28e17f72"));
    PB_CHECK_EQ(hash_of(d, "", pbc::HashAlgo::Sha256), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    PB_CHECK_EQ(hash_of(d, "", pbc::HashAlgo::Sha1), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    PB_CHECK_EQ(hash_of(d, "", pbc::HashAlgo::Md5), std::string("d41d8cd98f00b204e9800998ecf8427e"));
}

PB_TEST(a_file_of_5_mib_is_read_in_pieces) {
    pbtest::TmpDir d;
    const std::string big(5u << 20, 'a');
    PB_CHECK_EQ(hash_of(d, big, pbc::HashAlgo::Sha256), std::string("a29968fad2e782aa9f2040a35f05adb97ed8979eb1f572c8c8ea78637e275f3c"));
    PB_CHECK_EQ(hash_of(d, big, pbc::HashAlgo::Sha1), std::string("61b8d6600ac94d912874f569a9341120f680c9f8"));
    PB_CHECK_EQ(hash_of(d, big, pbc::HashAlgo::Md5), std::string("79b281060d337b9b2b84ccf390adcf74"));
}

PB_TEST(a_cancel_in_the_middle_ends_the_pass_and_a_missing_file_is_an_error) {
    pbtest::TmpDir d;
    d.write("f", std::string(5u << 20, 'x'));
    CancelAfter c(3);
    const auto r = pbc::hash_file(d.file("f"), pbc::HashAlgo::Sha256, c);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("cancelled"));
    NeverCancel n;
    PB_CHECK(!pbc::hash_file(d.file("missing"), pbc::HashAlgo::Md5, n).ok());
}

PB_TEST_MAIN()
