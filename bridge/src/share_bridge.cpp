#include "share_bridge.h"

#include <QMetaObject>
#include <QPointer>

#include <algorithm>

#include "format_util.h"
#include "pbc/auth_guard.h"
#include "pbc/credentials_file.h"
#include "pbc/error_keys.h"
#include "pbc/remote_path.h"
#include "pbc/secure_wipe.h"
#include "pbc/share_api.h"
#include "pbc/share_date.h"
#include "worker.h"

namespace {

const char* link_state_key(pbc::LinkState s) {
    switch (s) {
        case pbc::LinkState::Ok: return "ok";
        case pbc::LinkState::DifferentAddress: return "different_address";
        case pbc::LinkState::NotSecure: return "not_secure";
        case pbc::LinkState::Unreadable: break;
    }
    return "unreadable";
}

}  // namespace

ShareBridge::ShareBridge(Config config, QObject* parent) : QObject(parent), c_(std::move(config)) {}

ShareBridge::~ShareBridge() {
    if (c_.worker) c_.worker->wait_idle();   // no job may call back into a bridge that is gone
    wipe(&link_text_);
    pbc::secure_wipe(detail_.link);
}

void ShareBridge::wipe(QString* s) {
    // The buffer may be shared with copies that the QML engine or a caller holds; overwriting it in place wipes those too (QString::fill would
    // detach and leave the shared buffer untouched). Nothing writes to a QString buffer while another thread reads it: all of this runs on the UI thread.
    if (!s->isEmpty()) pbc::secure_wipe(const_cast<QChar*>(s->constData()), static_cast<size_t>(s->size()) * sizeof(QChar));
    s->clear();
}

QString ShareBridge::account_key() const {
    const auto a = c_.store ? c_.store->account() : std::nullopt;
    return a ? QString::fromStdString(a->base_url + "\n" + a->user_id) : QString();
}

QString ShareBridge::permission_text(const pbc::ShareRow& row) const {
    switch (row.permission) {
        case pbc::LinkPermission::ReadOnly: return bridge_tr("Read only");
        case pbc::LinkPermission::ReadUpload: return bridge_tr("Read and upload");
        case pbc::LinkPermission::UploadOnly: return bridge_tr("Upload only (existing link)");
        case pbc::LinkPermission::Other: break;
    }
    return bridge_tr("Custom permission");
}

QString ShareBridge::expiry_text(const pbc::ShareRow& row) const {
    if (row.expires_unreadable) return bridge_tr("Expiry date unreadable");
    if (!row.expires) return bridge_tr("No expiry");
    const QString date = c_.format_date ? c_.format_date(*row.expires) : QString::fromStdString(*row.expires);
    return bridge_tr("Expires %1").arg(date);
}

QVariantMap ShareBridge::detail() const {
    if (detail_.row.id == 0) return QVariantMap();
    QVariantMap m = detail_map_;
    m.insert(QStringLiteral("linkText"), link_text_);   // shares the buffer with link_text_: clearSecrets() wipes every copy in place
    return m;
}

void ShareBridge::rebuild_detail_map() {
    QVariantMap m;
    m.insert(QStringLiteral("id"), static_cast<qlonglong>(detail_.row.id));
    m.insert(QStringLiteral("permissionText"), permission_text(detail_.row));
    m.insert(QStringLiteral("expiryText"), expiry_text(detail_.row));
    m.insert(QStringLiteral("passwordText"), detail_.row.has_password ? bridge_tr("Password set") : bridge_tr("No password"));
    m.insert(QStringLiteral("canChange"), detail_.row.kind == pbc::ShareKind::PublicLink && detail_.row.owned_by_me);
    m.insert(QStringLiteral("passwordValue"), detail_.row.has_password ? bridge_tr("Set") : bridge_tr("None"));   // for the row labelled Password
    m.insert(QStringLiteral("expiryValue"), detail_.row.expires_unreadable ? bridge_tr("Expiry date unreadable")
                                             : detail_.row.expires ? dateText(QString::fromStdString(*detail_.row.expires)) : bridge_tr("Never"));   // for the row labelled Expires
    m.insert(QStringLiteral("hasPassword"), detail_.row.has_password);
    m.insert(QStringLiteral("expires"), detail_.row.expires ? QString::fromStdString(*detail_.row.expires) : QString());
    m.insert(QStringLiteral("permission"), QString::fromLatin1(detail_.row.permission == pbc::LinkPermission::ReadOnly ? "read" : detail_.row.permission == pbc::LinkPermission::ReadUpload ? "upload" : "other"));
    m.insert(QStringLiteral("linkState"), QString::fromLatin1(link_state_key(detail_.link_state)));
    m.insert(QStringLiteral("linkLength"), link_text_.size());
    detail_map_ = m;
}

void ShareBridge::setDetail(const pbc::ShareDetail& detail) {
    wipe(&link_text_);
    pbc::secure_wipe(detail_.link);
    detail_ = detail;
    link_text_ = QString::fromStdString(detail.link);
    rebuild_detail_map();
    if (state_ == QLatin1String("closed")) {
        state_ = QStringLiteral("listed");
        opened_key_ = account_key();
    }
    emit changed();
}

bool ShareBridge::copyLink() {
    if (!c_.clipboard_available || !c_.set_clipboard || link_text_.isEmpty() || detail_.link_state == pbc::LinkState::Unreadable) return false;
    c_.set_clipboard(link_text_);
    return true;
}

// Wipes the link wherever the bridge holds it and asks the clipboard to drop it when it still holds exactly that text.
void ShareBridge::drop_link() {
    if (!link_text_.isEmpty() && c_.clear_clipboard_if_equal) c_.clear_clipboard_if_equal(link_text_);   // before the wipe: the compare needs the text
    wipe(&link_text_);
    pbc::secure_wipe(detail_.link);
    detail_ = pbc::ShareDetail();
    detail_map_.clear();
}

void ShareBridge::leaveLink() {
    if (detail_.row.id == 0 && link_text_.isEmpty()) return;
    drop_link();
    emit changed();
}

void ShareBridge::clearSecrets() {
    const bool was_open = state_ != QLatin1String("closed");
    ++generation_;
    drop_link();
    message_.clear();
    rules_.clear();
    rows_.clear();
    opened_key_.clear();
    all_rows_.clear();
    item_name_.clear();
    unavailable_reason_.clear();
    inherited_note_.clear();
    target_ = pbc::ShareTarget();
    core_rules_ = pbc::ShareRules();
    shareable_ = true;
    truncated_ = stale_ = has_link_ = false;
    page_ = 0;
    page_count_ = 1;
    busy_ = false;
    waiting_network_ = false;
    state_ = QStringLiteral("closed");
    if (!was_open) return;
    emit changed();
    emit closed();
}

void ShareBridge::onApplicationStateChanged(Qt::ApplicationState state) {
    if (state != Qt::ApplicationActive && !waiting_network_) clearSecrets();
}

void ShareBridge::onConnectionChanged() {
    if (state_ != QLatin1String("closed") && account_key() != opened_key_) clearSecrets();
}

// ---- the list (US2) ----

struct ShareBridge::Loaded {
    pbc::Error error;
    pbc::ShareMessage message;
    bool have_rules = false;
    pbc::ShareRules rules;
    pbc::ShareApi::Listing listing;
    std::optional<int64_t> server_time;
};

namespace {

// Runs on the worker: the account, the credentials, the three reads. The password is wiped as soon as the session holds it.
void load_shares(const ShareBridge::Config& c, const pbc::ShareTarget& target, pbc::Error* error, pbc::ShareMessage* message, bool* have_rules,
                 pbc::ShareRules* rules, pbc::ShareApi::Listing* listing, std::optional<int64_t>* server_time) {
    const auto account = c.store->account();
    pbc::AuthGuard guard(*c.store);
    if (!account || guard.blocked()) {
        *error = pbc::error_from_key("auth_failed");
        message->key = "auth_failed";
        return;
    }
    auto creds = pbc::load_credentials(c.store->directory());
    if (!creds.ok() || creds.value.password.empty()) {
        *error = pbc::error_from_key("auth_failed");
        message->key = "auth_failed";
        return;
    }
    pbc::DavSession session(*c.http, account->base_url, account->login_name, creds.value.password);
    pbc::secure_wipe(creds.value.password);
    pbc::ShareApi api(session, *c.clock, account->user_id);
    const auto r = api.fetch_rules();
    guard.record(r.error);
    *server_time = api.server_time();
    if (!r.ok()) {
        *error = r.error;
        message->key = r.error.key;
        return;
    }
    *have_rules = true;
    *rules = r.value;
    if (!pbc::sharing_offered(r.value)) {
        *error = pbc::error_from_key("share_not_offered");
        message->key = "share_not_offered";
        return;
    }
    auto l = api.list(target);
    guard.record(l.error);
    *error = l.error;
    *message = api.message();
    *listing = std::move(l.value);
    *server_time = api.server_time();
}

}  // namespace

void ShareBridge::setPageSize(int n) {
    n = std::max(1, std::min(n, 100));
    if (n == page_size_) return;
    const int first = page_ * page_size_;
    page_size_ = n;
    page_ = first / page_size_;
    rebuild_rows();
    emit changed();
}

QVariantMap ShareBridge::message_map(const pbc::ShareMessage& m) {
    QVariantMap out;
    if (m.key.empty()) return out;
    out.insert(QStringLiteral("key"), QString::fromStdString(m.key));
    const char* field = m.field == pbc::ShareMessage::Field::Password ? "password" : m.field == pbc::ShareMessage::Field::Expiry ? "expiry"
                       : m.field == pbc::ShareMessage::Field::Permission ? "permission" : "";
    out.insert(QStringLiteral("field"), QString::fromLatin1(field));
    out.insert(QStringLiteral("fieldMask"), static_cast<int>(m.field_mask));
    out.insert(QStringLiteral("limitDays"), m.limit_days ? *m.limit_days : 0);
    out.insert(QStringLiteral("code"), QString::fromStdString(m.code));
    return out;
}

QVariantMap ShareBridge::rules_map(const pbc::ShareRules& r) const {
    QVariantMap out;
    out.insert(QStringLiteral("sharingOffered"), pbc::sharing_offered(r));
    out.insert(QStringLiteral("linkCreationOffered"), pbc::link_creation_offered(r));
    out.insert(QStringLiteral("passwordRequired"), pbc::password_required(r, pbc::LinkPermission::ReadOnly));
    out.insert(QStringLiteral("passwordRequiredUpload"), pbc::password_required(r, pbc::LinkPermission::ReadUpload));
    out.insert(QStringLiteral("expiryRequired"), pbc::expiry_required(r));
    out.insert(QStringLiteral("expiryMaxDays"), pbc::expiry_max_days(r).value_or(0));
    out.insert(QStringLiteral("uploadOffered"), pbc::upload_offered(r, target_.is_folder));
    QVariantMap known;
    known.insert(QStringLiteral("public"), r.public_enabled.has_value());
    known.insert(QStringLiteral("password"), r.password_enforced.has_value() || r.password_enforced_read_only.has_value() || r.password_enforced_read_write.has_value());
    known.insert(QStringLiteral("expiry"), r.expiry_enforced.has_value());
    known.insert(QStringLiteral("upload"), r.upload_allowed.has_value());
    out.insert(QStringLiteral("known"), known);
    return out;
}

QVariantMap ShareBridge::row_map(const pbc::ShareRow& row) const {
    QVariantMap m;
    const bool link = row.kind == pbc::ShareKind::PublicLink;
    m.insert(QStringLiteral("id"), static_cast<qlonglong>(row.id));
    const char* kind = link ? "link" : row.kind == pbc::ShareKind::User ? "user" : row.kind == pbc::ShareKind::Group ? "group" : "other";
    m.insert(QStringLiteral("kind"), QString::fromLatin1(kind));
    QString title;
    if (link) {
        title = row.label.empty() ? bridge_tr("Public link") : bridge_tr("Public link: %1").arg(display_safe(row.label, 64));
    } else if (row.kind == pbc::ShareKind::User) {
        title = bridge_tr("User: %1").arg(display_safe(row.with_name, 64));
    } else if (row.kind == pbc::ShareKind::Group) {
        title = bridge_tr("Group: %1").arg(display_safe(row.with_name, 64));
    } else {
        title = bridge_tr("Other kind of share");
    }
    m.insert(QStringLiteral("title"), title);
    QStringList parts;
    if (row.kind != pbc::ShareKind::Other && !row.limited) parts << permission_text(row);
    if (link && row.has_password) parts << bridge_tr("Password");
    if (row.kind != pbc::ShareKind::Other && (row.expires || row.expires_unreadable)) parts << expiry_text(row);
    if (!row.owned_by_me && row.kind != pbc::ShareKind::Other) parts << bridge_tr("Not yours");
    if (row.inherited) parts << bridge_tr("From a parent folder");
    if (row.limited) parts << bridge_tr("Details not available");
    m.insert(QStringLiteral("detailLine"), parts.join(QStringLiteral(" · ")));
    m.insert(QStringLiteral("ownedByMe"), row.owned_by_me);
    m.insert(QStringLiteral("inherited"), row.inherited);
    m.insert(QStringLiteral("limited"), row.limited);
    m.insert(QStringLiteral("canOpen"), link);
    return m;
}

void ShareBridge::rebuild_rows() {
    const int total = static_cast<int>(all_rows_.size());
    page_count_ = std::max(1, (total + page_size_ - 1) / page_size_);
    page_ = std::max(0, std::min(page_, page_count_ - 1));
    rows_.clear();
    const int first = page_ * page_size_;
    for (int i = first; i < std::min(total, first + page_size_); ++i) rows_.append(row_map(all_rows_[static_cast<size_t>(i)]));
}

void ShareBridge::nextPage() {
    if (page_ + 1 >= page_count_) return;
    ++page_;
    rebuild_rows();
    emit changed();
}

void ShareBridge::previousPage() {
    if (page_ == 0) return;
    --page_;
    rebuild_rows();
    emit changed();
}

QVariantMap ShareBridge::open(const QString& remotePath) {
    QVariantMap r;
    if (busy_) {
        r.insert(QStringLiteral("errorKey"), QStringLiteral("busy"));
        return r;
    }
    const std::string path = pbc::normalize_remote_path(remotePath.toStdString());
    const auto entry = c_.store ? c_.store->entry(path) : std::nullopt;
    if (!entry) {
        r.insert(QStringLiteral("errorKey"), QStringLiteral("share_item_gone"));
        return r;
    }
    clearSecrets();
    target_.path = path;
    target_.is_folder = entry->kind == pbc::Kind::Folder;
    item_name_ = display_safe(entry->name, 120);
    opened_key_ = account_key();
    shareable_ = true;
    unavailable_reason_.clear();
    if (entry->availability == pbc::Availability::Encrypted) {
        shareable_ = false;
        unavailable_reason_ = bridge_tr("End-to-end encrypted folders cannot be shared.");
    } else if (entry->availability != pbc::Availability::Ok) {
        shareable_ = false;
        unavailable_reason_ = bridge_tr("You may not use this entry, so you cannot share it.");
    } else if (!entry->permissions.empty() && entry->permissions.find('R') == std::string::npos) {
        shareable_ = false;
        unavailable_reason_ = bridge_tr("You may not share this item.");
    }
    if (!shareable_) {
        state_ = QStringLiteral("failed");
        message_ = message_map([] { pbc::ShareMessage m; m.key = "share_item_not_shareable"; return m; }());
        emit changed();
        r.insert(QStringLiteral("errorKey"), QStringLiteral("share_item_not_shareable"));
        return r;
    }
    return start_load();
}

QVariantMap ShareBridge::refresh() {
    QVariantMap r;
    if (busy_) {
        r.insert(QStringLiteral("errorKey"), QStringLiteral("busy"));
        return r;
    }
    if (state_ == QLatin1String("closed") || target_.path.empty()) {
        r.insert(QStringLiteral("errorKey"), QStringLiteral("share_item_gone"));
        return r;
    }
    if (!shareable_) {
        r.insert(QStringLiteral("errorKey"), QStringLiteral("share_item_not_shareable"));
        return r;
    }
    return start_load();
}

void ShareBridge::offline_load_failed() {
    stale_ = !all_rows_.empty() || stale_;
    state_ = all_rows_.empty() ? QStringLiteral("failed") : QStringLiteral("listed");
    message_ = message_map([] { pbc::ShareMessage m; m.key = "no_network"; return m; }());
    emit changed();
}

void ShareBridge::with_network(quint64 generation, std::function<void()> go, std::function<void()> failed) {
    if (!offline() || !c_.ensure_network) {
        go();
        return;
    }
    // A tap needs the network: the system is asked to bring Wi-Fi up, as in the files client. Nothing is queued: if it does not come up, the tap fails.
    waiting_network_ = true;
    QPointer<ShareBridge> self(this);
    c_.ensure_network([self, generation, go, failed](bool online) {
        if (!self || generation != self->generation_) return;   // the screen was left meanwhile: nothing is sent
        self->waiting_network_ = false;
        if (online) go();
        else failed();
    });
}

QVariantMap ShareBridge::start_load(bool keep_message) {
    QVariantMap r;
    if (offline() && !c_.ensure_network) {
        offline_load_failed();
        r.insert(QStringLiteral("errorKey"), QStringLiteral("no_network"));
        return r;
    }
    busy_ = true;
    state_ = QStringLiteral("loading");
    if (!keep_message) message_.clear();
    emit changed();
    const quint64 generation = generation_;
    const pbc::ShareTarget target = target_;
    const Config c = c_;
    with_network(
        generation,
        [this, c, target, generation] {
            c_.worker->post([this, c, target, generation] {
                Loaded loaded;
                load_shares(c, target, &loaded.error, &loaded.message, &loaded.have_rules, &loaded.rules, &loaded.listing, &loaded.server_time);
                QMetaObject::invokeMethod(this, [this, generation, loaded] { apply_loaded(generation, loaded); }, Qt::QueuedConnection);
            });
        },
        [this] {
            busy_ = false;
            offline_load_failed();
        });
    r.insert(QStringLiteral("ok"), true);
    return r;
}

void ShareBridge::apply_loaded(quint64 generation, const Loaded& loaded) {
    if (generation != generation_) return;   // the screen was left meanwhile: nothing from this answer is kept
    busy_ = false;
    if (loaded.server_time) server_epoch_ = loaded.server_time;
    if (loaded.have_rules) {
        core_rules_ = loaded.rules;
        rules_ = rules_map(core_rules_);
    }
    if (!loaded.error.ok()) {
        message_ = message_map(loaded.message.key.empty() ? [&] { pbc::ShareMessage m; m.key = loaded.error.key; return m; }() : loaded.message);
        state_ = QStringLiteral("failed");
        emit changed();
        return;
    }
    all_rows_ = loaded.listing.own.rows;
    all_rows_.insert(all_rows_.end(), loaded.listing.inherited.rows.begin(), loaded.listing.inherited.rows.end());
    truncated_ = loaded.listing.own.truncated;
    inherited_note_ = loaded.listing.inherited_failed ? QStringLiteral("inherited_failed") : QString();
    stale_ = false;
    has_link_ = std::any_of(loaded.listing.own.rows.begin(), loaded.listing.own.rows.end(), [](const pbc::ShareRow& r) { return r.kind == pbc::ShareKind::PublicLink; });
    page_ = 0;
    rebuild_rows();
    state_ = QStringLiteral("listed");
    emit changed();
}

// ---- create (US1) ----

struct ShareBridge::Created {
    pbc::Error error;
    pbc::ShareMessage message;
    bool unknown = false;
    pbc::ShareDetail detail;
    std::optional<int64_t> server_time;
    pbc::ShareRow current;   // after share_changed_meanwhile: the share as it is now
};

std::shared_ptr<ShareBridge::Created> ShareBridge::offline_result() {
    auto result = std::make_shared<Created>();
    result->error = pbc::error_from_key("no_network");
    result->message.key = "no_network";
    return result;
}

namespace {

// Runs on the worker: one create request. The request (with the password) is moved in; the core wipes it on every path.
void run_create(const ShareBridge::Config& c, const pbc::ShareTarget& target, const pbc::ShareRules& rules, pbc::LinkRequest&& request, pbc::Error* error,
                pbc::ShareMessage* message, bool* unknown, pbc::ShareDetail* detail, std::optional<int64_t>* server_time) {
    const auto account = c.store->account();
    pbc::AuthGuard guard(*c.store);
    auto creds = pbc::load_credentials(c.store->directory());
    if (!account || guard.blocked() || !creds.ok() || creds.value.password.empty()) {
        request.password.wipe();
        *error = pbc::error_from_key("auth_failed");
        message->key = "auth_failed";
        return;
    }
    pbc::DavSession session(*c.http, account->base_url, account->login_name, creds.value.password);
    pbc::secure_wipe(creds.value.password);
    pbc::ShareApi api(session, *c.clock, account->user_id);
    auto r = api.create(target, rules, std::move(request));
    guard.record(r.error);
    *error = r.error;
    *message = api.message();
    *unknown = api.outcome_unknown();
    *detail = std::move(r.value);
    *server_time = api.server_time();
}

}  // namespace

qint64 ShareBridge::today_days() const {
    const int64_t t = server_epoch_ ? *server_epoch_ : (c_.clock ? c_.clock->now() : 0);
    return static_cast<qint64>(t / 86400);
}

QString ShareBridge::summary_text(const pbc::LinkRequest& request) const {
    QStringList parts;
    if (!target_.is_folder) parts << bridge_tr("Anyone who has this link can read this file.");
    else if (request.permission == pbc::LinkPermission::ReadUpload) parts << bridge_tr("Anyone who has this link can read this folder and add files to it.");
    else parts << bridge_tr("Anyone who has this link can read this folder.");
    parts << (request.password.empty() ? bridge_tr("No password is set.") : bridge_tr("A password is set."));
    if (request.expiry) {
        const QString date = c_.format_date ? c_.format_date(*request.expiry) : QString::fromStdString(*request.expiry);
        parts << bridge_tr("The link stops working after %1.").arg(date);
    } else {
        parts << bridge_tr("The link does not expire.");
    }
    return parts.join(QLatin1Char(' '));
}

QVariantMap ShareBridge::create(const QVariantMap& options) {
    QVariantMap r;
    const auto refuse = [&](const char* key) {
        r.insert(QStringLiteral("errorKey"), QString::fromLatin1(key));
        return r;
    };
    if (busy_) return refuse("busy");
    if (state_ != QLatin1String("listed") || !shareable_ || target_.path.empty()) return refuse("share_item_gone");
    if (offline() && !c_.ensure_network) {   // sharing needs a live connection (FR-006); with the hook the tap brings Wi-Fi up first
        message_ = message_map([] { pbc::ShareMessage m; m.key = "no_network"; return m; }());
        emit changed();
        return refuse("no_network");
    }
    QString password = options.value(QStringLiteral("password")).toString();   // shares its buffer with the caller's string
    const bool confirmed = options.value(QStringLiteral("confirmed")).toBool();
    pbc::LinkRequest request;
    request.permission = options.value(QStringLiteral("permission")).toString() == QLatin1String("upload") ? pbc::LinkPermission::ReadUpload : pbc::LinkPermission::ReadOnly;
    if (!password.isEmpty()) request.password = pbc::SecretText(password.toStdString());   // a copy: the QString is untouched until the request is really sent
    const QString expiry = options.value(QStringLiteral("expiry")).toString().trimmed();
    if (!expiry.isEmpty()) request.expiry = expiry.toStdString();
    const pbc::ShareMessage invalid = pbc::validate_create(core_rules_, target_, request, today_days());
    if (!invalid.empty()) {
        message_ = message_map(invalid);
        emit changed();
        r.insert(QStringLiteral("errorKey"), QString::fromStdString(invalid.key));
        r.insert(QStringLiteral("field"), message_.value(QStringLiteral("field")));
        return r;
    }
    if (!confirmed) {
        r.insert(QStringLiteral("needsConfirm"), true);
        r.insert(QStringLiteral("summary"), summary_text(request));
        return r;
    }
    wipe(&password);   // the typed password now lives only in `request`: the string the page and the caller hold is overwritten in place
    busy_ = true;
    state_ = QStringLiteral("working");
    message_.clear();
    emit changed();
    const quint64 generation = generation_;
    const pbc::ShareTarget target = target_;
    const pbc::ShareRules rules = core_rules_;
    const Config c = c_;
    auto pending = std::make_shared<pbc::LinkRequest>(std::move(request));   // wiped by its destructor when the request is never sent
    with_network(
        generation,
        [this, c, target, rules, pending, generation] {
            c_.worker->post([this, c, target, rules, pending, generation] {
                auto created = std::make_shared<Created>();
                run_create(c, target, rules, std::move(*pending), &created->error, &created->message, &created->unknown, &created->detail, &created->server_time);
                QMetaObject::invokeMethod(this, [this, generation, created] { apply_created(generation, created); }, Qt::QueuedConnection);
            });
        },
        [this, generation, pending] {
            pending->password.wipe();
            apply_created(generation, offline_result());
        });
    r.insert(QStringLiteral("ok"), true);
    return r;
}

void ShareBridge::apply_created(quint64 generation, const std::shared_ptr<Created>& created) {
    if (generation != generation_) {   // the screen was left meanwhile: the new link is not shown anywhere
        pbc::secure_wipe(created->detail.link);
        return;
    }
    busy_ = false;
    if (created->server_time) server_epoch_ = created->server_time;
    if (!created->error.ok()) {
        pbc::ShareMessage m = created->message;
        if (m.key.empty()) m.key = created->error.key;
        message_ = message_map(m);
        if (created->unknown) stale_ = true;   // the request may have been processed: the list is not claimed to be current
        state_ = QStringLiteral("listed");
        emit changed();
        emit createFinished(false, QString::fromStdString(m.key));
        return;
    }
    setDetail(created->detail);
    pbc::secure_wipe(created->detail.link);
    state_ = QStringLiteral("listed");
    emit changed();
    emit createFinished(true, QString());
    start_load();   // the list is read again: it shows the new link among the others
}

QString ShareBridge::dateText(const QString& isoDay) const {
    const std::string iso = isoDay.toStdString();
    if (!pbc::parse_iso_day(iso)) return isoDay;
    return c_.format_date ? c_.format_date(iso) : isoDay;
}

void ShareBridge::clearMessage() {
    if (message_.isEmpty()) return;
    message_.clear();
    emit changed();
}

// ---- show a link (US3) ----

namespace {

// Runs on the worker: the read of one share (the only answer that may hold a link).
void run_detail(const ShareBridge::Config& c, int64_t id, bool is_folder, pbc::Error* error, pbc::ShareMessage* message, pbc::ShareDetail* detail,
                std::optional<int64_t>* server_time) {
    const auto account = c.store->account();
    pbc::AuthGuard guard(*c.store);
    auto creds = pbc::load_credentials(c.store->directory());
    if (!account || guard.blocked() || !creds.ok() || creds.value.password.empty()) {
        *error = pbc::error_from_key("auth_failed");
        message->key = "auth_failed";
        return;
    }
    pbc::DavSession session(*c.http, account->base_url, account->login_name, creds.value.password);
    pbc::secure_wipe(creds.value.password);
    pbc::ShareApi api(session, *c.clock, account->user_id);
    auto r = api.detail(id, is_folder);
    guard.record(r.error);
    *error = r.error;
    *message = api.message();
    *detail = std::move(r.value);
    *server_time = api.server_time();
}

}  // namespace

QVariantMap ShareBridge::showLink(qlonglong id) {
    QVariantMap r;
    const auto refuse = [&](const char* key) {
        r.insert(QStringLiteral("errorKey"), QString::fromLatin1(key));
        return r;
    };
    if (busy_) return refuse("busy");
    if (state_ == QLatin1String("closed") || target_.path.empty()) return refuse("share_item_gone");
    if (offline() && !c_.ensure_network) {
        message_ = message_map([] { pbc::ShareMessage m; m.key = "no_network"; return m; }());
        emit changed();
        return refuse("no_network");
    }
    busy_ = true;
    message_.clear();
    emit changed();
    const quint64 generation = generation_;
    const bool folder = target_.is_folder;
    const Config c = c_;
    with_network(
        generation,
        [this, c, id, folder, generation] {
            c_.worker->post([this, c, id, folder, generation] {
                auto result = std::make_shared<Created>();
                run_detail(c, id, folder, &result->error, &result->message, &result->detail, &result->server_time);
                QMetaObject::invokeMethod(this, [this, generation, id, result] { apply_detail(generation, id, result); }, Qt::QueuedConnection);
            });
        },
        [this, generation, id] { apply_detail(generation, id, offline_result()); });
    r.insert(QStringLiteral("ok"), true);
    return r;
}

void ShareBridge::apply_detail(quint64 generation, qlonglong id, const std::shared_ptr<Created>& result) {
    if (generation != generation_) {   // the screen was left meanwhile: the link is shown nowhere
        pbc::secure_wipe(result->detail.link);
        return;
    }
    busy_ = false;
    if (result->server_time) server_epoch_ = result->server_time;
    if (!result->error.ok()) {
        pbc::ShareMessage m = result->message;
        if (m.key.empty()) m.key = result->error.key;
        message_ = message_map(m);
        emit changed();
        if (m.key == "share_gone") start_load(true);   // the link is not there any more: the list is read again and the message stays
        return;
    }
    setDetail(result->detail);
    pbc::secure_wipe(result->detail.link);
    emit linkOpened(id);
}

// ---- change (US4) ----

namespace {

void run_change(const ShareBridge::Config& c, const pbc::ShareTarget& target, const pbc::ShareRules& rules, int64_t id, const pbc::Snapshot& shown,
                pbc::LinkChange&& change, pbc::Error* error, pbc::ShareMessage* message, bool* unknown, pbc::ShareDetail* detail, pbc::ShareRow* current,
                std::optional<int64_t>* server_time) {
    const auto account = c.store->account();
    pbc::AuthGuard guard(*c.store);
    auto creds = pbc::load_credentials(c.store->directory());
    if (!account || guard.blocked() || !creds.ok() || creds.value.password.empty()) {
        change.password.wipe();
        *error = pbc::error_from_key("auth_failed");
        message->key = "auth_failed";
        return;
    }
    pbc::DavSession session(*c.http, account->base_url, account->login_name, creds.value.password);
    pbc::secure_wipe(creds.value.password);
    pbc::ShareApi api(session, *c.clock, account->user_id);
    auto r = api.change(target, rules, id, shown, std::move(change));
    guard.record(r.error);
    *error = r.error;
    *message = api.message();
    *unknown = api.outcome_unknown();
    *current = api.current();
    *detail = std::move(r.value);
    *server_time = api.server_time();
}

}  // namespace

QString ShareBridge::effect_text(const pbc::LinkChange& change) const {
    switch (change.action) {
        case pbc::LinkChange::Action::SetPassword:
            return detail_.row.has_password ? bridge_tr("The old password stops working.") : bridge_tr("Visitors will need this password.");
        case pbc::LinkChange::Action::ClearPassword: return bridge_tr("The link will work without a password.");
        case pbc::LinkChange::Action::SetExpiry: return bridge_tr("The link will not work after %1.").arg(dateText(QString::fromStdString(change.expiry)));
        case pbc::LinkChange::Action::ClearExpiry: return bridge_tr("The link will not expire.");
        case pbc::LinkChange::Action::SetPermission:
            return change.permission == pbc::LinkPermission::ReadUpload ? bridge_tr("Anyone with the link will be able to add files.")
                                                                       : bridge_tr("Visitors will only be able to read.");
    }
    return QString();
}

QVariantMap ShareBridge::change(qlonglong id, const QVariantMap& options) {
    QVariantMap r;
    const auto refuse = [&](const char* key) {
        r.insert(QStringLiteral("errorKey"), QString::fromLatin1(key));
        return r;
    };
    if (busy_) return refuse("busy");
    if (detail_.row.id == 0 || detail_.row.id != id) return refuse("share_item_gone");
    if (!detail_.row.owned_by_me || detail_.row.kind != pbc::ShareKind::PublicLink) return refuse("share_not_changeable");
    if (offline() && !c_.ensure_network) {
        message_ = message_map([] { pbc::ShareMessage m; m.key = "no_network"; return m; }());
        emit changed();
        return refuse("no_network");
    }
    const QString action = options.value(QStringLiteral("action")).toString();
    QString password = options.value(QStringLiteral("password")).toString();   // shares its buffer with the caller's string
    const bool confirmed = options.value(QStringLiteral("confirmed")).toBool();
    pbc::LinkChange change;
    if (action == QLatin1String("set_password")) {
        change.action = pbc::LinkChange::Action::SetPassword;
        change.password = pbc::SecretText(password.toStdString());   // a copy: the QString is untouched until the request is really sent
    } else if (action == QLatin1String("clear_password")) {
        change.action = pbc::LinkChange::Action::ClearPassword;
    } else if (action == QLatin1String("set_expiry")) {
        change.action = pbc::LinkChange::Action::SetExpiry;
        change.expiry = options.value(QStringLiteral("expiry")).toString().trimmed().toStdString();
    } else if (action == QLatin1String("clear_expiry")) {
        change.action = pbc::LinkChange::Action::ClearExpiry;
    } else if (action == QLatin1String("set_permission")) {
        change.action = pbc::LinkChange::Action::SetPermission;
        change.permission = options.value(QStringLiteral("permission")).toString() == QLatin1String("upload") ? pbc::LinkPermission::ReadUpload : pbc::LinkPermission::ReadOnly;
    } else {
        return refuse("permission_invalid");
    }
    const pbc::ShareMessage invalid = pbc::validate_change(core_rules_, target_, detail_.row, change, today_days());
    if (!invalid.empty()) {
        message_ = message_map(invalid);
        emit changed();
        r.insert(QStringLiteral("errorKey"), QString::fromStdString(invalid.key));
        r.insert(QStringLiteral("field"), message_.value(QStringLiteral("field")));
        return r;
    }
    if (!confirmed) {
        r.insert(QStringLiteral("needsConfirm"), true);
        r.insert(QStringLiteral("effectText"), effect_text(change));
        return r;
    }
    wipe(&password);   // the typed password now lives only in `change`: the string the page and the caller hold is overwritten in place
    busy_ = true;
    state_ = QStringLiteral("working");
    message_.clear();
    emit changed();
    const quint64 generation = generation_;
    const pbc::ShareTarget target = target_;
    const pbc::ShareRules rules = core_rules_;
    const pbc::Snapshot shown = pbc::snapshot_of(detail_.row);
    const Config c = c_;
    auto pending = std::make_shared<pbc::LinkChange>(std::move(change));   // wiped by its destructor when the request is never sent
    with_network(
        generation,
        [this, c, target, rules, id, shown, pending, generation] {
            c_.worker->post([this, c, target, rules, id, shown, pending, generation] {
                auto result = std::make_shared<Created>();
                run_change(c, target, rules, id, shown, std::move(*pending), &result->error, &result->message, &result->unknown, &result->detail,
                           &result->current, &result->server_time);
                QMetaObject::invokeMethod(this, [this, generation, result] { apply_changed(generation, result); }, Qt::QueuedConnection);
            });
        },
        [this, generation, pending] {
            pending->password.wipe();
            apply_changed(generation, offline_result());
        });
    r.insert(QStringLiteral("ok"), true);
    return r;
}

void ShareBridge::apply_changed(quint64 generation, const std::shared_ptr<Created>& result) {
    if (generation != generation_) {
        pbc::secure_wipe(result->detail.link);
        return;
    }
    busy_ = false;
    if (result->server_time) server_epoch_ = result->server_time;
    state_ = QStringLiteral("listed");
    if (!result->error.ok()) {
        pbc::ShareMessage m = result->message;
        if (m.key.empty()) m.key = result->error.key;
        message_ = message_map(m);
        if (m.key == "share_changed_meanwhile" && result->current.id == detail_.row.id) {   // show the current state; the link text stays as it was
            detail_.row = result->current;
            rebuild_detail_map();
        }
        if (result->unknown) stale_ = true;
        emit changed();
        emit changeFinished(false, QString::fromStdString(m.key));
        return;
    }
    setDetail(result->detail);
    pbc::secure_wipe(result->detail.link);
    emit changed();
    emit changeFinished(true, QString());
    start_load();   // the list is read again: it shows the new state of the link
}

// ---- remove (US5) ----

namespace {

void run_remove(const ShareBridge::Config& c, const pbc::ShareTarget& target, const pbc::ShareRules& rules, int64_t id, pbc::Error* error,
                pbc::ShareMessage* message, bool* unknown, std::optional<int64_t>* server_time) {
    const auto account = c.store->account();
    pbc::AuthGuard guard(*c.store);
    auto creds = pbc::load_credentials(c.store->directory());
    if (!account || guard.blocked() || !creds.ok() || creds.value.password.empty()) {
        *error = pbc::error_from_key("auth_failed");
        message->key = "auth_failed";
        return;
    }
    pbc::DavSession session(*c.http, account->base_url, account->login_name, creds.value.password);
    pbc::secure_wipe(creds.value.password);
    pbc::ShareApi api(session, *c.clock, account->user_id);
    const pbc::Error e = api.remove_link(id, target, rules);
    guard.record(e);
    *error = e;
    *message = api.message();
    *unknown = api.outcome_unknown();
    *server_time = api.server_time();
}

}  // namespace

QVariantMap ShareBridge::remove(qlonglong id, bool confirmed) {
    QVariantMap r;
    const auto refuse = [&](const char* key) {
        r.insert(QStringLiteral("errorKey"), QString::fromLatin1(key));
        return r;
    };
    if (busy_) return refuse("busy");
    if (detail_.row.id == 0 || detail_.row.id != id) return refuse("share_item_gone");
    if (!detail_.row.owned_by_me || detail_.row.kind != pbc::ShareKind::PublicLink) return refuse("share_not_changeable");
    if (offline() && !c_.ensure_network) {
        message_ = message_map([] { pbc::ShareMessage m; m.key = "no_network"; return m; }());
        emit changed();
        return refuse("no_network");
    }
    if (!confirmed) {
        r.insert(QStringLiteral("needsConfirm"), true);
        r.insert(QStringLiteral("effectText"),
                 (target_.is_folder ? bridge_tr("The link to \"%1\" stops working at once for everyone who has it. The folder itself is not deleted.")
                                    : bridge_tr("The link to \"%1\" stops working at once for everyone who has it. The file itself is not deleted."))
                     .arg(item_name_));
        return r;
    }
    busy_ = true;
    state_ = QStringLiteral("working");
    message_.clear();
    emit changed();
    const quint64 generation = generation_;
    const pbc::ShareTarget target = target_;
    const pbc::ShareRules rules = core_rules_;
    const Config c = c_;
    with_network(
        generation,
        [this, c, target, rules, id, generation] {
            c_.worker->post([this, c, target, rules, id, generation] {
                auto result = std::make_shared<Created>();
                run_remove(c, target, rules, id, &result->error, &result->message, &result->unknown, &result->server_time);
                QMetaObject::invokeMethod(this, [this, generation, result] { apply_removed(generation, result); }, Qt::QueuedConnection);
            });
        },
        [this, generation] { apply_removed(generation, offline_result()); });
    r.insert(QStringLiteral("ok"), true);
    return r;
}

void ShareBridge::apply_removed(quint64 generation, const std::shared_ptr<Created>& result) {
    if (generation != generation_) return;
    busy_ = false;
    if (result->server_time) server_epoch_ = result->server_time;
    state_ = QStringLiteral("listed");
    const bool gone = !result->error.ok() && result->error.key == "share_gone";
    if (result->error.ok() || gone) {   // removed now, or already removed: the link is not shown any more and the list is read again
        drop_link();
        message_.clear();
        if (gone) message_ = message_map([] { pbc::ShareMessage m; m.key = "share_gone"; return m; }());
        emit changed();
        emit removeFinished(true, gone ? QStringLiteral("share_gone") : QString());
        start_load(gone);
        return;
    }
    pbc::ShareMessage m = result->message;
    if (m.key.empty()) m.key = result->error.key;
    message_ = message_map(m);
    if (result->unknown) {   // the request may have been processed: the link is dropped from the screen, the list is not claimed to be current
        stale_ = true;
        drop_link();
    }
    emit changed();
    emit removeFinished(false, QString::fromStdString(m.key));
}
