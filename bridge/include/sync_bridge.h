#pragma once
// Folder synchronization for QML (specs/003-folder-sync, contracts/bridge-qml.md): the pairs the user made between a server folder and a reader
// folder and what a comparison and a run of them do. The logic is in the core (sync_rules.h, sync_runner.h, sync_scheduler.h); this class wraps it.
// Saving, changing or removing a pair writes its rows and nothing else: no request, no file changed (FR-003). A comparison (never a transfer)
// starts from a tap, from the start-up hook and from the one single-shot Auto Sync timer; a transfer starts from the Approve tap only.

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <deque>
#include <functional>
#include <set>
#include <string>

#include "pbc/downloader.h"
#include "pbc/queue_sender.h"
#include "pbc/http_transport.h"
#include "pbc/source_walk.h"
#include "pbc/storage_info.h"
#include "pbc/store.h"
#include "pbc/sync_scheduler.h"
#include "pbc/sync_types.h"
#include "pbc/types.h"

class Worker;

class SyncBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList pairs READ pairs NOTIFY changed)   // [{id, remoteFolder, readerFolder, remotePath, readerPath, direction, directionText, state, stateText, ...}]
    Q_PROPERTY(int pairCount READ pairCount NOTIFY changed)
    Q_PROPERTY(int pairLimit READ pairLimit CONSTANT)
    Q_PROPERTY(bool offline READ offline NOTIFY changed)       // the reader has no Wi-Fi now
    Q_PROPERTY(bool busy READ busy NOTIFY changed)             // a comparison or a run is going
    Q_PROPERTY(qlonglong busyPairId READ busyPairId NOTIFY changed)
    Q_PROPERTY(QString stage READ stage NOTIFY changed)        // idle | comparing | downloading | uploading | applying | done
    Q_PROPERTY(QString progressText READ progressText NOTIFY changed)   // coarse: at most every 2 s or 5 %
    Q_PROPERTY(int percent READ percent NOTIFY changed)
    Q_PROPERTY(QVariantMap lastResult READ lastResult NOTIFY changed)    // the run that just finished: {pairId, downloaded, uploaded, ...}
    Q_PROPERTY(int readyCount READ readyCount NOTIFY changed)            // pairs with a plan that waits for the user (the mark on the Home tool)
    Q_PROPERTY(int planPageSize READ planPageSize WRITE setPlanPageSize NOTIFY changed)
    Q_PROPERTY(QString waitingNotice READ waitingNotice NOTIFY changed)  // "" | offline: an Auto Sync comparison found the reader without Wi-Fi and waits for the user

public:
    struct Config {
        pbc::Store* store = nullptr;
        Worker* worker = nullptr;
        pbc::HttpTransport* http = nullptr;
        const pbc::Clock* clock = nullptr;
        pbc::SourceRules rules;          // the picker root and the app's data folder
        std::string download_root;       // the download folder of feature 001
        std::function<bool()> wifi_up;   // empty = always online
        std::function<void(std::function<void(bool)>)> ensure_network;   // for a tap that needs the network; empty = online
        std::function<QString(qint64)> format_date_time;
        pbc::StorageInfo* storage = nullptr;   // free space for the plan and the downloader's checks
        pbc::DownloaderOptions downloader_options;
        std::function<void(const char*)> runner_hook;   // test seam: the steps of the runner (plan_written, approved, before_download, ...)
        std::function<void(const char*)> sender_hook;   // test seam: the steps of the queue's sender that sends a plan's uploads
        int progress_interval_ms = 2000;       // the coarse progress of a run: at most this often, or when it moves by progress_step_percent
        int progress_step_percent = 5;
        int auto_sync_quiet_ms = 10000;        // the quiet period after the user's own change (tests use milliseconds)
    };

    explicit SyncBridge(Config config, QObject* parent = nullptr);
    ~SyncBridge() override;

    QVariantList pairs() const { return pairs_; }
    int pairCount() const { return static_cast<int>(pairs_.size()); }
    int pairLimit() const { return pbc::kMaxSyncPairs; }
    bool offline() const { return c_.wifi_up ? !c_.wifi_up() : false; }   // read live; refresh() tells QML to read it again
    bool busy() const { return busy_; }
    qlonglong busyPairId() const { return busy_pair_; }
    QString stage() const { return stage_; }
    QString progressText() const { return progress_text_; }
    int percent() const { return percent_; }
    QVariantMap lastResult() const { return last_result_; }
    int readyCount() const { return ready_count_; }
    int planPageSize() const { return plan_page_size_; }
    void setPlanPageSize(int n);
    QString waitingNotice() const { return waiting_pairs_.empty() ? QString() : QStringLiteral("offline"); }

    // {remoteFolder, readerFolder, direction, maxFiles, maxBytes, startupSync, autoSync, confirmed} -> {ok, id} or {ok: false, errorKey}. A sending
    // direction without `confirmed` is refused (confirmation_needed). Writes the pair only: no request, no file change.
    Q_INVOKABLE QVariantMap addPair(const QVariantMap& spec);
    // The same keys, all optional; a change to a sending direction needs `confirmed`. Takes effect at the next comparison.
    Q_INVOKABLE QVariantMap updatePair(qlonglong id, const QVariantMap& spec);
    // Removes the rows of the pair (records, plans) and nothing else. {ok, queueItemsKept}
    Q_INVOKABLE QVariantMap removePair(qlonglong id);
    // The plain-language summary of a direction, shown before saving (FR-002); "" for anything else.
    Q_INVOKABLE QString describeDirection(const QString& direction) const;
    Q_INVOKABLE void refresh();   // re-reads the pairs and the Wi-Fi state; no request

    // The Sync tap: ensures the network as feature 001 does, then compares on the worker (scans, plan; no file is changed, nothing is queued). A pair that is
    // comparing or running answers sync_busy through comparisonFailed. Offline: comparisonFailed with no_network and nothing is compared.
    Q_INVOKABLE void compare(qlonglong id);
    // The same for every usable pair, one after the other; planReady is emitted for the first plan that waits for the user.
    Q_INVOKABLE void compareAll();
    // What is known from the stored listings, with no request: the plan in memory, flagged stale and incomplete, not approvable. plan(0) reads it.
    Q_INVOKABLE void previewOffline(qlonglong id);
    // The header of a plan (planId 0: the offline preview): {id, pairId, createdText, ageKind, ageCount, trigger, state, totals{...}, sizes{...}, freeText,
    // quotaText, reserveText, overLimit, incomplete, incompleteFolders, truncated, unapprovable, approvable, approveErrorKey, stale, removedCount}
    Q_INVOKABLE QVariantMap plan(qlonglong planId) const;
    // One page of a plan: section transfers | skips | proposals; rows {seq, nameText, localNameText, action, sizeText, reason, unapprovable, removed, allowed, choice}
    Q_INVOKABLE QVariantList planPage(qlonglong planId, const QString& section, int page) const;
    Q_INVOKABLE int planPageCount(qlonglong planId, const QString& section) const;
    // Takes a transfer entry out of the plan or puts it back (only while the plan is ready). {ok, errorKey}
    Q_INVOKABLE QVariantMap removeEntry(qlonglong planId, int seq, bool removed);
    // The Approve tap, **the only way a transfer starts**: checks the approval, ensures the network, then runs the plan on the worker. {ok, errorKey}
    // (plan_stale, plan_not_approvable, plan_too_large, over_limit, sync_busy, account_changed, no_network).
    Q_INVOKABLE QVariantMap approve(qlonglong planId);
    // The user's choice for one proposal: transfer_again, leave, delete, or "" to clear it; only a choice the entry allows. Changes nothing but the plan row.
    Q_INVOKABLE QVariantMap setChoice(qlonglong planId, int seq, const QString& choice);
    // Without `confirmed`: the data of the confirmation, {ok, deletes: [{nameText, side, trash, folder, recordsBelow}], transfers, leaves}; nothing is applied.
    // With it: SyncRunner::apply_choices on the worker (runFinished follows). A delete is never applied without `confirmed`; approve() never applies a choice.
    Q_INVOKABLE QVariantMap applyChoices(qlonglong planId, bool confirmed);
    Q_INVOKABLE void cancelRun();   // stops after the item in flight; what is done stays done
    Q_INVOKABLE QVariantMap cancelPlan(qlonglong planId);   // marks it cancelled; touches nothing else
    // The last result of a pair, from what was stored: {pairId, downloaded, uploaded, skipped, keptBoth, proposalsOpen, failed, seconds, failures: [{key, count}], stoppedBy, atText}
    Q_INVOKABLE QVariantMap resultFor(qlonglong pairId) const;

    // The three ways a comparison starts besides a tap (research R-13). Each only stores a plan: nothing here approves, transfers, queues or deletes.
    // 1. At start, once, after the account check of feature 001 succeeded and only when Wi-Fi is already on: every usable pair with `startupSync`, one after
    //    the other. Never called for Wi-Fi coming up later.
    Q_INVOKABLE void startupCompare();
    // 2. Auto Sync: after the user's own change below a pair's server folder (noteAction, told by the queue bridge) or reader folder (noteLocalAction, told by
    //    the pair settings), a quiet period later, for the pairs with `autoSync` only and only while the global Auto Sync of Settings is on. The one
    //    single-shot timer fires the comparison without switching Wi-Fi on; offline it leaves the pair waiting and retries nothing.
    Q_INVOKABLE void noteAction(const QString& remotePath);
    Q_INVOKABLE void noteLocalAction(const QString& localPath);

signals:
    void changed();
    void planReady(qlonglong pairId, qlonglong planId);
    void previewReady(qlonglong pairId);
    void runFinished(const QVariantMap& result);
    void comparisonFailed(qlonglong pairId, const QString& errorKey);

private:
    void reload();
    QVariantMap pair_map(const pbc::SyncPair& pair, const std::optional<pbc::AccountRow>& account) const;
    QVariantMap apply_spec(pbc::SyncPair* pair, const QVariantMap& spec, bool adding);
    QVariantMap error_map(const std::string& key) const;

    struct FlagCancel : pbc::Cancel {
        std::atomic<bool> flag{false};
        bool requested() const override { return flag.load(); }
    };
    struct Pending {
        qlonglong id = 0;
        std::string trigger;   // tap | startup | auto
        bool ensure_network = true;
    };
    void start_compare(const Pending& what, bool then_next);
    void finish_compare(const Pending& what, const pbc::Result<int64_t>& result);
    void compare_next();
    void note_for_pairs(const std::vector<int64_t>& pair_ids);
    void evaluate_auto_sync();
    qint64 now_ms() const { return mono_.elapsed(); }
    void note_progress(const pbc::SyncResult& r, int n_download, int n_upload);
    QVariantMap header_of(const pbc::SyncPlan& plan, bool preview) const;
    QVariantMap row_of(const pbc::PlanEntry& e) const;
    QVariantMap result_map(int64_t pair_id, const std::string& json, std::optional<int64_t> at) const;
    int64_t ready_plan_of(const pbc::SyncPair& pair) const;

    pbc::SystemRandom random_;   // for the upload sessions of the queue's sender
    Config c_;
    QVariantList pairs_;
    FlagCancel cancel_;
    bool busy_ = false;
    qlonglong busy_pair_ = 0;
    QString stage_ = QStringLiteral("idle");
    QString progress_text_;
    int percent_ = 0, ready_count_ = 0, plan_page_size_ = 8;
    QVariantMap last_result_;
    std::deque<Pending> compare_queue_;      // compareAll and the unattended comparisons: still to do, one after the other
    qlonglong first_ready_pair_ = 0, first_ready_plan_ = 0;
    pbc::SyncScheduler scheduler_;
    QTimer timer_;                           // the one single-shot timer of Auto Sync (rule N3's allow-list for this file)
    std::set<qlonglong> armed_pairs_, waiting_pairs_;
    pbc::SyncPlan preview_;
    qlonglong preview_pair_ = 0;
    QElapsedTimer mono_;
    qint64 last_progress_ms_ = -1;
    int last_progress_percent_ = -100;
};
