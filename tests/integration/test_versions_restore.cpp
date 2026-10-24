// Restoring an older version of a file (specs/006-trash-and-versions, spec Story 3, FR-016 and FR-017, research R-9, contracts/trash-requests.md T7), against the in-process
// fake Nextcloud: the chosen content becomes the current one, the content that was current is kept as one more version, nothing is deleted. It proves the app follows the
// contract, never a real server (constitution Principle IX).
#include <string>

#include "test_support.h"
#include "trash_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::TrashEnv;

namespace {

struct Setup {
    TrashEnv e;
    pbc::RemoteEntry file;
    pbc::VersionList shown;
    Setup() {
        e.server.add_file("/Books/a.txt", "current", 1760000300);
        e.server.add_version("/Books/a.txt", "v1", 1760000100);
        e.server.add_version("/Books/a.txt", "v2 text", 1760000200);
        refresh();
    }
    void refresh() {
        const auto n = e.server.nodes().at("/Books/a.txt");
        file = pbc::RemoteEntry();
        file.path = "/Books/a.txt";
        file.name = "a.txt";
        file.kind = pbc::Kind::File;
        file.fileid = n.fileid;
        file.etag = n.etag;
        file.mtime = n.mtime;
        file.size = n.size();
        const auto l = e.api.list_versions(file);
        if (!l.ok()) PB_FAIL_AT("cannot list: " + l.error.key);
        shown = l.value;
    }
    const pbc::VersionRow& row(const std::string& id) {
        for (const auto& r : shown.rows)
            if (r.version_id == id) return r;
        PB_FAIL_AT("no version " + id);
    }
};

}  // namespace

PB_TEST(the_chosen_version_becomes_the_current_content_and_the_previous_one_is_kept_as_a_version) {
    Setup s;
    s.e.server.clear_log();
    pbc::VersionList after;
    const auto r = s.e.api.restore_version(s.file, s.shown, s.row("1760000100"), &after);
    PB_CHECK(r.error.ok() && !r.outcome_unknown && !r.previous_not_kept);
    PB_CHECK_EQ(s.e.server.content_of("/Books/a.txt"), std::string("v1"));
    // nothing was lost: the former current content is a version now and the two others are still there
    PB_CHECK_EQ(s.e.server.versions_of("/Books/a.txt").size(), size_t(3));
    PB_CHECK_EQ(s.e.server.versions_of("/Books/a.txt").back().content, std::string("current"));
    PB_CHECK_EQ(after.rows.size(), size_t(3));   // the list the page shows afterwards
    bool previous = false;
    for (const auto& row : after.rows) previous = previous || row.version_id == "1760000300";
    PB_CHECK(previous);
    // the requests: the stale check (Depth 0), the MOVE with its destination, then the checks of the result; no DELETE and no PUT
    PB_CHECK_EQ(s.e.server.log()[0].method, std::string("PROPFIND"));
    PB_CHECK_EQ(s.e.server.log()[0].depth, std::string("0"));
    PB_CHECK_EQ(s.e.server.log()[1].method, std::string("MOVE"));
    PB_CHECK_EQ(s.e.server.log()[1].overwrite, std::string("F"));
    PB_CHECK(s.e.server.log()[1].destination.find("/remote.php/dav/versions/demo/restore/target") != std::string::npos);
    PB_CHECK_EQ(s.e.moves(), size_t(1));
    PB_CHECK_EQ(s.e.server.count("DELETE") + s.e.server.count("PUT"), size_t(0));
}

PB_TEST(a_server_that_does_not_keep_the_previous_content_is_reported_and_nothing_is_deleted_by_the_app) {
    Setup s;
    s.e.server.set_versions_keep_current(false);
    const auto r = s.e.api.restore_version(s.file, s.shown, s.row("1760000100"));
    PB_CHECK(r.error.ok());
    PB_CHECK(r.previous_not_kept);   // the content that was current is not among the versions: the page says so plainly
    PB_CHECK_EQ(s.e.server.versions_of("/Books/a.txt").size(), size_t(2));
    PB_CHECK_EQ(s.e.server.count("DELETE"), size_t(0));
}

PB_TEST(a_restored_file_can_be_restored_back_to_what_it_was) {
    Setup s;
    PB_CHECK(s.e.api.restore_version(s.file, s.shown, s.row("1760000100")).error.ok());
    s.refresh();
    PB_CHECK(s.e.api.restore_version(s.file, s.shown, s.row("1760000300")).error.ok());   // back to the content that was current
    PB_CHECK_EQ(s.e.server.content_of("/Books/a.txt"), std::string("current"));
}

PB_TEST(a_version_that_is_gone_gives_version_gone_and_changes_nothing) {
    Setup s;
    s.e.server.set_interceptor([](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.method != "MOVE") return std::nullopt;
        pbc::Response resp;
        resp.status = 404;
        return resp;
    });
    const auto r = s.e.api.restore_version(s.file, s.shown, s.row("1760000100"));
    PB_CHECK_EQ(r.error.key, std::string("version_gone"));
    PB_CHECK_EQ(r.code, std::string("T7-404"));
    PB_CHECK_EQ(s.e.server.content_of("/Books/a.txt"), std::string("current"));
}

PB_TEST(refusals_give_their_key_and_a_code_without_a_name_and_change_nothing) {
    struct Row { long status; const char* key; };
    for (const Row& row : {Row{403, "forbidden"}, Row{507, "server_storage_full"}, Row{401, "auth_failed"}, Row{500, "server_error"}, Row{429, "rate_limited"}}) {
        Setup s;
        s.e.server.answer_next("MOVE", "/versions/", row.status);
        const auto r = s.e.api.restore_version(s.file, s.shown, s.row("1760000100"));
        PB_CHECK_EQ(r.error.key, std::string(row.key));
        PB_CHECK_EQ(r.code, "T7-" + std::to_string(row.status));
        PB_CHECK_EQ(s.e.server.content_of("/Books/a.txt"), std::string("current"));
        PB_CHECK_EQ(s.e.server.versions_of("/Books/a.txt").size(), size_t(2));
        PB_CHECK_EQ(s.e.moves(), size_t(1));   // one try
    }
}

PB_TEST(a_locked_file_gives_locked) {
    Setup s;
    s.e.server.lock("/Books/a.txt");
    const auto r = s.e.api.restore_version(s.file, s.shown, s.row("1760000100"));
    PB_CHECK_EQ(r.error.key, std::string("locked"));
    PB_CHECK_EQ(s.e.server.content_of("/Books/a.txt"), std::string("current"));
}

PB_TEST(a_lost_answer_is_unknown_and_the_request_is_never_repeated) {
    Setup s;
    s.e.server.lose_next_answer("MOVE", "/versions/");
    s.e.server.clear_log();
    const auto r = s.e.api.restore_version(s.file, s.shown, s.row("1760000100"));
    PB_CHECK(r.outcome_unknown);
    PB_CHECK_EQ(r.error.key, std::string("outcome_unknown"));
    PB_CHECK_EQ(s.e.moves(), size_t(1));
    PB_CHECK_EQ(s.e.server.request_count(), size_t(2));   // the stale check and the MOVE: no guess, no second try
    PB_CHECK_EQ(s.e.server.content_of("/Books/a.txt"), std::string("v1"));   // the server did it; a reload shows the truth
}

PB_TEST(a_folder_a_bad_version_id_and_a_file_without_an_id_send_nothing) {
    Setup s;
    s.e.server.add_folder("/Books/Sub");
    s.e.server.clear_log();
    auto folder = s.file;
    folder.kind = pbc::Kind::Folder;
    PB_CHECK_EQ(s.e.api.restore_version(folder, s.shown, s.row("1760000100")).error.key, std::string("trash_not_actionable"));
    auto bad = s.row("1760000100");
    bad.version_id = "../5";
    PB_CHECK_EQ(s.e.api.restore_version(s.file, s.shown, bad).error.key, std::string("trash_not_actionable"));
    auto no_id = s.file;
    no_id.fileid.reset();
    PB_CHECK_EQ(s.e.api.restore_version(no_id, s.shown, s.row("1760000100")).error.key, std::string("refresh_first"));
    PB_CHECK_EQ(s.e.server.request_count(), size_t(0));
}

PB_TEST_MAIN()
