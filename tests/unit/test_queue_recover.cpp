// What happens to an item that was being sent when the app was killed or the power failed (specs/002-manage-server-files, research R-6.5 and
// R-7, constitution Principle III): a step that the server's own state can verify goes back to the queue, a step that may have changed the
// server becomes "outcome unknown". Recovery sends nothing and is the same when repeated.
#include <string>

#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbc::SystemClock g_clock;

pbc::QueueItem upload(const std::string& name) {
    pbc::QueueItem q;
    q.kind = pbc::ItemKind::Upload;
    q.entry_kind = pbc::Kind::File;
    q.server = "https://cloud.example.org";
    q.user_id = "demo";
    q.src_local = "/mnt/ext1/Downloads/" + name;
    q.dst_parent = "/Docs";
    q.dst_name = name;
    return q;
}

int64_t add_in_state(pbc::Store& s, const std::string& name, pbc::ItemState state, const std::string& step) {
    const auto id = s.queue_add(upload(name));
    if (!id.ok()) PB_FAIL_AT("queue_add failed: " + id.error.key);
    auto it = s.queue_item(id.value);
    it->state = state;
    it->step = step;
    if (!s.queue_update(*it).ok()) PB_FAIL_AT("queue_update failed");
    return id.value;
}

}  // namespace

PB_TEST(a_step_the_server_can_verify_goes_back_to_the_queue) {
    pbtest::TmpDir d;
    auto r = pbc::Store::open(d.file("data"), g_clock);
    PB_CHECK(r.ok());
    pbc::Store& s = *r.value;
    const int64_t prepare = add_in_state(s, "prepare.bin", pbc::ItemState::Sending, "prepare");
    const int64_t chunks = add_in_state(s, "chunks.bin", pbc::ItemState::Sending, "chunks");
    const auto rep = s.queue_recover();
    PB_CHECK_EQ(rep.requeued, 2);
    PB_CHECK_EQ(rep.unknown, 0);
    for (int64_t id : {prepare, chunks}) {
        PB_CHECK(s.queue_item(id)->state == pbc::ItemState::Queued);
        PB_CHECK_EQ(s.queue_item(id)->reason, std::string());
    }
}

PB_TEST(a_step_that_may_have_changed_the_server_becomes_outcome_unknown) {
    pbtest::TmpDir d;
    auto r = pbc::Store::open(d.file("data"), g_clock);
    PB_CHECK(r.ok());
    pbc::Store& s = *r.value;
    std::vector<int64_t> ids;
    for (const char* step : {"single", "assemble", "request", "", "something-new"}) ids.push_back(add_in_state(s, std::string("u") + step + ".bin", pbc::ItemState::Sending, step));
    const auto rep = s.queue_recover();
    PB_CHECK_EQ(rep.requeued, 0);
    PB_CHECK_EQ(rep.unknown, 5);   // an unknown step is treated like the dangerous ones
    for (int64_t id : ids) {
        PB_CHECK(s.queue_item(id)->state == pbc::ItemState::Unknown);
        PB_CHECK_EQ(s.queue_item(id)->reason, std::string("no_answer"));
    }
}

PB_TEST(every_other_state_is_left_alone_and_nothing_is_lost) {
    pbtest::TmpDir d;
    auto r = pbc::Store::open(d.file("data"), g_clock);
    PB_CHECK(r.ok());
    pbc::Store& s = *r.value;
    const int64_t queued = add_in_state(s, "q.bin", pbc::ItemState::Queued, "");
    const int64_t failed = add_in_state(s, "f.bin", pbc::ItemState::Failed, "");
    const int64_t conflict = add_in_state(s, "c.bin", pbc::ItemState::Conflict, "");
    const int64_t unknown = add_in_state(s, "u.bin", pbc::ItemState::Unknown, "");
    const int64_t done = add_in_state(s, "d.bin", pbc::ItemState::Done, "");
    const auto rep = s.queue_recover();
    PB_CHECK_EQ(rep.requeued, 0);
    PB_CHECK_EQ(rep.unknown, 0);
    PB_CHECK(s.queue_item(queued)->state == pbc::ItemState::Queued);
    PB_CHECK(s.queue_item(failed)->state == pbc::ItemState::Failed);
    PB_CHECK(s.queue_item(conflict)->state == pbc::ItemState::Conflict);
    PB_CHECK(s.queue_item(unknown)->state == pbc::ItemState::Unknown);
    PB_CHECK(s.queue_item(done)->state == pbc::ItemState::Done);
    PB_CHECK_EQ(s.queue_count_open(), 4);
}

PB_TEST(recovery_is_idempotent_and_keeps_the_upload_session_of_a_requeued_item) {
    pbtest::TmpDir d;
    auto r = pbc::Store::open(d.file("data"), g_clock);
    PB_CHECK(r.ok());
    pbc::Store& s = *r.value;
    const auto id = s.queue_add(upload("big.bin"));
    auto it = s.queue_item(id.value);
    it->state = pbc::ItemState::Sending;
    it->step = "chunks";
    it->upload_id = "pbc-feedface";
    it->chunk_size = 10 << 20;
    it->checksum = "00ff";
    it->src_size = 500 << 20;
    it->src_mtime = 1790000000;
    PB_CHECK(s.queue_update(*it).ok());
    PB_CHECK_EQ(s.queue_recover().requeued, 1);
    const auto back = s.queue_item(id.value);
    PB_CHECK(back->state == pbc::ItemState::Queued);
    PB_CHECK_EQ(back->upload_id, std::string("pbc-feedface"));   // the next send resumes this upload folder
    PB_CHECK_EQ(back->checksum, std::string("00ff"));
    PB_CHECK_EQ(back->src_size.value_or(-1), 500LL << 20);
    const auto again = s.queue_recover();                        // the same call twice gives the same result
    PB_CHECK_EQ(again.requeued, 0);
    PB_CHECK_EQ(again.unknown, 0);
    PB_CHECK(s.queue_item(id.value)->state == pbc::ItemState::Queued);
}

PB_TEST(a_kill_between_two_opens_of_the_store_leaves_the_same_picture) {
    pbtest::TmpDir d;
    int64_t single = 0, chunked = 0;
    {
        auto r = pbc::Store::open(d.file("data"), g_clock);
        PB_CHECK(r.ok());
        single = add_in_state(*r.value, "single.bin", pbc::ItemState::Sending, "single");
        chunked = add_in_state(*r.value, "chunked.bin", pbc::ItemState::Sending, "chunks");
        // the process dies here: the store is simply closed without recovery
    }
    auto r = pbc::Store::open(d.file("data"), g_clock);
    PB_CHECK(r.ok());
    PB_CHECK(r.value->queue_item(single)->state == pbc::ItemState::Sending);   // still the picture the crash left
    const auto rep = r.value->queue_recover();
    PB_CHECK_EQ(rep.unknown, 1);
    PB_CHECK_EQ(rep.requeued, 1);
    PB_CHECK(r.value->queue_item(single)->state == pbc::ItemState::Unknown);
    PB_CHECK(r.value->queue_item(chunked)->state == pbc::ItemState::Queued);
}

PB_TEST_MAIN()
