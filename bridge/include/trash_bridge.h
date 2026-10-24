#pragma once
// The server's trash (specs/006-trash-and-versions, contracts/bridge-qml.md "TrashBridge"). Every action is a tap of the user on a live connection:
// nothing is queued, retried or done in the background, and the bridge never makes a request on its own. The logic is in the core (trash_api.h,
// trash_rules.h); this class wraps it for QML and runs the requests on the Worker. Nothing here logs more than a code, a status and a count, and no
// name or location leaves the bridge unsanitized (rules R6, R8, N4, N11 of scripts/check-constitution.sh).

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pbc/http_transport.h"
#include "pbc/store.h"
#include "pbc/trash_types.h"
#include "pbc/types.h"

class Worker;

class TrashBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)                 // closed | checking | loading | listed | explainOff | explainUnknown | failed | working
    Q_PROPERTY(bool busy READ busy NOTIFY changed)                      // a request is running; a second action answers {errorKey: "busy"}
    Q_PROPERTY(bool offline READ offline NOTIFY changed)                // the reader has no Wi-Fi now
    Q_PROPERTY(QVariantMap availability READ availability NOTIFY changed)   // {trash, versions}: on | off | unknown, as the last check said
    Q_PROPERTY(QVariantMap message READ message NOTIFY changed)         // {key, code} of the last problem; no name, no server text
    Q_PROPERTY(bool canAct READ canAct NOTIFY changed)                  // the list was just read from the server, its app is on and the reader is online: the tools may be offered
    Q_PROPERTY(bool fromStore READ fromStore NOTIFY changed)            // the shown list is the stored copy of an earlier visit (no connection, or the server could not be reached)
    Q_PROPERTY(QString ageKind READ ageKind NOTIFY changed)             // how old the shown list is: not_synced | just_now | minutes | hours | days
    Q_PROPERTY(int ageCount READ ageCount NOTIFY changed)
    Q_PROPERTY(bool clockSuspect READ clockSuspect NOTIFY changed)      // the list claims to be from the future: the reader's clock is wrong
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)              // the shown page: {index, name, originalText, deletedText, sizeText, isFolder, unreadable, actionable, chosen}
    Q_PROPERTY(int page READ page NOTIFY changed)                       // 0-based
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)                     // items kept (at most 1,000)
    Q_PROPERTY(bool truncated READ truncated NOTIFY changed)            // more items exist than are listed
    Q_PROPERTY(QString chosenName READ chosenName NOTIFY changed)       // the display name of the chosen item, "" when none
    Q_PROPERTY(bool deleteEnabled READ deleteEnabled CONSTANT)          // the irreversible actions (Delete for good, Empty trash) may be offered: the constitution allows them on this screen
    Q_PROPERTY(QVariantMap progress READ progress NOTIFY changed)       // {done, total, removed, notRemoved} of an Empty trash run; updated about every tenth item
    Q_PROPERTY(QVariantMap result READ result NOTIFY changed)           // {kind, ok, key, code, outcomeUnknown, landedElsewhere} of the last action; no name, no location

public:
    struct Config {
        pbc::Store* store = nullptr;
        Worker* worker = nullptr;
        pbc::HttpTransport* http = nullptr;
        const pbc::Clock* clock = nullptr;
        std::function<bool()> wifi_up;                                  // empty = online
        // for a tap that needs the network: brings Wi-Fi up if needed, then calls done(online). Empty = a tap while offline is refused with no_network.
        std::function<void(std::function<void(bool)>)> ensure_network;
        std::function<QString(int64_t epoch)> format_date;              // empty = the epoch as a number
        bool delete_enabled = false;                                    // false: Delete for good and Empty trash cannot even begin (answered not_available)
    };

    explicit TrashBridge(Config config, QObject* parent = nullptr);
    ~TrashBridge() override;

    QString state() const { return state_; }
    bool busy() const { return busy_; }
    bool offline() const { return c_.wifi_up ? !c_.wifi_up() : false; }
    QVariantMap availability() const { return availability_map_; }
    QVariantMap message() const { return message_; }
    bool canAct() const;
    bool fromStore() const { return from_store_; }
    QString ageKind() const;
    int ageCount() const;
    bool clockSuspect() const;
    QVariantList rows() const { return rows_; }
    int page() const { return page_; }
    int pageCount() const { return page_count_; }
    int pageSize() const { return page_size_; }
    void setPageSize(int n);
    int total() const { return static_cast<int>(items_.size()); }
    bool truncated() const { return truncated_; }
    QString chosenName() const;
    QVariantMap result() const { return result_; }
    bool deleteEnabled() const { return c_.delete_enabled; }
    QVariantMap progress() const { return progress_; }

    // The Trash tap: asks the server whether the trash app is on and, when it is, reads the trash (two requests, on the worker, then the list is
    // stored). {ok: true}, or {errorKey}: busy (a request is running), no_network (nothing is sent; the stored list of an earlier visit is shown, marked
    // with its age). Replaces the screen state.
    Q_INVOKABLE QVariantMap open();
    // The Refresh tool: the same as open(). The chosen item stays chosen only if it is still in the list.
    Q_INVOKABLE QVariantMap refresh();
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();
    // Marks one item as the chosen one (a second call moves the mark). `index` is the place in the whole list (the row's `index`). {ok: true}, or
    // {errorKey}: trash_item_gone (no such place), trash_not_actionable (an unreadable item, or one the user cannot act on).
    Q_INVOKABLE QVariantMap choose(int index);

    // ---- Restore (specs/006-trash-and-versions US2) ----
    // The Restore tap for the chosen item: looks at its original place (one or two probes, on the worker) and answers with restorePlan(plan, proposal): plan is
    // original_free | name_taken | parent_missing | choose_place; the proposal is the confirmation (kind, names, place, originalPlace, destructive false,
    // defaultCancel true) of a restore to the original place, or, for the other plans, the place that cannot be used. Nothing is moved. {ok: true}, or {errorKey}:
    // busy, trash_unavailable (the trash app is off or unclear), trash_item_gone (nothing is chosen), no_network (the actions need a connection).
    Q_INVOKABLE QVariantMap restore();
    // After the confirmation of the proposed original place: ONE request on the worker; the result arrives as actionFinished. {ok: true}, or {errorKey}:
    // nothing_pending (no proposal, or it was cancelled), busy, no_network.
    Q_INVOKABLE QVariantMap restoreConfirmed();
    // The folder the user chose with the chooser. The name proposed is the original name (or the item's name). The answer arrives as targetChecked: {needsConfirm,
    // summary, folder, name} for a free place, {needsName, folder, name} (a free name to start from) when the name is taken there, {errorKey} otherwise.
    // {ok: true}, or {errorKey}: nothing_pending, busy, no_network, restore_parent_missing, restore_into_self.
    Q_INVOKABLE QVariantMap chooseTarget(const QString& folderPath);
    // A new name for the chosen folder; checked with the rename rules at once ({errorKey: name_forbidden}), then probed; the answer arrives as targetChecked.
    // There is no way to replace what is there: a taken name always asks for another.
    Q_INVOKABLE QVariantMap setTargetName(const QString& name);
    // After the confirmation of the chosen place: ONE request with Overwrite: F; the result arrives as actionFinished. Errors as restoreConfirmed.
    Q_INVOKABLE QVariantMap restoreChosenConfirmed();
    // Drops the proposal, the chosen place and any confirmation of an irreversible step (Cancel, leaving the screen): nothing is sent.
    Q_INVOKABLE void cancelStep();

    // ---- Delete for good and Empty trash (specs/006-trash-and-versions US4; constitution 1.3.0, rule N12) ----
    // The Delete for good tap for the chosen item: creates the confirmation chain (one confirmation for a file, two for a folder) and answers with the content of the
    // first one: {kind: delete_file | delete_folder, names, items, folders, stage: 1, destructive: true, defaultCancel: true}. Nothing is sent. {errorKey}: not_available
    // (the actions are switched off), busy, trash_unavailable, trash_item_gone (nothing chosen), trash_not_actionable, no_network.
    Q_INVOKABLE QVariantMap beginDelete();
    // The Empty trash tap: the confirmation chain over exactly the items of the list as held now (the actionable ones); the first confirmation says the number of items
    // and folders and names up to five. Nothing is sent. {errorKey} as beginDelete, trash_item_gone for nothing to delete.
    Q_INVOKABLE QVariantMap beginEmpty();
    // The user confirmed the confirmation that is shown: answers with the content of the next one ({stage: 2}), or at the last stage starts the deletion on the worker
    // ({started: true}; the result arrives as actionFinished). {errorKey}: nothing_pending, busy, no_network. This is the only place that deletes from the trash.
    Q_INVOKABLE QVariantMap confirmStep();
    // Stops an Empty trash run between two requests (the request in flight finishes). Safe to call from any thread.
    Q_INVOKABLE void cancelRun();

public slots:
    // The application was switched away: a confirmation that was shown is dropped. A request in flight is not touched.
    void onApplicationStateChanged(Qt::ApplicationState state);
    // The connection or the account changed: a confirmation is dropped; the list of another account is not shown.
    void onConnectionChanged();

signals:
    void changed();
    void restorePlan(const QString& plan, const QVariantMap& proposal);
    void targetChecked(const QVariantMap& answer);
    void actionFinished(const QVariantMap& result);

private:
    struct Checked;
    void apply_checked(quint64 generation, const Checked& checked);
    void with_network(quint64 generation, std::function<void()> go, std::function<void()> failed);
    QVariantMap start_check();
    bool show_stored(const std::string& message_key);    // the stored list of an earlier visit, marked as such; false when there is none
    void rebuild_rows();
    QVariantMap row_map(int index) const;
    void clear_list();
    // restore
    struct Pending;
    struct Probed;
    struct Finished;
    struct Destruction;
    struct Destroyed;
    QVariantMap refuse(const char* key) const;
    QString account_key() const;
    QVariantMap check_action(bool needs_chosen);
    QVariantMap destruction_content(const Destruction& d) const;
    void run_destruction();
    void apply_destroyed(const Destroyed& done);
    void remove_items(const std::vector<std::string>& trash_names);
    void probe_target(const pbc::RestoreTarget& target);
    void run_restore(bool original);
    void apply_finished(const Finished& finished);
    void remove_item(const std::string& trash_name);

    Config c_;
    QString state_ = QStringLiteral("closed");
    bool busy_ = false;
    bool waiting_network_ = false;
    quint64 generation_ = 0;
    pbc::TrashAvailability availability_;
    QVariantMap availability_map_;
    QVariantMap message_;
    std::vector<pbc::TrashItem> items_;              // the whole list, newest first
    QVariantList rows_;
    std::optional<int64_t> fetched_at_;
    bool from_store_ = false, truncated_ = false;
    int page_ = 0, page_count_ = 1, page_size_ = 10;
    int chosen_ = -1;                                // the place in items_ of the chosen item, -1 = none
    std::string chosen_trash_name_;                  // its identity: it survives a refresh only while the item is still there
    std::string unknown_trash_name_;                 // an item whose restore has an unknown outcome: marked until the next refresh
    QVariantMap result_;
    quint64 epoch_ = 0;                              // advanced by cancelStep: a probe that returns later is dropped
    std::shared_ptr<Pending> pending_;               // the restore in progress: its item, the proposal and the chosen place
    std::shared_ptr<Destruction> destruction_;       // the confirmation chain of an irreversible step: the named items and the stage
    std::shared_ptr<std::atomic<bool>> cancel_run_;  // set by cancelRun(): checked between two requests of an Empty trash run
    QVariantMap progress_;
    QString opened_key_;                             // the account the shown list belongs to
};
