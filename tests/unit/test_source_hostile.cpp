// The file an upload reads (specs/002-manage-server-files, research R-5 and R-13, constitution Principle IV): it is checked again when the
// item is sent, opened read-only without following links, and its identity (size and modification time) is what a resume relies on.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "pbc/upload_source.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct NeverCancel : pbc::Cancel {
    bool requested() const override { return false; }
};

struct CancelAfter : pbc::Cancel {
    mutable int asked = 0;
    int limit;
    explicit CancelAfter(int n) : limit(n) {}
    bool requested() const override { return ++asked > limit; }
};

void write_file(const std::string& path, const std::string& data) {
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    PB_CHECK(fd >= 0);
    if (!data.empty()) PB_CHECK(::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    ::close(fd);
}

void sparse_file(const std::string& path, long long size) {
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    PB_CHECK(fd >= 0);
    PB_CHECK(::ftruncate(fd, size) == 0);
    ::close(fd);
}

struct Env {
    pbtest::TmpDir dir;
    pbc::SourceRules rules;
    std::string root, data;
    Env() {
        root = dir.file("root");
        data = root + "/system/pbcloud";
        ::mkdir(root.c_str(), 0700);
        ::mkdir((root + "/system").c_str(), 0700);
        ::mkdir(data.c_str(), 0700);
        rules.root = root;
        rules.deny = {data};
    }
    std::string file(const std::string& name) const { return root + "/" + name; }
};

}  // namespace

PB_TEST(a_regular_file_of_one_byte_up_to_four_gib_minus_one_is_a_source) {
    Env e;
    write_file(e.file("one.bin"), "a");
    const auto r = pbc::inspect_source(e.file("one.bin"), e.rules);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.size, 1LL);
    PB_CHECK(r.value.mtime > 1500000000);
    sparse_file(e.file("max.bin"), 4294967295ll);   // 4 GiB - 1: the largest file FAT can hold
    const auto big = pbc::inspect_source(e.file("max.bin"), e.rules);
    PB_CHECK(big.ok());
    PB_CHECK_EQ(big.value.size, 4294967295LL);
}

PB_TEST(an_empty_a_too_large_a_missing_and_an_unreadable_file_are_refused_with_their_keys) {
    Env e;
    write_file(e.file("empty.bin"), "");
    PB_CHECK_EQ(pbc::inspect_source(e.file("empty.bin"), e.rules).error.key, std::string("source_unreadable"));
    sparse_file(e.file("huge.bin"), 4294967296ll);   // 4 GiB
    PB_CHECK_EQ(pbc::inspect_source(e.file("huge.bin"), e.rules).error.key, std::string("too_large_for_storage"));
    PB_CHECK_EQ(pbc::inspect_source(e.file("missing.bin"), e.rules).error.key, std::string("source_unreadable"));
    ::mkdir(e.file("folder").c_str(), 0700);
    PB_CHECK_EQ(pbc::inspect_source(e.file("folder"), e.rules).error.key, std::string("source_unreadable"));
    write_file(e.file("closed.bin"), "x");
    ::chmod(e.file("closed.bin").c_str(), 0);
    if (::access(e.file("closed.bin").c_str(), R_OK) != 0) PB_CHECK_EQ(pbc::inspect_source(e.file("closed.bin"), e.rules).error.key, std::string("source_unreadable"));
    ::chmod(e.file("closed.bin").c_str(), 0600);
}

PB_TEST(a_link_swapped_in_after_the_pick_is_refused_when_the_item_is_sent) {
    Env e;
    write_file(e.file("photo.jpg"), "image");
    write_file(e.data + "/credentials", "app-secret");
    PB_CHECK(pbc::inspect_source(e.file("photo.jpg"), e.rules).ok());
    ::unlink(e.file("photo.jpg").c_str());
    PB_CHECK(::symlink((e.data + "/credentials").c_str(), e.file("photo.jpg").c_str()) == 0);   // the same name, now a link to the secrets
    PB_CHECK_EQ(pbc::inspect_source(e.file("photo.jpg"), e.rules).error.key, std::string("source_not_allowed"));
    NeverCancel never;
    const auto h = pbc::hash_source(e.file("photo.jpg"), never, nullptr);
    PB_CHECK(!h.ok());
    PB_CHECK_EQ(h.error.key, std::string("source_not_allowed"));
    // the data folder itself is never a source, whatever the file is called
    PB_CHECK_EQ(pbc::inspect_source(e.data + "/credentials", e.rules).error.key, std::string("source_not_allowed"));
}

PB_TEST(the_identity_is_size_and_modification_time_in_seconds) {
    pbc::SourceIdentity a{10, 1000}, b{10, 1000}, c{11, 1000}, d{10, 1001};
    PB_CHECK(pbc::same_identity(a, b));
    PB_CHECK(!pbc::same_identity(a, c));
    PB_CHECK(!pbc::same_identity(a, d));
    Env e;
    write_file(e.file("f.bin"), "abc");
    const auto first = pbc::inspect_source(e.file("f.bin"), e.rules);
    PB_CHECK(first.ok());
    PB_CHECK(pbc::same_identity(first.value, pbc::inspect_source(e.file("f.bin"), e.rules).value));
    write_file(e.file("f.bin"), "abcd");   // changed content
    PB_CHECK(!pbc::same_identity(first.value, pbc::inspect_source(e.file("f.bin"), e.rules).value));
}

PB_TEST(the_sha256_of_known_content_is_computed_in_64_kib_steps_with_progress) {
    Env e;
    write_file(e.file("a.bin"), "a");
    NeverCancel never;
    const auto one = pbc::hash_source(e.file("a.bin"), never, nullptr);
    PB_CHECK(one.ok());
    PB_CHECK_EQ(one.value, std::string("ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"));

    write_file(e.file("x5.bin"), std::string(5 * 1024 * 1024, 'x'));
    long long last_done = 0, last_total = 0;
    int calls = 0;
    const auto five = pbc::hash_source(e.file("x5.bin"), never, [&](long long done, long long total) {
        PB_CHECK(done >= last_done);
        last_done = done;
        last_total = total;
        ++calls;
    });
    PB_CHECK(five.ok());
    PB_CHECK_EQ(five.value, std::string("dba67a476fa78973aabb087f214a1010f3bebca053674e0af50dfe5a582112be"));
    PB_CHECK_EQ(last_done, 5ll * 1024 * 1024);
    PB_CHECK_EQ(last_total, 5ll * 1024 * 1024);
    PB_CHECK_EQ(calls, 80);   // 5 MiB in pieces of 64 KiB
}

PB_TEST(a_cancel_stops_the_hash_pass_and_a_missing_file_is_unreadable) {
    Env e;
    write_file(e.file("x5.bin"), std::string(5 * 1024 * 1024, 'x'));
    CancelAfter c(3);
    const auto r = pbc::hash_source(e.file("x5.bin"), c, nullptr);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("cancelled"));
    NeverCancel never;
    PB_CHECK_EQ(pbc::hash_source(e.file("none.bin"), never, nullptr).error.key, std::string("source_unreadable"));
}

PB_TEST_MAIN()
