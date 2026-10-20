// The state of an entry (data-model.md "Entry state"): computed from what the listing says, what was downloaded and what the card holds
// now - never stored. Spec FR-026.
#include <string>

#include "pbc/entry_state.h"
#include "test_support.h"

using pbc::EntryState;

namespace {

pbc::RemoteEntry server(const std::string& etag = "\"e1\"", std::optional<int64_t> size = 100, std::optional<int64_t> mtime = 150) {
    pbc::RemoteEntry e;
    e.name = "a.epub";
    e.path = "/a.epub";
    e.etag = etag;
    e.size = size;
    e.mtime = mtime;
    return e;
}

pbc::LocalFile copy(const std::string& etag = "\"e1\"") {
    pbc::LocalFile f;
    f.remote_path = "/a.epub";
    f.local_path = "a.epub";
    f.size = 100;
    f.mtime = 200;
    f.remote_etag = etag;
    f.remote_size = 100;
    f.remote_mtime = 150;
    return f;
}

pbc::FileStat on_card(int64_t size = 100, int64_t mtime = 200) {
    pbc::FileStat s;
    s.exists = true;
    s.regular = true;
    s.size = size;
    s.mtime = mtime;
    return s;
}

EntryState state(const pbc::RemoteEntry* e, const pbc::LocalFile* l, const pbc::FileStat& s) { return pbc::compute_state(e, l, s); }

}  // namespace

PB_TEST(without_a_recorded_copy_the_entry_is_not_downloaded) {
    const auto e = server();
    PB_CHECK(state(&e, nullptr, pbc::FileStat()) == EntryState::NotDownloaded);
    PB_CHECK(state(nullptr, nullptr, pbc::FileStat()) == EntryState::NotDownloaded);
}

PB_TEST(a_recorded_copy_that_is_gone_from_the_card_is_missing_locally) {
    const auto e = server();
    const auto l = copy();
    PB_CHECK(state(&e, &l, pbc::FileStat()) == EntryState::MissingLocally);
    pbc::FileStat dir = on_card();
    dir.regular = false;   // a folder or a link where the file was
    PB_CHECK(state(&e, &l, dir) == EntryState::MissingLocally);
}

PB_TEST(an_unchanged_copy_of_the_current_server_version_is_downloaded) {
    const auto e = server();
    const auto l = copy();
    PB_CHECK(state(&e, &l, on_card()) == EntryState::Downloaded);
    PB_CHECK(state(nullptr, &l, on_card()) == EntryState::Downloaded);   // nothing known about the server: no claim that it changed
}

PB_TEST(a_different_server_version_makes_an_unchanged_copy_outdated) {
    const auto e = server("\"e2\"");
    const auto l = copy();
    PB_CHECK(state(&e, &l, on_card()) == EntryState::Outdated);
}

PB_TEST(a_copy_changed_by_size_or_by_time_is_changed_on_the_reader) {
    const auto e = server();
    const auto l = copy();
    PB_CHECK(state(&e, &l, on_card(101, 200)) == EntryState::ChangedOnReader);
    PB_CHECK(state(&e, &l, on_card(100, 202)) == EntryState::ChangedOnReader);
}

PB_TEST(a_changed_copy_and_a_changed_server_version_are_a_conflict) {
    const auto e = server("\"e2\"");
    const auto l = copy();
    PB_CHECK(state(&e, &l, on_card(7, 200)) == EntryState::Conflict);
}

PB_TEST(removed_on_the_server_wins_among_the_states_with_a_copy) {
    auto l = copy();
    l.server_missing = true;
    const auto e = server("\"e2\"");
    PB_CHECK(state(&e, &l, on_card()) == EntryState::RemovedOnServer);
    PB_CHECK(state(&e, &l, on_card(7, 7)) == EntryState::RemovedOnServer);   // also when the user changed the copy
    PB_CHECK(state(nullptr, &l, on_card()) == EntryState::RemovedOnServer);
    PB_CHECK(state(&e, &l, pbc::FileStat()) == EntryState::MissingLocally);   // but a copy that is not on the card is missing first
}

PB_TEST(the_etag_is_compared_as_a_string_weak_marker_and_quotes_included) {
    auto l = copy("W/\"w1\"");
    PB_CHECK(state(nullptr, &l, on_card()) == EntryState::Downloaded);
    const auto same = server("W/\"w1\"");
    PB_CHECK(state(&same, &l, on_card()) == EntryState::Downloaded);
    const auto strong = server("\"w1\"");   // a proxy that stopped calling it weak is still a different string
    PB_CHECK(state(&strong, &l, on_card()) == EntryState::Outdated);
}

PB_TEST(without_an_etag_size_and_time_are_compared) {
    const auto l = copy();
    const pbc::RemoteEntry same = server("", 100, 150);
    PB_CHECK(state(&same, &l, on_card()) == EntryState::Downloaded);
    pbc::RemoteEntry bigger = server("", 120, 150);
    PB_CHECK(state(&bigger, &l, on_card()) == EntryState::Outdated);
    pbc::RemoteEntry later = server("", 100, 999);
    PB_CHECK(state(&later, &l, on_card()) == EntryState::Outdated);
    pbc::RemoteEntry unknown = server("", std::nullopt, std::nullopt);   // nothing to compare: no claim
    PB_CHECK(state(&unknown, &l, on_card()) == EntryState::Downloaded);
}

PB_TEST(the_version_the_user_chose_to_keep_against_is_not_newer_but_a_later_one_is) {
    auto l = copy("\"e1\"");
    l.seen_etag = "\"e2\"";
    const auto e2 = server("\"e2\"");
    PB_CHECK(state(&e2, &l, on_card()) == EntryState::Downloaded);
    PB_CHECK(state(&e2, &l, on_card(5, 5)) == EntryState::ChangedOnReader);   // keeping my copy: not a conflict any more
    const auto e3 = server("\"e3\"");
    PB_CHECK(state(&e3, &l, on_card()) == EntryState::Outdated);
}

PB_TEST(every_state_has_a_stable_key) {
    PB_CHECK_EQ(std::string(pbc::state_key(EntryState::NotDownloaded)), std::string("not_downloaded"));
    PB_CHECK_EQ(std::string(pbc::state_key(EntryState::Downloaded)), std::string("downloaded"));
    PB_CHECK_EQ(std::string(pbc::state_key(EntryState::Outdated)), std::string("outdated"));
    PB_CHECK_EQ(std::string(pbc::state_key(EntryState::ChangedOnReader)), std::string("changed_on_reader"));
    PB_CHECK_EQ(std::string(pbc::state_key(EntryState::Conflict)), std::string("conflict"));
    PB_CHECK_EQ(std::string(pbc::state_key(EntryState::RemovedOnServer)), std::string("removed_on_server"));
    PB_CHECK_EQ(std::string(pbc::state_key(EntryState::MissingLocally)), std::string("missing_locally"));
}

PB_TEST_MAIN()
