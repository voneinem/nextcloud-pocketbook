// When Auto Sync sends the queue (specs/002-manage-server-files, research R-14, spec Story 2): a quiet period after the user's own action, once. The
// scheduler takes the time as a parameter, so every rule is proven without waiting.
#include "pbc/auto_sync.h"
#include "test_support.h"

using pbc::AutoSyncDecision;
using pbc::AutoSyncInputs;
using pbc::AutoSyncScheduler;
using pbc::AutoSyncStep;

namespace {
AutoSyncInputs ready() {
    AutoSyncInputs in;
    in.enabled = true;
    in.has_account = true;
    in.send_running = false;
    in.sendable_items = true;
    in.wifi_up = true;
    return in;
}
}  // namespace

PB_TEST(nothing_happens_until_the_user_acts) {
    AutoSyncScheduler s;
    PB_CHECK(!s.armed());
    PB_CHECK(s.next(0, ready()).step == AutoSyncStep::Idle);
    PB_CHECK(s.next(1000000, ready()).step == AutoSyncStep::Idle);   // time passing is not an action
    PB_CHECK(!s.armed());
}

PB_TEST(an_action_arms_it_and_the_start_comes_after_the_quiet_period) {
    AutoSyncScheduler s;   // 10 s
    s.note_action(1000);
    PB_CHECK(s.armed());
    AutoSyncDecision d = s.next(1000, ready());
    PB_CHECK(d.step == AutoSyncStep::Wait);
    PB_CHECK_EQ(d.wait_ms, 10000);
    d = s.next(6000, ready());
    PB_CHECK(d.step == AutoSyncStep::Wait);
    PB_CHECK_EQ(d.wait_ms, 5000);
    d = s.next(10999, ready());
    PB_CHECK(d.step == AutoSyncStep::Wait);
    PB_CHECK_EQ(d.wait_ms, 1);
    PB_CHECK(s.next(11000, ready()).step == AutoSyncStep::Start);
    PB_CHECK(s.next(50000, ready()).step == AutoSyncStep::Start);   // still due until a send starts
}

PB_TEST(every_new_action_restarts_the_quiet_period_so_close_actions_are_sent_together) {
    AutoSyncScheduler s;
    for (int64_t t : {0, 3000, 6000, 9000, 12000}) s.note_action(t);
    AutoSyncDecision d = s.next(15000, ready());
    PB_CHECK(d.step == AutoSyncStep::Wait);
    PB_CHECK_EQ(d.wait_ms, 7000);
    PB_CHECK(s.next(21999, ready()).step == AutoSyncStep::Wait);
    PB_CHECK(s.next(22000, ready()).step == AutoSyncStep::Start);
}

PB_TEST(a_custom_quiet_period_is_honored_so_that_tests_run_in_milliseconds) {
    AutoSyncScheduler s(50);
    s.note_action(0);
    PB_CHECK_EQ(s.next(10, ready()).wait_ms, 40);
    PB_CHECK(s.next(50, ready()).step == AutoSyncStep::Start);
}

PB_TEST(with_auto_sync_off_or_without_an_account_it_disarms_and_stays_quiet) {
    AutoSyncScheduler s;
    s.note_action(0);
    AutoSyncInputs off = ready();
    off.enabled = false;
    PB_CHECK(s.next(20000, off).step == AutoSyncStep::Idle);
    PB_CHECK(!s.armed());
    PB_CHECK(s.next(30000, ready()).step == AutoSyncStep::Idle);   // switching it on later does not revive an old action
    s.note_action(40000);
    AutoSyncInputs none = ready();
    none.has_account = false;
    PB_CHECK(s.next(60000, none).step == AutoSyncStep::Idle);
    PB_CHECK(!s.armed());
}

PB_TEST(a_running_send_keeps_it_armed_for_the_single_follow_up) {
    AutoSyncScheduler s;
    s.note_action(0);
    AutoSyncInputs running = ready();
    running.send_running = true;
    PB_CHECK(s.next(15000, running).step == AutoSyncStep::Idle);
    PB_CHECK(s.armed());                                          // an action made during a send is still to be sent
    PB_CHECK(s.next(16000, ready()).step == AutoSyncStep::Start);  // the send has ended: the follow-up
}

PB_TEST(nothing_a_run_could_take_disarms) {
    AutoSyncScheduler s;
    s.note_action(0);
    AutoSyncInputs empty = ready();
    empty.sendable_items = false;
    PB_CHECK(s.next(20000, empty).step == AutoSyncStep::Idle);
    PB_CHECK(!s.armed());
}

PB_TEST(no_wifi_at_the_end_of_the_quiet_period_drops_the_send_and_nothing_retries_it) {
    AutoSyncScheduler s;
    s.note_action(0);
    AutoSyncInputs offline = ready();
    offline.wifi_up = false;
    PB_CHECK(s.next(5000, offline).step == AutoSyncStep::Wait);   // the quiet period is checked first
    PB_CHECK(s.next(10000, offline).step == AutoSyncStep::Idle);
    PB_CHECK(!s.armed());
    PB_CHECK(s.next(20000, ready()).step == AutoSyncStep::Idle);   // Wi-Fi coming up later starts nothing
    PB_CHECK(s.next(1000000, ready()).step == AutoSyncStep::Idle);
}

PB_TEST(a_send_that_started_after_the_last_action_disarms_one_that_started_before_does_not) {
    AutoSyncScheduler s;
    s.note_action(1000);
    s.send_started(5000);   // the user tapped Send: it read what was queued
    PB_CHECK(!s.armed());
    PB_CHECK(s.next(30000, ready()).step == AutoSyncStep::Idle);

    s.note_action(40000);
    s.send_started(30000);  // a send that began before the action did not read it (the clock of a late note_action)
    PB_CHECK(s.armed());
    PB_CHECK(s.next(60000, ready()).step == AutoSyncStep::Start);
}

PB_TEST(ten_actions_a_minute_four_seconds_apart_cause_one_start_per_quiet_period_not_one_per_action) {
    AutoSyncScheduler s;
    int starts = 0;
    int64_t send_ends = -1;
    for (int64_t t = 0; t <= 80000; t += 1000) {
        if (t <= 36000 && t % 4000 == 0) s.note_action(t);
        AutoSyncInputs in = ready();
        in.send_running = t < send_ends;
        const AutoSyncDecision d = s.next(t, in);
        if (d.step == AutoSyncStep::Start) {
            ++starts;
            s.send_started(t);
            send_ends = t + 2000;   // a send takes two seconds
        }
    }
    PB_CHECK_EQ(starts, 1);   // all ten actions are 4 s apart: the quiet period never ends until the last one
}

PB_TEST(a_start_is_never_given_without_an_action_since_the_last_send) {
    AutoSyncScheduler s;
    s.note_action(0);
    PB_CHECK(s.next(10000, ready()).step == AutoSyncStep::Start);
    s.send_started(10000);
    for (int64_t t = 10000; t < 200000; t += 10000) PB_CHECK(s.next(t, ready()).step == AutoSyncStep::Idle);
}

PB_TEST_MAIN()
