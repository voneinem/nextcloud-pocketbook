#pragma once
// When Auto Sync sends the queue (specs/002-manage-server-files, research R-14, constitution Principle VI): a short quiet period after the user's own
// action, once, and never otherwise. The scheduler holds no timer and reads no clock - time is a parameter - so every rule is proven on the host without
// waiting. The one single-shot timer lives in the queue bridge. Pattern of the scheduler of notes-pocketbook (MIT, same author; NOTICE).
// Qt-free.

#include <cstdint>

namespace pbc {

enum class AutoSyncStep { Idle, Wait, Start };

struct AutoSyncInputs {
    bool enabled = true;          // the Auto Sync setting
    bool has_account = true;
    bool send_running = false;
    bool sendable_items = false;  // something in the queue that a run could take
    bool wifi_up = true;
};

struct AutoSyncDecision {
    AutoSyncStep step = AutoSyncStep::Idle;
    int wait_ms = 0;              // for Wait: how long until the quiet period is over
};

class AutoSyncScheduler {
public:
    explicit AutoSyncScheduler(int quiet_ms = 10000) : quiet_ms_(quiet_ms) {}

    // The user's own action (queuing, removing, a choice on a held item): arms the scheduler and restarts the quiet period.
    void note_action(int64_t now_ms);
    // A send started, manual or automatic: it read everything queued so far, so a send that started after the last action disarms.
    void send_started(int64_t now_ms);
    // The rules, in order: not armed -> Idle; Auto Sync off or no account -> disarm, Idle; a send is running -> Idle (stay armed: the single follow-up when
    // it ends); nothing a run could take -> disarm, Idle; the quiet period is not over -> Wait; no Wi-Fi -> disarm, Idle (the queue waits; no retry);
    // else Start.
    AutoSyncDecision next(int64_t now_ms, const AutoSyncInputs& in);
    bool armed() const { return armed_; }

private:
    int quiet_ms_;
    bool armed_ = false;
    int64_t last_action_ms_ = 0;
};

}  // namespace pbc
