// What a confirmed change does to the reader's own records (specs/002-manage-server-files, research R-10, FR-029, clarification 3). This file holds the
// parts that exist so far: the link of an uploaded replacement to the downloaded copy it came from. Fake Nextcloud.
#include <fcntl.h>
#include <sys/stat.h>

#include <fstream>
#include <string>

#include "pbc/entry_state.h"
#include "pbc/local_files.h"
#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;
using pbc::AskChoice;

namespace {

// A downloaded copy of /Docs/a.txt under the download folder (which is also a top folder of the picker), as feature 001 records it, and then edited by
// the user: `edited` is what the file holds now.
std::string make_copy(UpEnv& e, const std::string& edited) {
    pbtest::up_mkdirs(e.download_root + "/Docs");
    const std::string path = e.download_root + "/Docs/a.txt";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "old version";
    }
    struct stat st {};
    PB_CHECK(::stat(path.c_str(), &st) == 0);
    pbc::LocalFile lf;
    lf.remote_path = "/Docs/a.txt";
    lf.local_path = "Docs/a.txt";
    lf.remote_etag = e.etag("/Docs/a.txt");
    lf.size = st.st_size;
    lf.mtime = st.st_mtime;
    lf.remote_size = 11;
    lf.remote_mtime = 1790000000;
    lf.downloaded_at = e.clock.t;
    PB_CHECK(e.store->commit_download(lf, 0).ok());
    {   // the user edits the copy: another size, a later time
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << edited;
    }
    struct timespec ts[2] = {{st.st_mtime + 100, 0}, {st.st_mtime + 100, 0}};
    PB_CHECK(::utimensat(AT_FDCWD, path.c_str(), ts, 0) == 0);
    return path;
}

int64_t replace_from(UpEnv& e, const std::string& src) {
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {src});
    PB_CHECK_EQ(r.asks.size(), size_t(1));
    const pbc::AddResult q = e.actions().answer_ask(r.asks[0], AskChoice::Replace);
    PB_CHECK_EQ(q.queued.size(), size_t(1));
    return q.queued[0];
}

std::string state_of(UpEnv& e) {
    const auto entry = e.store->entry("/Docs/a.txt");
    const auto local = e.store->local_file("/Docs/a.txt");
    const pbc::FileStat st = pbc::stat_file(e.download_root + "/" + local->local_path);
    return pbc::state_key(pbc::compute_state(entry ? &*entry : nullptr, local ? &*local : nullptr, st));
}

}  // namespace

PB_TEST(a_replace_from_the_entrys_own_downloaded_copy_links_the_copy_to_the_new_version) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "old version");
    e.cache("/Docs");
    const std::string copy = make_copy(e, "my edited copy of the file");
    PB_CHECK_EQ(state_of(e), std::string("changed_on_reader"));
    const int64_t id = replace_from(e, copy);
    PB_CHECK_EQ(e.run().done, 1);
    const auto local = e.store->local_file("/Docs/a.txt");
    PB_CHECK(local.has_value());
    PB_CHECK_EQ(local->remote_etag, e.etag("/Docs/a.txt"));
    PB_CHECK_EQ(local->remote_size.value_or(-1), 26LL);
    PB_CHECK_EQ(local->size, 26LL);
    PB_CHECK_EQ(state_of(e), std::string("downloaded"));   // neither "changed on the reader" nor "outdated"
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").content, std::string("my edited copy of the file"));
    (void)id;
}

PB_TEST(a_source_that_changed_again_during_the_upload_is_not_linked) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "old version");
    e.cache("/Docs");
    const std::string copy = make_copy(e, "my edited copy of the file");
    const std::string before_etag = e.store->local_file("/Docs/a.txt")->remote_etag;
    replace_from(e, copy);
    e.on_hook = [&](const char* step) {
        if (std::string(step) == "confirm") {   // the user edits the copy once more, after the bytes went out
            std::ofstream f(copy, std::ios::binary | std::ios::trunc);
            f << "edited again, longer than before";
        }
    };
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(state_of(e) != std::string("downloaded"));   // the copy is not claimed to match the server
    PB_CHECK_EQ(e.store->local_file("/Docs/a.txt")->remote_etag, before_etag);
}

PB_TEST(an_upload_from_any_other_source_never_touches_the_record_of_the_copy) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "old version");
    e.cache("/Docs");
    make_copy(e, "the downloaded copy");
    const auto before = *e.store->local_file("/Docs/a.txt");
    replace_from(e, e.source("Downloads/a.txt", 77));   // another file with the same name
    PB_CHECK_EQ(e.run().done, 1);
    const auto after = *e.store->local_file("/Docs/a.txt");
    PB_CHECK_EQ(after.remote_etag, before.remote_etag);
    PB_CHECK_EQ(after.size, before.size);
    PB_CHECK_EQ(after.mtime, before.mtime);
    PB_CHECK_EQ(after.remote_size.value_or(-1), before.remote_size.value_or(-1));
}

PB_TEST(a_failed_or_unknown_replace_never_changes_the_record) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "old version");
    e.cache("/Docs");
    const std::string copy = make_copy(e, "my edited copy of the file");
    const auto before = *e.store->local_file("/Docs/a.txt");
    const int64_t id = replace_from(e, copy);
    e.server.set_file("/Docs/a.txt", "changed by someone", 1790000900);
    PB_CHECK_EQ(e.run().conflict, 1);
    PB_CHECK_EQ(e.store->local_file("/Docs/a.txt")->remote_etag, before.remote_etag);
    PB_CHECK(e.sender()->resolve_keep_both(id, std::string()).ok());   // a new name: an upload of a new file never links either
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK_EQ(e.store->local_file("/Docs/a.txt")->remote_etag, before.remote_etag);
    PB_CHECK_EQ(e.store->local_file("/Docs/a.txt")->size, before.size);
}

PB_TEST(an_upload_of_a_new_file_never_creates_or_changes_a_record) {
    UpEnv e;
    e.server.add_folder("/Docs");
    e.cache("/Docs");
    e.queue({e.source("Downloads/new.txt", 10)});
    PB_CHECK_EQ(e.run().done, 1);
    PB_CHECK(!e.store->local_file("/Docs/new.txt").has_value());
}

PB_TEST_MAIN()
