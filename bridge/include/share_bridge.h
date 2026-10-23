#pragma once
// Share links of a file or folder (specs/004-share-links, contracts/bridge-qml.md "ShareBridge"). Every sharing action is a tap of the user on a
// live connection: nothing is queued, retried, cached or stored, and the bridge never makes a request on its own. A link and a password are
// secrets with a short life: the link exists here only while its detail screen is shown, a typed password is handed to the core as a SecretText and
// every QString that held either is overwritten in place (clearSecrets). Nothing here logs more than a code, a status and a count, and nothing
// opens a link (constitution IV; rules N8 and N9 of scripts/check-constitution.sh). The logic is in the core (share_api.h); this class wraps it
// for QML and runs the requests on the Worker.

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
#include "pbc/share_diagnose.h"
#include "pbc/share_rules.h"
#include "pbc/share_types.h"
#include "pbc/store.h"
#include "pbc/types.h"

class Worker;

class ShareBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)                 // closed | loading | listed | failed | working
    Q_PROPERTY(bool busy READ busy NOTIFY changed)                      // a sharing request is running; a second action answers {errorKey: "busy"}
    Q_PROPERTY(bool offline READ offline NOTIFY changed)                // the reader has no Wi-Fi now
    Q_PROPERTY(QVariantMap message READ message NOTIFY changed)         // {key, text, hint, field, limitDays, code}; no secret, no server text
    Q_PROPERTY(QVariantMap detail READ detail NOTIFY changed)           // the open link; linkText is the only property that carries a link
    Q_PROPERTY(QVariantMap rules READ rules NOTIFY changed)             // what the server says about sharing, for the controls
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)              // the shown page of shares; never a link or a token
    Q_PROPERTY(bool clipboardAvailable READ clipboardAvailable CONSTANT)
    Q_PROPERTY(QString itemName READ itemName NOTIFY changed)           // the item's name through display_safe()
    Q_PROPERTY(bool isFolder READ isFolder NOTIFY changed)
    Q_PROPERTY(bool itemShareable READ itemShareable NOTIFY changed)    // false for an unavailable entry: the page shows unavailableReason
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY changed)
    Q_PROPERTY(int page READ page NOTIFY changed)                       // 0-based
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)                     // rows kept (at most 500)
    Q_PROPERTY(bool truncated READ truncated NOTIFY changed)            // more shares exist than can be listed
    Q_PROPERTY(QString inheritedNote READ inheritedNote NOTIFY changed) // "" | inherited_failed
    Q_PROPERTY(bool staleNote READ staleNote NOTIFY changed)            // the rows may be out of date (an unknown outcome, or the connection was lost)
    Q_PROPERTY(bool hasExistingLink READ hasExistingLink NOTIFY changed)   // the listed shares contain a public link: a new one is a second link

public:
    struct Config {
        pbc::Store* store = nullptr;
        Worker* worker = nullptr;
        pbc::HttpTransport* http = nullptr;
        const pbc::Clock* clock = nullptr;
        std::function<bool()> wifi_up;                                  // empty = online
        // for a tap that needs the network: brings Wi-Fi up if needed, then calls done(online). Empty = a tap while offline is refused with no_network.
        std::function<void(std::function<void(bool)>)> ensure_network;
        std::function<QString(const std::string& iso_day)> format_date; // empty = the ISO text
        // The system clipboard (R-11): set_clipboard puts text there; clear_clipboard_if_equal clears it only when it still holds exactly that text.
        std::function<void(const QString&)> set_clipboard;
        std::function<void(const QString&)> clear_clipboard_if_equal;
        bool clipboard_available = false;                               // a constant of the build (app/main.cpp), not a probe of the platform
    };

    explicit ShareBridge(Config config, QObject* parent = nullptr);
    ~ShareBridge() override;

    QString state() const { return state_; }
    bool busy() const { return busy_; }
    bool offline() const { return c_.wifi_up ? !c_.wifi_up() : false; }
    QVariantMap message() const { return message_; }
    QVariantMap detail() const;
    QVariantMap rules() const { return rules_; }
    QVariantList rows() const { return rows_; }
    bool clipboardAvailable() const { return c_.clipboard_available; }
    QString itemName() const { return item_name_; }
    bool isFolder() const { return target_.is_folder; }
    bool itemShareable() const { return shareable_; }
    QString unavailableReason() const { return unavailable_reason_; }
    int page() const { return page_; }
    int pageCount() const { return page_count_; }
    int pageSize() const { return page_size_; }
    void setPageSize(int n);
    int total() const { return static_cast<int>(all_rows_.size()); }
    bool truncated() const { return truncated_; }
    QString inheritedNote() const { return inherited_note_; }
    bool staleNote() const { return stale_; }
    bool hasExistingLink() const { return has_link_; }

    // The Share tap for an entry of a stored listing: reads the rules and the shares of the item (three requests, on the worker). {ok: true}, or
    // {errorKey}: busy (a request is running), share_item_gone (the entry is not in a stored listing), share_item_not_shareable (nothing is
    // sent), no_network (nothing is sent). Replaces the screen state.
    Q_INVOKABLE QVariantMap open(const QString& remotePath);
    // Reads the same three again (the Refresh tool and after a change). Same answers; offline the rows stay and are marked stale.
    Q_INVOKABLE QVariantMap refresh();
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();

    // Creates a public link. `options`: {permission: "read" | "upload", password, expiry: "YYYY-MM-DD" or "", confirmed}. Without `confirmed` it
    // validates and returns {needsConfirm: true, summary} (what the link allows, in plain words) and sends nothing; the password string is not touched.
    // With `confirmed` it validates again and, when nothing is wrong, hands the password to the core, overwrites the QString in place and sends ONE
    // request on the worker: {ok: true}; the result arrives as createFinished. Input the rules or the form refuse is answered at once: {errorKey, field}
    // (and `message` is set). Other answers: busy, no_network, share_item_gone. Nothing is sent without a connection and nothing is repeated.
    Q_INVOKABLE QVariantMap create(const QVariantMap& options);
    // Changes the open link, one explicit action at a time. `options`: {action: "set_password" | "clear_password" | "set_expiry" | "clear_expiry" |
    // "set_permission", password, expiry, permission: "read" | "upload", confirmed}. Without `confirmed` it validates and returns {needsConfirm, effectText}
    // (what the change does, in plain words) and sends nothing; the password string is not touched. With it: the share is read again, compared with what the
    // screen showed, and ONE change is sent on the worker: {ok: true}, result as changeFinished. A link of someone else, input the rules refuse, no connection
    // and a running request are answered at once ({errorKey, field}). A link that changed meanwhile is NOT overwritten: the current state is shown and the user
    // confirms again.
    Q_INVOKABLE QVariantMap change(qlonglong id, const QVariantMap& options);
    // Removes the open public link. Without `confirmed`: {needsConfirm, effectText} - names the item and says that the link stops working at once for everyone
    // and that the item itself is not deleted - and nothing is sent. With it: ONE request on the worker, {ok: true}; the result arrives as removeFinished.
    // A link of someone else, no connection, a running request: answered at once ({errorKey}).
    Q_INVOKABLE QVariantMap remove(qlonglong id, bool confirmed);
    // "YYYY-MM-DD" as the reader shows dates (the language setting); the text itself when it is not a date.
    Q_INVOKABLE QString dateText(const QString& isoDay) const;
    // Drops the message of an earlier refusal (a form opens clean).
    Q_INVOKABLE void clearMessage();

    // Shows one link on the link screen (used by showLink() and by tests): fills `detail` and moves a closed bridge to `listed`.
    void setDetail(const pbc::ShareDetail& detail);
    // Puts exactly the open link on the clipboard; false when no clipboard is available, no link is open, or the link is unreadable.
    Q_INVOKABLE bool copyLink();
    // The tap on a public link of the list: reads that one share (one request, on the worker) and, when it arrives, fills `detail` and emits linkOpened.
    // {ok: true}, or {errorKey}: busy, no_network (nothing is sent), share_item_gone (nothing is open). A link that is gone shows the message and reads the
    // list again. The link is asked for again every time: nothing is kept between two visits.
    Q_INVOKABLE QVariantMap showLink(qlonglong id);
    // Leaving the link screen: drops the link only (overwritten in place, the clipboard asked to drop it); the list stays.
    Q_INVOKABLE void leaveLink();
    // Drops the link, the rows, the rules and the message, overwrites every QString that held a link, asks the clipboard to drop the link when it still
    // holds exactly that text, and closes the bridge. Called when a screen is left, when the account changes and when the application is not active.
    Q_INVOKABLE void clearSecrets();

public slots:
    void onApplicationStateChanged(Qt::ApplicationState state);   // anything but Active clears (R-15)
    void onConnectionChanged();                                   // an account that changed or went away clears

signals:
    void changed();
    // The answer of remove(): ok with an empty key when the link was removed, ok with "share_gone" when it was already gone, not ok with the key otherwise
    // (an unknown outcome too: the list is then marked as possibly out of date).
    void removeFinished(bool ok, const QString& errorKey);
    void changeFinished(bool ok, const QString& errorKey);   // the answer of change(): on success `detail` shows the state of the server
    void linkOpened(qlonglong id);   // showLink() delivered the link: `detail` holds it
    void createFinished(bool ok, const QString& errorKey);   // the answer of create(): on success `detail` holds the new link
    void closed();   // clearSecrets() closed an open screen: pages still on the stack pop themselves

private:
    struct Loaded;
    struct Created;
    void apply_created(quint64 generation, const std::shared_ptr<Created>& created);
    void apply_detail(quint64 generation, qlonglong id, const std::shared_ptr<Created>& result);
    void apply_changed(quint64 generation, const std::shared_ptr<Created>& result);
    void apply_removed(quint64 generation, const std::shared_ptr<Created>& result);
    void rebuild_detail_map();
    QString effect_text(const pbc::LinkChange& change) const;
    void drop_link();
    qint64 today_days() const;
    QString summary_text(const pbc::LinkRequest& request) const;
    QVariantMap start_load(bool keep_message = false);
    // Runs `go` when the reader is online: at once, or after ensure_network brought Wi-Fi up (busy_ is set meanwhile). Offline for good: `failed`.
    // Nothing runs when the screen was left meanwhile (generation).
    void with_network(quint64 generation, std::function<void()> go, std::function<void()> failed);
    void offline_load_failed();
    static std::shared_ptr<Created> offline_result();
    void apply_loaded(quint64 generation, const Loaded& loaded);
    void rebuild_rows();
    QVariantMap row_map(const pbc::ShareRow& row) const;
    QVariantMap rules_map(const pbc::ShareRules& rules) const;
    static QVariantMap message_map(const pbc::ShareMessage& m);
    static void wipe(QString* s);
    QString account_key() const;
    QString permission_text(const pbc::ShareRow& row) const;
    QString expiry_text(const pbc::ShareRow& row) const;

    Config c_;
    QString state_ = QStringLiteral("closed");
    bool busy_ = false;
    QVariantMap message_, rules_, detail_map_;
    QVariantList rows_;
    pbc::ShareTarget target_;
    pbc::ShareRules core_rules_;
    std::vector<pbc::ShareRow> all_rows_;
    QString item_name_, unavailable_reason_, inherited_note_;
    bool shareable_ = true, truncated_ = false, stale_ = false, has_link_ = false;
    int page_ = 0, page_count_ = 1, page_size_ = 10;
    std::optional<int64_t> server_epoch_;   // the server's clock from the last answer: its day decides what "today" is for an expiry date
    bool waiting_network_ = false;   // ensure_network is running: the system's own Wi-Fi dialog may take the focus, which must not close the screen
    quint64 generation_ = 0;   // raised by clearSecrets(): an answer that arrives for an older screen is dropped
    pbc::ShareDetail detail_;
    QString link_text_;
    QString opened_key_;
};
