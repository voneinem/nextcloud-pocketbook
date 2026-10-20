#pragma once
// The connection to the user's Nextcloud (specs/001-nextcloud-files-client, contracts/bridge-qml.md): the Test, the browser login,
// sign-out and the single account check at start. Every request runs on the Worker; nothing is sent unless a tap (or the one call of
// main.cpp at start) asked for it. The typed app password is passed through and never kept.

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "pbc/auth_guard.h"
#include "pbc/http_transport.h"
#include "pbc/nc_login.h"
#include "pbc/store.h"
#include "pbc/types.h"

class Worker;

class ConnectionBridge : public QObject {
    Q_OBJECT
    // none | untested | ok | needs_signin | problem | offline (the reader had no Wi-Fi at start and nothing was checked)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(bool hasAccount READ hasAccount NOTIFY changed)
    Q_PROPERTY(QString address READ address NOTIFY changed)
    Q_PROPERTY(QString loginName READ loginName NOTIFY changed)
    Q_PROPERTY(QString serverName READ serverName NOTIFY changed)
    Q_PROPERTY(QString serverVersion READ serverVersion NOTIFY changed)
    // the quota in pieces, so that the page can put the words around them in the reader's language
    Q_PROPERTY(QString quotaUsedText READ quotaUsedText NOTIFY changed)
    Q_PROPERTY(QString quotaTotalText READ quotaTotalText NOTIFY changed)
    Q_PROPERTY(bool quotaUnlimited READ quotaUnlimited NOTIFY changed)
    Q_PROPERTY(qint64 checkedAt READ checkedAt NOTIFY changed)
    Q_PROPERTY(QString checkedAtText READ checkedAtText NOTIFY changed)   // the time of the last account check in the reader's zone, "" = never
    Q_PROPERTY(QString problemKey READ problemKey NOTIFY changed)
    Q_PROPERTY(int clockWarningMinutes READ clockWarningMinutes NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool loginActive READ loginActive NOTIFY changed)
    Q_PROPERTY(QString loginUrl READ loginUrl NOTIFY changed)
    Q_PROPERTY(QString qrImageUrl READ qrImageUrl NOTIFY changed)
    Q_PROPERTY(int loginExpiresInMinutes READ loginExpiresInMinutes NOTIFY changed)
    // enforced | not_enforced | unknown: what the file system really does with the mode of the credentials file
    Q_PROPERTY(QString credentialsProtection READ credentialsProtection NOTIFY changed)

public:
    struct Config {
        std::function<QString(qint64)> format_date_time;   // a time in the reader's zone
        pbc::Store* store = nullptr;
        Worker* worker = nullptr;
        pbc::HttpTransport* http = nullptr;
        const pbc::Clock* clock = nullptr;
        // true when the reader is online now (never turns Wi-Fi on)
        std::function<bool()> wifi_up;
        // for a tap that needs the network: brings Wi-Fi up if needed, then calls done(online)
        std::function<void(std::function<void(bool)>)> ensure_network;
    };

    explicit ConnectionBridge(Config config, QObject* parent = nullptr);
    ~ConnectionBridge() override;

    QString state() const;
    bool hasAccount() const { return account_.has_value(); }
    QString address() const;
    QString loginName() const;
    QString serverName() const;
    QString serverVersion() const;
    QString quotaUsedText() const;
    QString quotaTotalText() const;
    bool quotaUnlimited() const;
    qint64 checkedAt() const;
    QString checkedAtText() const;
    QString problemKey() const { return problem_; }
    int clockWarningMinutes() const { return clock_warning_; }
    bool busy() const { return busy_; }
    bool loginActive() const { return flow_.has_value(); }
    QString loginUrl() const;
    QString qrImageUrl() const;
    int loginExpiresInMinutes() const;
    QString credentialsProtection() const;

    // Checks address, user name and app password; saves them only if everything works.
    Q_INVOKABLE void testConnection(const QString& address, const QString& loginName, const QString& appPassword);
    Q_INVOKABLE void startLogin(const QString& address);
    Q_INVOKABLE void checkLogin();     // exactly one request
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE void signOut();
    // Called once by main.cpp: one request, and only when the reader is already online.
    Q_INVOKABLE void checkAtStart();
    Q_INVOKABLE void refresh();        // re-reads the account row (no request)

    // The link of the running browser login, a copy that is safe to read from the image loader thread (the QR image provider).
    std::string loginLinkSnapshot() const {
        std::lock_guard<std::mutex> lock(link_mu_);
        return link_copy_;
    }

signals:
    void changed();
    void testFinished(bool ok, const QString& key);
    void loginChecked(const QString& status);   // approved | pending | expired | failed

private:
    struct Outcome;
    void finish_test(bool ok, const QString& key);
    void load_account();
    void publish_link();   // updates the snapshot after the flow changed

    Config c_;
    pbc::AuthGuard guard_;
    std::optional<pbc::AccountRow> account_;
    QString problem_;
    int clock_warning_ = 0;
    bool busy_ = false;
    bool offline_ = false;
    std::optional<pbc::LoginStart> flow_;
    std::string flow_base_;
    int64_t flow_started_ = 0;
    int qr_counter_ = 0;
    mutable std::mutex link_mu_;
    std::string link_copy_;
};
