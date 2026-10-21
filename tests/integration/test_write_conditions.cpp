// Every write request the app sends carries its condition (specs/002-manage-server-files SC-007/SC-008, constitution Principle III, research R-3):
// the request log of the fake shows what actually went out, whatever path the item took. Each later story adds its operations to the table below.
// The only unconditional writes are the app's own upload folder under /dav/uploads/ (a temporary session, not user data) and MKCOL, which a server
// refuses by nature when the name exists.
#include <string>

#include "upload_env.h"

PBC_FAKE_SERVER_TEST();

using pbtest::UpEnv;
using pbc::AskChoice;

namespace {

constexpr long long kMiB = 1ll << 20;

bool in_upload_area(const std::string& path) { return path.find("/remote.php/dav/uploads/") != std::string::npos; }
bool strong(const std::string& etag) { return !etag.empty() && etag.compare(0, 2, "W/") != 0; }

// The rule for every write request in the log.
void check_conditions(const std::vector<pbtest::FakeNextcloud::Logged>& log) {
    for (const auto& l : log) {
        const bool write = l.method == "PUT" || l.method == "MKCOL" || l.method == "MOVE" || l.method == "COPY" || l.method == "DELETE";
        PB_CHECK(l.method != "PROPPATCH" && l.method != "LOCK" && l.method != "UNLOCK");   // never sent by this app
        if (!write) continue;
        if (in_upload_area(l.path)) {
            if (l.method == "MOVE") {   // the assembly into the user's tree: Overwrite F (never T) or T with the target's version
                PB_CHECK(l.overwrite == "F" || (l.overwrite == "T" && strong(l.if_match)));
            }
            continue;
        }
        if (l.method == "PUT") {
            // exactly one of the two, and a strong version when it is If-Match
            PB_CHECK((l.if_none_match == "*") != strong(l.if_match));
            if (!l.if_match.empty()) PB_CHECK(strong(l.if_match));
        } else if (l.method == "DELETE") {
            PB_CHECK(strong(l.if_match));
        } else if (l.method == "MOVE" || l.method == "COPY") {
            PB_CHECK(l.overwrite == "F" || (l.overwrite == "T" && strong(l.if_match)));
            PB_CHECK(strong(l.if_match));
        }
    }
}

}  // namespace

PB_TEST(a_new_small_file_is_put_if_none_match_and_a_replace_is_put_if_match_with_a_strong_version) {
    UpEnv e;
    e.server.add_file("/Docs/old.txt", "old");
    e.cache("/Docs");
    e.queue({e.source("Downloads/new.txt", 10)});
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {e.source("Downloads/old.txt", 20)});
    PB_CHECK_EQ(e.actions().answer_ask(r.asks[0], AskChoice::Replace).queued.size(), size_t(1));
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 2);
    const auto puts = e.logged("PUT");
    PB_CHECK_EQ(puts.size(), size_t(2));
    PB_CHECK_EQ(puts[0].if_none_match, std::string("*"));
    PB_CHECK(puts[0].if_match.empty());
    PB_CHECK(puts[1].if_none_match.empty());
    PB_CHECK(strong(puts[1].if_match));
    check_conditions(e.server.log());
}

PB_TEST(a_chunked_upload_new_or_replace_never_asks_the_server_to_overwrite_blindly) {
    UpEnv e;
    e.server.add_file("/Docs/big2.bin", "old big");
    e.cache("/Docs");
    e.queue({e.source("Downloads/big1.bin", 12 * kMiB)});
    const pbc::AddResult r = e.actions().add_uploads("/Docs", {e.source("Downloads/big2.bin", 12 * kMiB)});
    PB_CHECK_EQ(e.actions().answer_ask(r.asks[0], AskChoice::Replace).queued.size(), size_t(1));
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 2);
    check_conditions(e.server.log());
    for (const auto& m : e.logged("MOVE")) PB_CHECK_EQ(m.overwrite, std::string("F"));
    PB_CHECK_EQ(e.server.overwrites(), 0);
    // the target is looked at (Depth 0) before the final MOVE of each
    int propfinds_of_targets = 0;
    for (const auto& l : e.server.log())
        if (l.method == "PROPFIND" && l.depth == "0" && l.path.find("/remote.php/dav/files/") != std::string::npos) ++propfinds_of_targets;
    PB_CHECK(propfinds_of_targets >= 2);
}

PB_TEST(a_new_folder_is_a_plain_mkcol_which_the_server_refuses_for_an_existing_name) {
    UpEnv e;
    e.source("Notes/trip/plan.md", 20);
    auto act = e.actions();
    act.confirm_folder_pick(act.count_folder_pick("/Docs", {e.root + "/Notes/trip"}));
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 2);
    PB_CHECK_EQ(e.logged("MKCOL").size(), size_t(1));
    check_conditions(e.server.log());
}

PB_TEST(the_upload_folder_is_the_only_unconditional_write_and_lives_under_the_upload_area) {
    UpEnv e;
    e.queue({e.source("Downloads/big.bin", 12 * kMiB)});
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 1);
    int unconditional = 0;
    for (const auto& l : e.server.log()) {
        const bool write = l.method == "PUT" || l.method == "MKCOL" || l.method == "MOVE" || l.method == "DELETE";
        if (write && l.if_match.empty() && l.if_none_match.empty() && l.overwrite.empty()) {
            ++unconditional;
            PB_CHECK(in_upload_area(l.path));
        }
    }
    PB_CHECK(unconditional >= 3);   // the MKCOL of the upload folder and the chunks
    check_conditions(e.server.log());
}

PB_TEST(rename_move_copy_and_delete_carry_the_entrys_strong_version_and_overwrite_f) {
    UpEnv e;
    e.server.add_folder("/Docs/dst");
    e.server.add_file("/Docs/a.txt", "x");
    e.server.add_file("/Docs/b.txt", "x");
    e.server.add_file("/Docs/c.txt", "x");
    e.server.add_file("/Docs/d.txt", "x");
    e.cache("/Docs");
    e.cache("/Docs/dst");
    auto act = e.actions();
    PB_CHECK(act.rename_entry("/Docs/a.txt", "a2.txt").error.ok());
    PB_CHECK_EQ(act.move_or_copy(pbc::ItemKind::Move, {"/Docs/b.txt"}, "/Docs/dst").queued.size(), size_t(1));
    PB_CHECK_EQ(act.move_or_copy(pbc::ItemKind::Copy, {"/Docs/c.txt"}, "/Docs/dst").queued.size(), size_t(1));
    PB_CHECK_EQ(act.delete_entries({"/Docs/d.txt"}).queued.size(), size_t(1));
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 4);
    check_conditions(e.server.log());
    PB_CHECK_EQ(e.logged("MOVE").size(), size_t(2));
    PB_CHECK_EQ(e.logged("COPY").size(), size_t(1));
    PB_CHECK_EQ(e.logged("DELETE").size(), size_t(1));
    PB_CHECK_EQ(e.server.overwrites(), 0);
    PB_CHECK_EQ(e.server.permanent_deletes(), 0);
}

PB_TEST(a_replace_by_move_or_copy_is_a_conditional_delete_of_the_target_then_overwrite_f) {
    UpEnv e;
    e.server.add_folder("/Docs/dst");
    e.server.add_file("/Docs/a.txt", "mine");
    e.server.add_file("/Docs/b.txt", "mine");
    e.server.add_file("/Docs/dst/a.txt", "theirs");
    e.server.add_file("/Docs/dst/b.txt", "theirs");
    e.cache("/Docs");
    e.cache("/Docs/dst");
    auto act = e.actions();
    const auto m = act.move_or_copy(pbc::ItemKind::Move, {"/Docs/a.txt"}, "/Docs/dst");
    const auto c = act.move_or_copy(pbc::ItemKind::Copy, {"/Docs/b.txt"}, "/Docs/dst");
    PB_CHECK_EQ(act.answer_ask(m.asks[0], AskChoice::Replace).queued.size(), size_t(1));
    PB_CHECK_EQ(act.answer_ask(c.asks[0], AskChoice::Replace).queued.size(), size_t(1));
    e.server.clear_log();
    PB_CHECK_EQ(e.run().done, 2);
    check_conditions(e.server.log());
    PB_CHECK_EQ(e.logged("DELETE").size(), size_t(2));
    PB_CHECK_EQ(e.server.overwrites(), 0);
    for (const auto& l : e.server.log())
        if (l.method == "MOVE" || l.method == "COPY") PB_CHECK_EQ(l.overwrite, std::string("F"));
    PB_CHECK_EQ(e.server.trash_count(), 2LL);
}

PB_TEST(a_weak_etag_is_never_sent_in_if_match_because_such_an_item_cannot_be_queued) {
    UpEnv e;
    e.server.add_file("/Docs/a.txt", "x");
    e.server.set_weak_etag("/Docs/a.txt", true);
    e.cache("/Docs");
    auto act = e.actions();
    PB_CHECK_EQ(act.rename_entry("/Docs/a.txt", "b.txt").error.key, std::string("refresh_first"));
    PB_CHECK_EQ(act.delete_entries({"/Docs/a.txt"}).refused.size(), size_t(1));
    PB_CHECK_EQ(act.move_or_copy(pbc::ItemKind::Move, {"/Docs/a.txt"}, "/Docs").refused.size(), size_t(1));
    const pbc::AddResult up = act.add_uploads("/Docs", {e.source("Downloads/a.txt", 10)});
    PB_CHECK_EQ(act.answer_ask(up.asks[0], AskChoice::Replace).error.key, std::string("refresh_first"));
    PB_CHECK_EQ(e.store->queue_count_open(), 0);
}

PB_TEST_MAIN()
