#pragma once
// What the trash bridge and the versions bridge share (specs/006-trash-and-versions): the account and credentials of one request, the shape of the
// answer about the server's apps, and the key of the message a failed check shows. Header only, Qt for the maps. Nothing here logs.

#include <QString>
#include <QVariantMap>

#include <memory>
#include <string>

#include "pbc/auth_guard.h"
#include "pbc/credentials_file.h"
#include "pbc/dav_common.h"
#include "pbc/error_keys.h"
#include "pbc/http_transport.h"
#include "pbc/secure_wipe.h"
#include "pbc/store.h"
#include "pbc/trash_types.h"
#include "pbc/types.h"

namespace trash_support {

// One session for the requests of one job, built on the worker: the stored account, the app password (wiped as soon as the session holds it) and the
// guard that stops requests after refused logins. `ok()` false: no request may be sent, error() says why.
class Online {
public:
    Online(pbc::Store& store, pbc::HttpTransport& http) : store_(store), guard_(store) {
        const auto account = store.account();
        if (!account || guard_.blocked()) {
            error_ = pbc::error_from_key("auth_failed");
            return;
        }
        auto creds = pbc::load_credentials(store.directory());
        if (!creds.ok() || creds.value.password.empty()) {
            error_ = pbc::error_from_key("auth_failed");
            return;
        }
        session_ = std::make_unique<pbc::DavSession>(http, account->base_url, account->login_name, creds.value.password);
        pbc::secure_wipe(creds.value.password);
        user_id_ = account->user_id;
    }
    bool ok() const { return session_ != nullptr; }
    const pbc::Error& error() const { return error_; }
    pbc::DavSession& dav() { return *session_; }
    const std::string& user_id() const { return user_id_; }
    void record(const pbc::Error& e) { guard_.record(e); }   // the outcome of a request that carried the credentials

private:
    pbc::Store& store_;
    pbc::AuthGuard guard_;
    std::unique_ptr<pbc::DavSession> session_;
    std::string user_id_;
    pbc::Error error_;
};

inline const char* feature_key(pbc::FeatureState s) {
    switch (s) {
        case pbc::FeatureState::On: return "on";
        case pbc::FeatureState::Off: return "off";
        case pbc::FeatureState::Unknown: break;
    }
    return "unknown";
}

inline QVariantMap availability_map(const pbc::TrashAvailability& a) {
    QVariantMap m;
    m.insert(QStringLiteral("trash"), QString::fromLatin1(feature_key(a.trash)));
    m.insert(QStringLiteral("versions"), QString::fromLatin1(feature_key(a.versions)));
    return m;
}

// {key, code}: the key picks the text in qml/Problems.qml, the code (T<op>-<status>) is appended by the page. No name, no server text.
inline QVariantMap message_map(const std::string& key, const std::string& code = std::string()) {
    QVariantMap m;
    if (key.empty()) return m;
    m.insert(QStringLiteral("key"), QString::fromStdString(key));
    m.insert(QStringLiteral("code"), QString::fromStdString(code));
    return m;
}

// A failure the user must act on differently from "the answer was unclear": no connection, a wrong password, a certificate.
inline bool is_connection_problem(const pbc::Error& e) {
    return e.cls == pbc::ErrorClass::Network || e.cls == pbc::ErrorClass::Tls || e.cls == pbc::ErrorClass::Auth || e.cls == pbc::ErrorClass::Cancelled;
}

}  // namespace trash_support
