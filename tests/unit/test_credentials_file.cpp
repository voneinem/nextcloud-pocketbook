// T034: the `credentials` file (research R-13): atomic write, 0600 from the first byte, honest protection status,
// round trip of awkward passwords, overwrite-then-unlink on forget, and no password anywhere else.
#include <sys/stat.h>
#include <unistd.h>

#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>

#include "pbc/credentials_file.h"
#include "pbc/log.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

mode_t mode_of(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0;
}

std::vector<std::string> entries(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
        }
        closedir(d);
    }
    return out;
}

pbc::Credentials sample() {
    pbc::Credentials c;
    c.password = "pw = with=equals, spaces \\ percent% and \xC3\xA4\xE2\x82\xAC ";
    return c;
}

}  // namespace

PB_TEST(save_and_load_round_trip_an_awkward_password) {
    pbtest::TmpDir dir;
    const auto in = sample();
    PB_CHECK(pbc::save_credentials(dir.path(), in).ok());
    auto out = pbc::load_credentials(dir.path());
    PB_CHECK(out.ok());
    PB_CHECK_EQ(out.value.password, in.password);
}

PB_TEST(passwords_with_line_breaks_round_trip_too) {
    pbtest::TmpDir dir;
    pbc::Credentials c;
    c.password = "line1\nline2\r\nend";
    PB_CHECK(pbc::save_credentials(dir.path(), c).ok());
    PB_CHECK_EQ(pbc::load_credentials(dir.path()).value.password, c.password);
    PB_CHECK_EQ(entries(dir.path()).size(), static_cast<size_t>(1));   // the line breaks did not make extra files
}

PB_TEST(a_missing_file_loads_as_empty_passwords) {
    pbtest::TmpDir dir;
    auto r = pbc::load_credentials(dir.path());
    PB_CHECK(r.ok());
    PB_CHECK(r.value.password.empty());
    PB_CHECK(pbc::credentials_protection(dir.path()) == pbc::ProtectionStatus::Unknown);
}

PB_TEST(the_file_is_private_whatever_the_umask_and_leaves_no_temp_file) {
    pbtest::TmpDir dir;
    const mode_t old = ::umask(0);   // a permissive umask must not widen the file (K6: 0600 from the start)
    pbc::ProtectionStatus status = pbc::ProtectionStatus::Unknown;
    const auto err = pbc::save_credentials(dir.path(), sample(), &status);
    ::umask(old);
    PB_CHECK(err.ok());
    PB_CHECK_EQ(mode_of(pbc::credentials_path(dir.path())), static_cast<mode_t>(0600));
    PB_CHECK(status == pbc::ProtectionStatus::Enforced);   // /tmp honors chmod
    PB_CHECK(pbc::credentials_protection(dir.path()) == pbc::ProtectionStatus::Enforced);
    const auto names = entries(dir.path());
    PB_CHECK_EQ(names.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(names[0], std::string("credentials"));
}

PB_TEST(a_group_readable_file_is_reported_as_not_enforced) {
    pbtest::TmpDir dir;
    PB_CHECK(pbc::save_credentials(dir.path(), sample()).ok());
    PB_CHECK(::chmod(pbc::credentials_path(dir.path()).c_str(), 0644) == 0);
    PB_CHECK(pbc::credentials_protection(dir.path()) == pbc::ProtectionStatus::NotEnforced);   // the FAT case
}

PB_TEST(a_missing_data_directory_is_created_private) {
    pbtest::TmpDir dir;
    const std::string sub = dir.file("data");
    PB_CHECK(pbc::save_credentials(sub, sample()).ok());
    PB_CHECK_EQ(mode_of(sub), static_cast<mode_t>(0700));
}

PB_TEST(saving_again_replaces_the_content_atomically) {
    pbtest::TmpDir dir;
    PB_CHECK(pbc::save_credentials(dir.path(), sample()).ok());
    pbc::Credentials c;
    c.password = "second";
    PB_CHECK(pbc::save_credentials(dir.path(), c).ok());
    auto r = pbc::load_credentials(dir.path());
    PB_CHECK_EQ(r.value.password, std::string("second"));
    PB_CHECK_EQ(entries(dir.path()).size(), static_cast<size_t>(1));
}

PB_TEST(forget_overwrites_then_unlinks) {
    pbtest::TmpDir dir;
    PB_CHECK(pbc::save_credentials(dir.path(), sample()).ok());
    const std::string alias = dir.file("alias");   // a second name for the same inode shows what is left on disk
    PB_CHECK(::link(pbc::credentials_path(dir.path()).c_str(), alias.c_str()) == 0);
    PB_CHECK(pbc::forget_credentials(dir.path()).ok());
    PB_CHECK(!dir.exists("credentials"));
    const std::string left = dir.read("alias");
    PB_CHECK(left.find("with=equals") == std::string::npos);
    PB_CHECK(pbc::forget_credentials(dir.path()).ok());   // missing file: success
}

PB_TEST(a_damaged_or_newer_file_is_reported_not_guessed) {
    pbtest::TmpDir dir;
    dir.write("credentials", "garbage without a version line\npassword=x\n");
    PB_CHECK_EQ(pbc::load_credentials(dir.path()).error.key, std::string("credentials_unreadable"));
    dir.write("credentials", "pbc-credentials 99\npassword=x\n");
    PB_CHECK_EQ(pbc::load_credentials(dir.path()).error.key, std::string("credentials_unreadable"));
}

PB_TEST(passwords_never_reach_the_log) {
    pbtest::TmpDir dir;
    std::string logged;
    pbc::log::set_sink([&](const std::string& line) { logged += line + "\n"; });
    const auto c = sample();
    pbc::save_credentials(dir.path(), c);
    pbc::load_credentials(dir.path());
    pbc::forget_credentials(dir.path());
    dir.write("credentials", "broken");
    pbc::load_credentials(dir.path());
    pbc::log::reset_sink();
    PB_CHECK(logged.find(c.password) == std::string::npos);
}

PB_TEST_MAIN()
