#include "pbc/folder_service.h"

#include <memory>

#include "pbc/auth_guard.h"
#include "pbc/credentials_file.h"
#include "pbc/log.h"
#include "pbc/nc_listing.h"
#include "pbc/remote_path.h"
#include "pbc/secure_wipe.h"

namespace pbc {

FolderResult FolderService::load(const std::string& raw_path, Fetch mode) {
    FolderResult out;
    const std::string path = normalize_remote_path(raw_path);
    const bool has = store_->folder(path).has_value();
    out.cached = has;
    const bool need = mode == Fetch::Always || (mode == Fetch::IfMissing && !has);
    if (!need) return out;

    const auto account = store_->account();
    AuthGuard guard(*store_);
    if (!account || guard.blocked()) {
        out.error = make_error(ErrorClass::Auth, "auth_failed", "sign in again");
        return out;
    }
    auto creds = load_credentials(store_->directory());
    if (!creds.ok() || creds.value.password.empty()) {
        out.error = make_error(ErrorClass::Auth, "auth_failed", "there is no app password");
        return out;
    }
    DavSession session(*http_, account->base_url, account->login_name, creds.value.password);
    secure_wipe(creds.value.password);
    auto fetched = fetch_listing(session, account->user_id, path);
    guard.record(fetched.error);
    if (!fetched.ok()) {
        if (fetched.error.key == "not_found" && has) store_->mark_folder_gone(path);   // gone on the server; the rows stay readable
        out.error = fetched.error;
        log::event("listing.refresh", {log::text("result", fetched.error.key)});
        return out;
    }
    const Error saved = store_->replace_listing(path, fetched.value, clock_->now());
    if (!saved.ok()) {
        out.error = saved;
        log::event("listing.refresh", {log::text("result", saved.key)});
        return out;
    }
    out.fetched = true;
    out.cached = true;
    out.partial = fetched.value.partial;
    out.skipped = fetched.value.skipped_foreign + fetched.value.skipped_hostile;
    log::event("listing.refresh", {log::text("result", "ok"), log::count("entries", static_cast<long>(fetched.value.entries.size())), log::count("skipped", out.skipped)});
    return out;
}

namespace {

// The signed-in session for one request, or an error: no account, refused twice, no app password.
struct Opened {
    Error error;
    std::optional<AccountRow> account;
    std::unique_ptr<DavSession> session;
};

Opened open_session(Store& store, HttpTransport& http, AuthGuard& guard) {
    Opened o;
    o.account = store.account();
    if (!o.account || guard.blocked()) {
        o.error = make_error(ErrorClass::Auth, "auth_failed", "sign in again");
        return o;
    }
    auto creds = load_credentials(store.directory());
    if (!creds.ok() || creds.value.password.empty()) {
        o.error = make_error(ErrorClass::Auth, "auth_failed", "there is no app password");
        return o;
    }
    o.session = std::make_unique<DavSession>(http, o.account->base_url, o.account->login_name, creds.value.password);
    secure_wipe(creds.value.password);
    return o;
}

}  // namespace

FlatResult FolderService::refresh_favorites() {
    FlatResult out;
    AuthGuard guard(*store_);
    Opened o = open_session(*store_, *http_, guard);
    if (!o.error.ok()) {
        out.error = o.error;
        return out;
    }
    auto fetched = fetch_favorites(*o.session, o.account->user_id);
    guard.record(fetched.error);
    if (!fetched.ok()) {
        out.error = fetched.error;
        log::event("favorites.refresh", {log::text("result", fetched.error.key)});
        return out;
    }
    out.entries = std::move(fetched.value);
    if (out.entries.size() > static_cast<size_t>(kMaxFavorites)) {
        out.entries.resize(static_cast<size_t>(kMaxFavorites));
        out.more = true;
    }
    const Error saved = store_->replace_favorites(out.entries, clock_->now());
    if (!saved.ok()) {
        out.error = saved;
        out.entries.clear();
        log::event("favorites.refresh", {log::text("result", saved.key)});
        return out;
    }
    log::event("favorites.refresh", {log::text("result", "ok"), log::count("entries", static_cast<long>(out.entries.size()))});
    return out;
}

FlatResult FolderService::search_server(const std::string& term) {
    FlatResult out;
    if (clean_search_term(term).empty()) return out;   // nothing usable: no request, no sign-in check
    AuthGuard guard(*store_);
    Opened o = open_session(*store_, *http_, guard);
    if (!o.error.ok()) {
        out.error = o.error;
        return out;
    }
    auto found = pbc::search_server(*o.session, o.account->user_id, term);
    guard.record(found.error);
    if (!found.ok()) {
        out.error = found.error;
        log::event("search.server", {log::text("result", found.error.key)});   // never the term
        return out;
    }
    out.entries = std::move(found.value);
    if (out.entries.size() > static_cast<size_t>(kMaxSearchResults)) {
        out.entries.resize(static_cast<size_t>(kMaxSearchResults));
        out.more = true;
    }
    store_->set_search_results(out.entries);
    log::event("search.server", {log::text("result", "ok"), log::count("results", static_cast<long>(out.entries.size()))});
    return out;
}

}  // namespace pbc
