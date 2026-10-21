#include "pbc/auto_sync.h"

namespace pbc {

void AutoSyncScheduler::note_action(int64_t now_ms) {
    armed_ = true;
    last_action_ms_ = now_ms;
}

void AutoSyncScheduler::send_started(int64_t now_ms) {
    if (armed_ && now_ms >= last_action_ms_) armed_ = false;   // the send read what the user did; an action after it is still to be sent
}

AutoSyncDecision AutoSyncScheduler::next(int64_t now_ms, const AutoSyncInputs& in) {
    AutoSyncDecision d;
    if (!armed_) return d;
    if (!in.enabled || !in.has_account) {
        armed_ = false;
        return d;
    }
    if (in.send_running) return d;   // stays armed: asked again when that send ends (the single follow-up)
    if (!in.sendable_items) {
        armed_ = false;
        return d;
    }
    const int64_t due = last_action_ms_ + quiet_ms_;
    if (now_ms < due) {
        d.step = AutoSyncStep::Wait;
        d.wait_ms = static_cast<int>(due - now_ms);
        return d;
    }
    if (!in.wifi_up) {   // the queue waits; nothing retries by itself (FR-013)
        armed_ = false;
        return d;
    }
    d.step = AutoSyncStep::Start;
    return d;
}

}  // namespace pbc
