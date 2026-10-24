#pragma once
// The older versions of a file (specs/006-trash-and-versions, contracts/bridge-qml.md "VersionsBridge"). Every action is a tap of the user on a live
// connection: nothing is queued, retried or done in the background. The logic is in the core (trash_api.h); this class wraps it for QML and runs the
// requests on the Worker. Nothing here logs more than a code, a status and a count (rules R6, R8, N4, N11 of scripts/check-constitution.sh).

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

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

class VersionsBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)                 // closed | checking | loading | listed | empty | explainOff | explainUnknown | failed | working
    Q_PROPERTY(bool busy READ busy NOTIFY changed)                      // a request is running; a second action answers {errorKey: "busy"}
    Q_PROPERTY(bool offline READ offline NOTIFY changed)                // the reader has no Wi-Fi now
    Q_PROPERTY(QVariantMap availability READ availability NOTIFY changed)   // {trash, versions}: on | off | unknown, as the last check said
    Q_PROPERTY(QVariantMap message READ message NOTIFY changed)         // {key, code} of the last problem; no name, no server text
    Q_PROPERTY(bool canAct READ canAct NOTIFY changed)                  // the versions are listed, their app is on and the reader is online
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)           // the file's name through display_safe()
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)              // the shown page: {index, dateText, sizeText, future, chosen}; the current content is not a row
    Q_PROPERTY(int page READ page NOTIFY changed)                       // 0-based
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)                     // older versions kept (at most 200)
    Q_PROPERTY(bool truncated READ truncated NOTIFY changed)            // more versions exist than are listed
    Q_PROPERTY(QVariantMap result READ result NOTIFY changed)           // {kind, ok, key, code, outcomeUnknown, previousNotKept} of the last restore; no name

public:
    struct Config {
        pbc::Store* store = nullptr;
        Worker* worker = nullptr;
        pbc::HttpTransport* http = nullptr;
        const pbc::Clock* clock = nullptr;
        std::function<bool()> wifi_up;                                  // empty = online
        std::function<void(std::function<void(bool)>)> ensure_network;
        std::function<QString(int64_t epoch)> format_date;              // empty = the epoch as a number
        std::function<QString(int64_t epoch)> format_date_time;         // the date and time of a version; empty = format_date
    };

    explicit VersionsBridge(Config config, QObject* parent = nullptr);
    ~VersionsBridge() override;

    QString state() const { return state_; }
    bool busy() const { return busy_; }
    bool offline() const { return c_.wifi_up ? !c_.wifi_up() : false; }
    QVariantMap availability() const { return availability_map_; }
    QVariantMap message() const { return message_; }
    bool canAct() const;
    QString fileName() const { return file_name_; }
    QVariantList rows() const { return rows_; }
    int page() const { return page_; }
    int pageCount() const { return page_count_; }
    int pageSize() const { return page_size_; }
    void setPageSize(int n);
    int total() const { return static_cast<int>(list_.rows.size()); }
    bool truncated() const { return list_.truncated; }
    QVariantMap result() const { return result_; }

    // The Versions tap on the details of a file of a stored listing. {ok: true}, or {errorKey}: busy, file_gone (the entry is not in a stored
    // listing), trash_not_actionable (a folder, or an entry the user cannot use: nothing is sent), refresh_first (the file's id is not known),
    // no_network (nothing is sent). Replaces the screen state.
    Q_INVOKABLE QVariantMap open(const QString& remotePath);
    // The Reload tool: the same for the file that is open (after a stale file, a version that is gone, an unknown outcome).
    Q_INVOKABLE QVariantMap reload();
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();
    // Marks one version as the chosen one (`index` = the place in the whole list, the row's `index`). {ok: true}, or {errorKey: version_gone}.
    Q_INVOKABLE QVariantMap choose(int index);
    // The Restore tap for the chosen version: {needsConfirm: true, summary} - the file, the version's date and size, that the current content is kept as one more
    // version, Cancel as the default - and nothing is sent. Or {errorKey}: busy, versions_unavailable (the app is off or unclear), version_gone (nothing chosen),
    // no_network.
    Q_INVOKABLE QVariantMap restore();
    // After the confirmation: the stale check and ONE request on the worker; the result arrives as actionFinished (the list is then the one read afterwards).
    // {ok: true}, or {errorKey}: nothing_pending, busy, no_network.
    Q_INVOKABLE QVariantMap restoreConfirmed();
    // Drops the confirmation: nothing is sent.
    Q_INVOKABLE void cancelStep();

signals:
    void changed();
    void actionFinished(const QVariantMap& result);

private:
    struct Checked;
    struct Finished;
    void apply_checked(quint64 generation, const Checked& checked);
    void apply_finished(const Finished& finished);
    void with_network(quint64 generation, std::function<void()> go, std::function<void()> failed);
    QVariantMap start_check();
    void rebuild_rows();
    QVariantMap row_map(int index) const;
    QVariantMap refuse(const char* key) const;
    QString date_text(const std::optional<int64_t>& epoch) const;

    Config c_;
    QString state_ = QStringLiteral("closed");
    bool busy_ = false;
    bool waiting_network_ = false;
    quint64 generation_ = 0;
    pbc::TrashAvailability availability_;
    QVariantMap availability_map_;
    QVariantMap message_;
    QString file_name_;
    std::string path_;
    pbc::VersionList list_;                          // the versions as they were read, with the stamp of the stale check
    QVariantList rows_;
    int page_ = 0, page_count_ = 1, page_size_ = 10;
    int chosen_ = -1;                                // the place in list_.rows of the chosen version, -1 = none
    bool pending_ = false;                           // a confirmation was shown: restoreConfirmed() may send
    QVariantMap result_;
};
