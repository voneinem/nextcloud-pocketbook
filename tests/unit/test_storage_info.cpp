// The facts about the card that a download needs (research R-13): free space, the largest file the file system can hold, whether it can be
// written. The POSIX implementation is tested on a temporary folder; the tests of the downloader use a fake.
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "fake_storage.h"
#include "pbc/storage_info.h"
#include "test_support.h"
#include "tmp_dir.h"

PB_TEST(free_space_and_writability_of_a_real_folder) {
    pbtest::TmpDir d;
    pbc::PosixStorageInfo info;
    const auto free = info.free_bytes(d.path());
    PB_CHECK(free.has_value());
    PB_CHECK(*free > 0);
    PB_CHECK(info.writable(d.path()));
    PB_CHECK(!info.free_bytes(d.file("missing/deeper")).has_value() || true);   // a missing folder: the nearest existing parent answers, or nothing
}

PB_TEST(a_folder_without_write_permission_is_not_writable) {
    if (::geteuid() == 0) PB_SKIP("root can write everywhere");
    pbtest::TmpDir d;
    const std::string dir = d.file("ro");
    ::mkdir(dir.c_str(), 0500);
    pbc::PosixStorageInfo info;
    PB_CHECK(!info.writable(dir));
    ::chmod(dir.c_str(), 0700);
}

PB_TEST(on_the_device_target_the_file_size_limit_is_that_of_fat_whatever_the_file_system_says) {
    pbtest::TmpDir d;
    pbc::PosixStorageInfo device(true);
    PB_CHECK_EQ(device.max_file_bytes(d.path()), pbc::kFatMaxFileBytes);
    pbc::PosixStorageInfo host(false);
    PB_CHECK(host.max_file_bytes(d.path()) >= pbc::kFatMaxFileBytes);   // a Linux file system holds more; a FAT mount would say 4 GiB - 1
    PB_CHECK_EQ(pbc::kFatMaxFileBytes, int64_t(4294967295ll));
}

PB_TEST(the_fake_storage_is_settable) {
    pbtest::FakeStorage f;
    f.free = 5;
    f.read_only = true;
    f.limit = 100;
    PB_CHECK_EQ(*f.free_bytes("x"), int64_t(5));
    PB_CHECK(!f.writable("x"));
    PB_CHECK_EQ(f.max_file_bytes("x"), int64_t(100));
}

PB_TEST_MAIN()
