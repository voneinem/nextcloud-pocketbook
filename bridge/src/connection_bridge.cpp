#include "connection_bridge.h"

#include <QMetaObject>

#include "format_util.h"
#include "pbc/credentials_file.h"
#include "pbc/error_keys.h"
#include "pbc/nc_server.h"
#include "pbc/secure_wipe.h"
#include "worker.h"

namespace {

constexpr int kLoginSeconds = 20 * 60;   // the validity of a Login Flow v2 token

QString q(const std::string& s) { return QString::fromStdString(s); }

pbc::AccountRow row_from(const std::string& base_url, const std::string& login_name, const pbc::AccountCheck& check, int64_t now) {
    pbc::AccountRow a;
    a.base_url = base_url;
    a.login_name = login_name;
    a.user_id = check.user_id;
    a.server_name = check.server.name;
    a.server_version = check.server.version;
    a.quota_used = check.quota.used;
    a.quota_total = check.quota.unlimited ? std::optional<int64_t>(-1) : check.quota.total;
    a.checked_at = now;
    a.state = "ok";
    return a;
}

// Stores the app password (the file first), then the account. One failure leaves the earlier state.
pbc::Error store_account(pbc::Store& store, const std::string& base_url, const std::string& login_name, std::string app_password,
                         const pbc::AccountCheck& check, int64_t now) {
    pbc::Credentials creds;
    creds.password = std::move(app_password);
    pbc::Error e = pbc::save_credentials(store.directory(), creds);
    pbc::secure_wipe(creds.password);
    if (!e.ok()) return e;
    return store.save_account(row_from(base_url, login_name, check, now));
}

}  // namespace

ConnectionBridge::ConnectionBridge(Config config, QObject* parent) : QObject(parent), c_(std::move(config)), guard_(*c_.store) { load_account(); }

ConnectionBridge::~ConnectionBridge() {
    if (c_.worker) c_.worker->wait_idle();   // no job may call back into a bridge that is gone
    if (flow_) pbc::login_discard(*flow_);
}

void ConnectionBridge::load_account() {
    account_ = c_.store->account();
    if (account_ && account_->state != "ok" && !account_->problem_key.empty() && problem_.isEmpty()) problem_ = q(account_->problem_key);
    if (account_ && account_->state == "ok") problem_.clear();
}

void ConnectionBridge::publish_link() {
    std::lock_guard<std::mutex> lock(link_mu_);
    link_copy_ = flow_ ? flow_->login_url : std::string();
}

void ConnectionBridge::refresh() {
    load_account();
    emit changed();
}

QString ConnectionBridge::state() const {
    if (!account_) return QStringLiteral("none");
    if (offline_ && account_->state != "needs_signin") return QStringLiteral("offline");
    return q(account_->state);
}
QString ConnectionBridge::address() const { return account_ ? q(account_->base_url) : QString(); }
QString ConnectionBridge::loginName() const { return account_ ? q(account_->login_name) : QString(); }
QString ConnectionBridge::serverName() const { return account_ ? display_safe(account_->server_name, 64) : QString(); }
QString ConnectionBridge::serverVersion() const { return account_ ? q(account_->server_version) : QString(); }
QString ConnectionBridge::quotaUsedText() const { return account_ && account_->quota_used ? human_size(*account_->quota_used) : QString(); }
QString ConnectionBridge::quotaTotalText() const {
    return account_ && account_->quota_total && *account_->quota_total >= 0 ? human_size(*account_->quota_total) : QString();
}
bool ConnectionBridge::quotaUnlimited() const { return account_ && account_->quota_total && *account_->quota_total == -1; }
QString ConnectionBridge::checkedAtText() const {
    const qint64 t = checkedAt();
    return t > 0 && c_.format_date_time ? c_.format_date_time(t) : QString();
}
qint64 ConnectionBridge::checkedAt() const { return account_ && account_->checked_at ? *account_->checked_at : 0; }
QString ConnectionBridge::loginUrl() const { return flow_ ? q(flow_->login_url) : QString(); }
QString ConnectionBridge::qrImageUrl() const { return flow_ ? QStringLiteral("image://qr/%1").arg(qr_counter_) : QString(); }

int ConnectionBridge::loginExpiresInMinutes() const {
    if (!flow_) return 0;
    const int64_t left = kLoginSeconds - (c_.clock->now() - flow_started_);
    return left <= 0 ? 0 : static_cast<int>((left + 59) / 60);
}

QString ConnectionBridge::credentialsProtection() const {
    switch (pbc::credentials_protection(c_.store->directory())) {
        case pbc::ProtectionStatus::Enforced: return QStringLiteral("enforced");
        case pbc::ProtectionStatus::NotEnforced: return QStringLiteral("not_enforced");
        default: return QStringLiteral("unknown");
    }
}

void ConnectionBridge::finish_test(bool ok, const QString& key) {
    busy_ = false;
    problem_ = ok ? QString() : key;
    emit changed();
    emit testFinished(ok, key);
}

void ConnectionBridge::testConnection(const QString& address, const QString& loginName, const QString& appPassword) {
    if (busy_) return;
    const auto canonical = pbc::canonical_base_url(address.toStdString());
    if (!canonical.ok()) {
        finish_test(false, q(canonical.error.key));
        return;
    }
    const std::string login = loginName.trimmed().toStdString();
    if (login.empty() || appPassword.isEmpty()) {
        finish_test(false, QStringLiteral("auth_failed"));
        return;
    }
    guard_.reset();   // an explicit Test lifts the block of two refused logins
    busy_ = true;
    emit changed();
    std::string password = appPassword.toStdString();
    const std::string base = canonical.value;
    auto done = [this, base, login, password = std::move(password)](bool online) mutable {
        if (!online) {
            pbc::secure_wipe(password);
            finish_test(false, QStringLiteral("no_network"));
            return;
        }
        c_.worker->post([this, base, login, password = std::move(password)]() mutable {
            auto check = pbc::check_account(*c_.http, base, login, password, *c_.clock);
            pbc::Error error = check.error;
            if (check.ok()) error = store_account(*c_.store, base, login, password, check.value, c_.clock->now());
            pbc::secure_wipe(password);
            const int warning = check.ok() ? check.value.clock_warning_minutes : 0;
            QMetaObject::invokeMethod(this, [this, error, warning] {
                if (error.ok()) {
                    guard_.record(pbc::Error());
                    offline_ = false;
                    clock_warning_ = warning;
                    load_account();
                }
                finish_test(error.ok(), q(error.key));
            }, Qt::QueuedConnection);
        });
    };
    c_.ensure_network(std::move(done));
}

void ConnectionBridge::startLogin(const QString& address) {
    if (busy_) return;
    const auto canonical = pbc::canonical_base_url(address.toStdString());
    if (!canonical.ok()) {
        problem_ = q(canonical.error.key);
        emit changed();
        emit loginChecked(QStringLiteral("failed"));
        return;
    }
    if (flow_) pbc::login_discard(*flow_);
    flow_.reset();
    publish_link();
    busy_ = true;
    emit changed();
    const std::string base = canonical.value;
    c_.ensure_network([this, base](bool online) {
        if (!online) {
            busy_ = false;
            problem_ = QStringLiteral("no_network");
            emit changed();
            return;
        }
        c_.worker->post([this, base] {
            auto r = pbc::login_start(*c_.http, base);
            QMetaObject::invokeMethod(this, [this, base, r = std::move(r)]() mutable {
                busy_ = false;
                if (r.ok()) {
                    flow_ = std::move(r.value);
                    publish_link();
                    flow_base_ = base;
                    flow_started_ = c_.clock->now();
                    ++qr_counter_;
                    problem_.clear();
                } else {
                    problem_ = q(r.error.key);
                }
                emit changed();
            }, Qt::QueuedConnection);
        });
    });
}

void ConnectionBridge::cancelLogin() {
    if (flow_) pbc::login_discard(*flow_);
    flow_.reset();
    publish_link();
    emit changed();
}

void ConnectionBridge::checkLogin() {
    if (!flow_ || busy_) return;
    if (c_.clock->now() - flow_started_ >= kLoginSeconds) {   // the token is no longer valid: no request for a flow that is over
        pbc::login_discard(*flow_);
        flow_.reset();
        publish_link();
        emit changed();
        emit loginChecked(QStringLiteral("expired"));
        return;
    }
    busy_ = true;
    emit changed();
    pbc::LoginStart copy = *flow_;
    const std::string base = flow_base_;
    c_.ensure_network([this, base, copy = std::move(copy)](bool online) mutable {
        if (!online) {
            pbc::login_discard(copy);
            busy_ = false;
            problem_ = QStringLiteral("no_network");
            emit changed();
            emit loginChecked(QStringLiteral("failed"));
            return;
        }
        c_.worker->post([this, base, copy = std::move(copy)]() mutable {
            QString status, key;
            bool flow_over = false;
            auto r = pbc::login_check(*c_.http, base, copy);
            if (!r.ok()) {
                key = q(r.error.key);
                status = r.error.key == "login_expired" ? QStringLiteral("expired") : QStringLiteral("failed");
            } else if (!r.value) {
                status = QStringLiteral("pending");
            } else {
                // approved: the answer comes only once, so the account is checked and stored right now
                auto check = pbc::check_account(*c_.http, r.value->server, r.value->login_name, r.value->app_password, *c_.clock);
                pbc::Error e = check.error;
                if (check.ok()) e = store_account(*c_.store, r.value->server, r.value->login_name, r.value->app_password, check.value, c_.clock->now());
                pbc::secure_wipe(r.value->app_password);
                status = e.ok() ? QStringLiteral("approved") : QStringLiteral("failed");
                key = q(e.key);
            }
            flow_over = copy.token.empty();   // login_check discards the flow when it ends
            QMetaObject::invokeMethod(this, [this, status, key, flow_over] {
                busy_ = false;
                if (flow_over) {
                    if (flow_) pbc::login_discard(*flow_);
                    flow_.reset();
                    publish_link();
                }
                if (status == QLatin1String("approved")) {
                    guard_.reset();
                    offline_ = false;
                    problem_.clear();
                    load_account();
                } else if (status != QLatin1String("pending")) {
                    problem_ = key;
                }
                emit changed();
                emit loginChecked(status);
            }, Qt::QueuedConnection);
        });
    });
}

void ConnectionBridge::signOut() {
    // the password file goes first: there must never be an account whose password the user believes is gone
    const pbc::Error e = pbc::forget_credentials(c_.store->directory());
    if (!e.ok()) {
        problem_ = q(e.key);
        emit changed();
        return;
    }
    if (flow_) pbc::login_discard(*flow_);
    flow_.reset();
    publish_link();
    c_.store->clear_account();
    c_.store->set_setting("auth_fail_count", "0");
    offline_ = false;
    problem_.clear();
    clock_warning_ = 0;
    load_account();
    emit changed();
}

void ConnectionBridge::checkAtStart() {
    if (!account_ || busy_) return;
    if (guard_.blocked()) return;   // two refused logins: nothing is sent until the user tests again
    if (!c_.wifi_up || !c_.wifi_up()) {   // never turns Wi-Fi on by itself
        offline_ = true;
        emit changed();
        return;
    }
    busy_ = true;
    emit changed();
    const pbc::AccountRow account = *account_;
    c_.worker->post([this, account] {
        pbc::Error error;
        int warning = 0;
        auto creds = pbc::load_credentials(c_.store->directory());
        if (!creds.ok() || creds.value.password.empty()) {
            error = pbc::make_error(pbc::ErrorClass::Auth, "auth_failed", "there is no app password");
        } else {
            auto check = pbc::quick_check(*c_.http, account.base_url, account.login_name, creds.value.password, *c_.clock);
            error = check.error;
            if (check.ok()) {
                warning = check.value.clock_warning_minutes;
                pbc::AccountRow row = account;
                row.server_name = check.value.server.name;
                row.server_version = check.value.server.version;
                row.checked_at = c_.clock->now();
                row.state = "ok";
                row.problem_key.clear();
                c_.store->save_account(row);
            }
        }
        pbc::secure_wipe(creds.value.password);
        guard_.record(error);
        if (!error.ok() && !guard_.blocked()) c_.store->set_account_state("problem", error.key);
        QMetaObject::invokeMethod(this, [this, error, warning] {
            busy_ = false;
            offline_ = false;
            clock_warning_ = warning;
            problem_ = error.ok() ? QString() : q(error.key);
            load_account();
            emit changed();
        }, Qt::QueuedConnection);
    });
}
