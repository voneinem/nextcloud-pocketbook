#pragma once
// The queue of changes (specs/002-manage-server-files, contracts/bridge-qml.md): what the user's picks and actions put into it, and the
// queue as the page shows it. Everything the user does is written to the queue first (FR-006) and nothing is sent by a pick, a property read
// or the opening of the screen. The logic is in the core (queue_actions.h, queue_sender.h, auto_sync.h); this class wraps it for QML: the send
// runs on the Worker, started by a tap (Send, Retry) or by the one single-shot Auto Sync timer a short quiet period after the user's own action.
// The resolving of held items (choices for conflicts) is added by the later stories of the feature.

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pbc/auto_sync.h"
#include "pbc/http_transport.h"
#include "pbc/queue_actions.h"
#include "pbc/queue_sender.h"
#include "pbc/source_walk.h"
#include "pbc/store.h"
#include "pbc/types.h"

class Worker;

class QueueBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(int openCount READ openCount NOTIFY changed)               // items not done: what the Queue tool shows
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)              // the rows of the shown page
    Q_PROPERTY(int page READ page NOTIFY changed)                         // 0-based
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(bool offline READ offline NOTIFY changed)                  // the reader has no Wi-Fi now
    Q_PROPERTY(bool autoSync READ autoSync WRITE setAutoSync NOTIFY changed)   // the setting auto_sync, on unless the user switched it off
    Q_PROPERTY(bool running READ running NOTIFY changed)                  // a send is running
    Q_PROPERTY(QString runStage READ runStage NOTIFY changed)             // idle | preparing | sending | done
    Q_PROPERTY(QString runProgressText READ runProgressText NOTIFY changed)   // coarse: at most every 2 s or 5 %
    Q_PROPERTY(int runPercent READ runPercent NOTIFY changed)
    Q_PROPERTY(QString waitingNotice READ waitingNotice NOTIFY changed)   // "" | offline | rate_limited | maintenance | signin: why the last run stopped
    Q_PROPERTY(QString lastRunText READ lastRunText NOTIFY changed)       // when the last send ended, "" = never
    Q_PROPERTY(QVariantMap lastRunCounts READ lastRunCounts NOTIFY changed)   // {done, failed, conflict, unknown, left}
    Q_PROPERTY(QString checkResult READ checkResult NOTIFY changed)       // arrived | not_there | something_else | unclear | "" (no check yet)

public:
    struct Config {
        pbc::Store* store = nullptr;
        Worker* worker = nullptr;
        pbc::HttpTransport* http = nullptr;
        const pbc::Clock* clock = nullptr;
        pbc::SourceRules rules;
        std::function<bool()> wifi_up;   // empty = always online
        std::function<void(std::function<void(bool)>)> ensure_network;   // for a tap that needs the network (Send, Retry, Check); empty = online
        std::function<QString(qint64)> format_date_time;
        std::string download_root;
        int auto_sync_quiet_ms = 10000;   // the quiet period after the user's own action (tests use milliseconds)
        int progress_interval_ms = 2000;  // the coarse progress of a run: at most this often, or when it moves by progress_step_percent
        int progress_step_percent = 5;
        // Folder synchronization (specs/003-folder-sync): told the server folder of every action of the user that arms Auto Sync, so that a pair that holds
        // it can compare. Called only where this bridge already notes an action of its own, never for anything else.
        std::function<void(const std::string& remote_path)> on_user_action;
    };

    explicit QueueBridge(Config config, QObject* parent = nullptr);
    ~QueueBridge() override;

    int openCount() const { return open_count_; }
    QVariantList items() const { return items_; }
    int page() const { return page_; }
    int pageCount() const { return page_count_; }
    int pageSize() const { return page_size_; }
    void setPageSize(int n);
    int total() const { return total_; }
    bool offline() const { return c_.wifi_up ? !c_.wifi_up() : false; }   // read live; refresh() tells QML to read it again
    bool autoSync() const;
    void setAutoSync(bool on);
    bool running() const { return running_; }
    QString runStage() const { return run_stage_; }
    QString runProgressText() const { return run_progress_text_; }
    int runPercent() const { return run_percent_; }
    QString waitingNotice() const { return waiting_notice_; }
    QString lastRunText() const;
    QVariantMap lastRunCounts() const;
    QString checkResult() const { return check_result_; }

    // Files picked for a server folder. {queued, ids, errorKey, refused: [{name, key}], asks: []}. Nothing is sent.
    Q_INVOKABLE QVariantMap addUploads(const QString& targetFolder, const QStringList& paths);
    // Folders picked for a server folder: the walk and the numbers, nothing queued yet.
    // {token, items, folders, files, bytes, bytesText, skipped, overLimit, refused: [{name, key}], errorKey, headroom}
    Q_INVOKABLE QVariantMap addFolderPick(const QString& targetFolder, const QStringList& folderPaths);
    // Queues the counted pick as one group, once. {queued, errorKey, refused, asks}. `policy` answers every taken name inside the pick at once:
    // "keep_both", "replace" or "skip"; "" asks.
    Q_INVOKABLE QVariantMap confirmFolderPick(const QString& token, const QString& policy = QString());
    // The user's answer to one question of the last result: choice "keep_both" (newName "" = the proposal), "replace" (the page has asked its
    // confirmation first) or "skip". {queued, errorKey, refused, asks}: a keep-both name that is taken as well comes back as a new question.
    Q_INVOKABLE QVariantMap answerCollision(const QString& askId, const QString& choice, const QString& newName = QString());
    // New folder in `parent` and Rename of an entry of a cached listing: validated at once, queued, nothing sent. {queued, exists, errorKey}: `exists`
    // is true when the name is taken (the page shows the existing entry); errorKey is name_forbidden, name_exists, refresh_first, not_allowed, queue_full.
    Q_INVOKABLE QVariantMap newFolder(const QString& parent, const QString& name);
    Q_INVOKABLE QVariantMap rename(const QString& remotePath, const QString& newName);
    // Move or copy entries of the shown listing into a folder: kind "move" | "copy". {queued, errorKey, refused: [{name, key}], asks}. Entries that cannot
    // be queued come back in `refused` with their reason (refresh_first, change_pending, not_allowed, name_exists); taken names are questions.
    Q_INVOKABLE QVariantMap moveOrCopy(const QString& kind, const QStringList& remotePaths, const QString& targetFolder);
    // Delete entries of the shown listing. Without `confirmed`: the data of the confirmation {names, entries, folders, itemsInside, deleteOffered}, nothing
    // queued. With it: queues one item per entry, {queued, errorKey, refused} (errorKey trash_unavailable when the server has no trash: nothing is queued).
    Q_INVOKABLE QVariantMap deleteEntries(const QStringList& remotePaths, bool confirmed);
    // A held item (conflict): looks at the server for what the second confirmation shows, result in resolvePrepared. Changes nothing.
    Q_INVOKABLE void prepareResolve(int id);
    // The user's choice for a held item: keep_both (newName "" = the proposal), replace_anyway or apply_anyway (after prepareResolve), discard / skip /
    // keep_entry. Result in resolveFinished.
    Q_INVOKABLE void resolve(int id, const QString& choice, const QString& newName = QString());
    // Remove: an upload that has an upload folder on the server deletes that folder first (best effort); an item that is being sent is refused.
    Q_INVOKABLE void remove(int id);
    // The Send tap: ensures the network (as in feature 001), then runs the queue on the worker. Ignored while a send runs.
    Q_INVOKABLE void send();
    Q_INVOKABLE void cancelSend();   // stops after the piece in flight; the item stays resumable
    Q_INVOKABLE void retry(int id);  // a failed item goes back to the queue and is sent alone
    // An item whose outcome is unknown: the read-only check (result in checkResult and checkFinished), the user's decision that it arrived, or sending it again.
    Q_INVOKABLE void checkUnknown(int id);
    Q_INVOKABLE void markDone(int id);
    Q_INVOKABLE void sendAgain(int id);
    Q_INVOKABLE void leaveScreen();   // done rows go when the user leaves the queue screen
    Q_INVOKABLE void refresh();       // re-reads the page and the Wi-Fi state; no request
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();
    Q_INVOKABLE void goToPage(int page);

signals:
    void changed();
    void sendFinished(const QVariantMap& report);       // {done, failed, conflict, unknown, left, stoppedBy}
    void checkFinished(int id, const QString& result);
    // {ok, exists, name, sizeText, dateText, errorKey}: what the server has now for the item (the target of an upload, the entry of another change)
    void resolvePrepared(int id, const QVariantMap& info);
    void resolveFinished(int id, const QString& errorKey);

private:
    struct FlagCancel : pbc::Cancel {
        std::atomic<bool> flag{false};
        bool requested() const override { return flag.load(); }
    };
    void reload();
    void start_run(bool ensure_network, int64_t only);
    void finish_run(const pbc::SendReport& report);
    void note_action(const std::vector<std::string>& remote_folders = {});   // the user's own action: arms Auto Sync (and tells the pairs that hold these folders)
    void evaluate_auto_sync();     // asks the scheduler what to do now
    void on_timer();
    qint64 now_ms() const { return mono_.elapsed(); }
    std::unique_ptr<pbc::QueueSender> make_sender(const std::function<void(const pbc::SendProgress&)>& on_progress);
    QVariantMap row_map(const pbc::QueueItem& item, const std::optional<pbc::AccountRow>& account) const;
    QVariantMap result_map(const pbc::AddResult& r);
    QVariantMap ask_map(const pbc::NameAsk& ask);

    Config c_;
    pbc::QueueActions actions_;
    int open_count_ = 0, page_ = 0, page_count_ = 1, page_size_ = 12, total_ = 0;
    QVariantList items_;
    std::map<QString, pbc::FolderPick> picks_;
    std::deque<QString> pick_order_;
    int pick_counter_ = 0;
    std::map<QString, pbc::NameAsk> asks_;
    std::deque<QString> ask_order_;
    int ask_counter_ = 0;
    std::map<int64_t, std::string> prepared_etag_;   // the version the user was shown in the second confirmation, per item

    FlagCancel cancel_;
    pbc::SystemRandom random_;
    pbc::AutoSyncScheduler scheduler_;
    QTimer timer_;
    QElapsedTimer mono_;
    bool running_ = false;
    QString run_stage_ = QStringLiteral("idle");
    QString run_progress_text_;
    int run_percent_ = 0;
    qlonglong run_item_ = 0;
    QString waiting_notice_;
    QString check_result_;
};
