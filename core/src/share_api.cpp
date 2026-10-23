#include "pbc/share_api.h"

#include <utility>

#include "pbc/error_keys.h"
#include "pbc/json_min.h"
#include "pbc/log.h"
#include "pbc/remote_path.h"
#include "pbc/secure_wipe.h"
#include "pbc/version.h"

namespace pbc {

namespace {

bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

void encode_into(std::string* out, const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (unreserved(c)) {
            out->push_back(static_cast<char>(c));
        } else {
            out->push_back('%');
            out->push_back(hex[c >> 4]);
            out->push_back(hex[c & 15]);
        }
    }
}


// Headers of every sharing request (contracts/share-requests.md "Conventions").
Headers share_headers(bool form) {
    Headers h = {{"OCS-APIRequest", "true"}, {"Accept", "application/json"}, {"User-Agent", std::string("PBCloud/") + version()}};
    if (form) h.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    return h;
}

// A value of the query string: everything except A-Z a-z 0-9 - . _ ~ is percent-encoded (so "/" is %2F).
std::string query_escape(const std::string& s) {
    std::string out;
    encode_into(&out, s);
    return out;
}

std::string host_of(const std::string& origin) {
    std::string scheme, host;
    int port = 0;
    return parse_url_origin(origin, &scheme, &host, &port) ? host : std::string();
}

using share_detail::Answer;

Answer classify(const Response& r) {
    Answer a;
    a.status = r.status;
    if (r.status == 0) { a.kind = Answer::Kind::Transport; return a; }
    if (r.body_too_large) { a.kind = Answer::Kind::TooBig; return a; }
    if (r.status < 200 || r.status >= 300) { a.kind = Answer::Kind::Refused; return a; }
    const auto st = parse_ocs_status(r.body);
    if (!st.ok()) { a.kind = Answer::Kind::Bad; return a; }
    a.ocs = st.value.statuscode;
    if (!st.value.ok) {
        a.kind = Answer::Kind::Refused;
        a.status = st.value.statuscode >= 400 ? st.value.statuscode : 400;
    }
    return a;
}

// Wipes a string that held a secret on every way out of a function, also when an exception leaves it.
struct WipeOnExit {
    std::string* s;
    ~WipeOnExit() { secure_wipe(*s); }
};

void log_failure(ShareOp op, long status) {
    log::event("share.fail", {log::status(status), log::count("op", static_cast<long>(op))});
}

}  // namespace

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& f : fields) {
        if (!out.empty()) out.push_back('&');
        encode_into(&out, f.first);
        out.push_back('=');
        encode_into(&out, f.second);
    }
    return out;
}

}  // namespace pbc

namespace pbc {

ShareApi::ShareApi(DavSession& session, const Clock& clock, std::string user_id)
    : session_(session), clock_(clock), user_id_(std::move(user_id)), host_(host_of(session.origin())) {}

namespace {

int64_t days_of(int64_t epoch) { return epoch >= 0 ? epoch / 86400 : -((-epoch + 86399) / 86400); }

}  // namespace

int64_t ShareApi::today() const {
    const auto server = session_.last_server_time();
    return days_of(server ? *server : clock_.now());
}

std::string ShareApi::api_url(const std::string& sub) const { return session_.base_url() + kShareApiPath + sub; }

std::string ShareApi::item_url(const ShareTarget& target) const {
    return session_.base_url() + "/remote.php/dav/files" + encode_path("/" + user_id_) + encode_path(target.path);
}

void ShareApi::reset() {
    message_ = ShareMessage();
    current_ = ShareRow();
    unknown_ = false;
}

Error ShareApi::fail(const std::string& key) {
    message_.key = key;
    return error_from_key(key);
}

// The one Depth-0 PROPFIND of research R-4 (the read request of feature 001): does the item still exist?
std::optional<bool> ShareApi::item_exists(const ShareTarget& target) {
    const std::string body = std::string(dav::kXmlHeader) + "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/></d:prop></d:propfind>";
    const Response r = session_.send("PROPFIND", item_url(target), body, dav::dav_headers("0"), 1 << 20, kShareTotalTimeoutS);
    if (r.status == 207) return true;
    if (r.status == 404) return false;
    return std::nullopt;
}

// Turns a refused or failed answer into an Error and fills message_ (and unknown_ for a write whose answer was lost).
Error ShareApi::refusal(const Response& r, const Answer& a, ShareOp op, const ShareTarget& target, const LinkRequest* create, const LinkChange* change,
                        const ShareRow* current) {
    const bool write = op == ShareOp::Create || op == ShareOp::Change || op == ShareOp::Remove;
    log_failure(op, a.status);
    if (a.kind == Answer::Kind::Transport) {
        if (write && r.delivery != Delivery::NotSent) {
            unknown_ = true;
            return fail("share_unknown_outcome");
        }
        return fail(session_.status_error(r).key);
    }
    if (a.kind == Answer::Kind::TooBig || a.kind == Answer::Kind::Bad) return fail("protocol_error");
    if (a.status == 401 || a.status == 429 || a.status == 503) return fail(session_.status_error(r).key);
    // 400/403/404 are explained by the rules the server reports now (R-4); other statuses cannot be, and are refused with their code
    ShareRules fresh;
    if (op != ShareOp::Detail && (a.status == 400 || a.status == 403 || a.status == 404)) {
        const Result<ShareRules> f = fetch_rules();
        if (f.ok()) fresh = f.value;
    }
    std::optional<bool> exists;
    if ((op == ShareOp::Create || op == ShareOp::List) && (a.status == 403 || a.status == 404)) exists = item_exists(target);
    RefusalContext ctx{fresh, target, op, create, change, current, a.status, a.ocs, exists, today()};
    message_ = diagnose_refusal(ctx);
    return error_from_key(message_.key);
}

Result<ShareRules> ShareApi::fetch_rules() {
    Result<ShareRules> out;
    const Response r = session_.send("GET", session_.base_url() + "/ocs/v1.php/cloud/capabilities?format=json", std::string(), share_headers(false), 1 << 20, kShareTotalTimeoutS);
    if (r.status < 200 || r.status >= 300 || r.body_too_large) {
        out.error = r.body_too_large ? error_from_key("protocol_error") : session_.status_error(r);
        return out;
    }
    const auto doc = parse_json(r.body);
    const JsonValue* v = doc.ok() ? &doc.value : nullptr;
    for (const char* key : {"ocs", "data", "capabilities"}) v = v ? v->get(key) : nullptr;
    if (!v) {
        out.error = error_from_key("protocol_error");
        return out;
    }
    out.value = share_rules_from_capabilities(*v);
    return out;
}

Result<ShareApi::Listing> ShareApi::list(const ShareTarget& target) {
    reset();
    Result<Listing> out;
    if (target.path.empty() || target.path[0] != '/' || target.path.size() > kMaxSharePathBytes) {
        out.error = fail("protocol_error");
        return out;
    }
    const std::string q = query_escape(target.path);
    const Response own = session_.send("GET", api_url("/shares?path=" + q + "&reshares=true&format=json"), std::string(), share_headers(false), kMaxShareResponseBytes,
                                        kShareTotalTimeoutS);
    const Answer a = classify(own);
    if (a.kind != Answer::Kind::Ok) {
        out.error = refusal(own, a, ShareOp::List, target, nullptr, nullptr, nullptr);
        return out;
    }
    auto rows = parse_share_list(own.body, user_id_, false, target.is_folder);
    if (!rows.ok()) {
        log_failure(ShareOp::List, own.status);
        out.error = fail(rows.error.key);
        return out;
    }
    out.value.own = std::move(rows.value);
    // the optional list of shares that reach the item from a parent folder; "not offered" is silent, any other failure is a note
    const Response parent = session_.send("GET", api_url("/shares/inherited?path=" + q + "&format=json"), std::string(), share_headers(false),
                                          kMaxShareResponseBytes, kShareTotalTimeoutS);
    const Answer b = classify(parent);
    if (b.kind == Answer::Kind::Ok) {
        auto inherited = parse_share_list(parent.body, user_id_, true, target.is_folder);
        if (inherited.ok()) out.value.inherited = std::move(inherited.value);
        else out.value.inherited_failed = true;
    } else if (!(b.kind == Answer::Kind::Refused && (b.status == 404 || b.status == 405 || b.status == 501))) {
        out.value.inherited_failed = true;
    }
    log::event("share.open", {log::status(own.status), log::count("rows", static_cast<long>(out.value.own.rows.size())),
                              log::count("skipped", out.value.own.dropped), log::count("inherited", static_cast<long>(out.value.inherited.rows.size()))});
    return out;
}

Result<ShareDetail> ShareApi::create(const ShareTarget& target, const ShareRules& rules, LinkRequest&& request) {
    reset();
    struct WipeRequest {   // the password does not outlive this call, whatever happens
        LinkRequest& r;
        ~WipeRequest() { r.password.wipe(); }
    } wipe_request{request};
    Result<ShareDetail> out;
    if (target.path.empty() || target.path[0] != '/' || target.path.size() > kMaxSharePathBytes) {
        out.error = fail("protocol_error");
        return out;
    }
    // what the screen holds is enough to refuse without a request (R-4 step 1)
    const ShareMessage invalid = validate_create(rules, target, request, today());
    if (!invalid.empty()) {
        message_ = invalid;
        out.error = error_from_key(invalid.key);
        log_failure(ShareOp::Create, 0);
        return out;
    }
    std::string password;   // a copy for the form encoder only; wiped with the body
    WipeOnExit wipe_password{&password};
    if (!request.password.empty()) password = request.password.reveal();
    std::vector<std::pair<std::string, std::string>> fields = {
        {"path", target.path}, {"shareType", "3"}, {"permissions", std::to_string(link_bits_for(request.permission))}};
    if (!request.password.empty()) fields.push_back({"password", password});
    if (request.expiry) fields.push_back({"expireDate", *request.expiry});
    std::string body = form_encode(fields);
    WipeOnExit wipe_body{&body};
    for (auto& f : fields) secure_wipe(f.second);
    const Response r = session_.send("POST", api_url("/shares?format=json"), body, share_headers(true), kMaxShareResponseBytes, kShareTotalTimeoutS, true);
    const Answer a = classify(r);
    if (a.kind != Answer::Kind::Ok) {
        out.error = refusal(r, a, ShareOp::Create, target, &request, nullptr, nullptr);
        return out;
    }
    auto detail = parse_share_detail(r.body, user_id_, host_, target.is_folder);
    if (!detail.ok()) {
        log_failure(ShareOp::Create, r.status);
        out.error = fail(detail.error.key);
        return out;
    }
    out.value = std::move(detail.value);
    log::event("share.create", {log::status(r.status)});
    return out;
}

Result<ShareDetail> ShareApi::detail(int64_t id, bool item_is_folder) {
    reset();
    Result<ShareDetail> out;
    if (id <= 0) {
        out.error = fail("protocol_error");
        return out;
    }
    const Response r = session_.send("GET", api_url("/shares/" + std::to_string(id) + "?format=json"), std::string(), share_headers(false), kMaxShareResponseBytes,
                                     kShareTotalTimeoutS);
    const Answer a = classify(r);
    if (a.kind != Answer::Kind::Ok) {
        out.error = refusal(r, a, ShareOp::Detail, ShareTarget(), nullptr, nullptr, nullptr);
        return out;
    }
    auto d = parse_share_detail(r.body, user_id_, host_, item_is_folder);
    if (!d.ok()) {
        log_failure(ShareOp::Detail, r.status);
        out.error = fail(d.error.key);
        return out;
    }
    out.value = std::move(d.value);
    log::event("share.open", {log::status(r.status), log::count("rows", 1)});
    return out;
}

Result<ShareDetail> ShareApi::change(const ShareTarget& target, const ShareRules& rules, int64_t id, const Snapshot& shown, LinkChange&& change) {
    reset();
    struct WipeChange {   // the password does not outlive this call, whatever happens
        LinkChange& c;
        ~WipeChange() { c.password.wipe(); }
    } wipe_change{change};
    Result<ShareDetail> out;
    // a share of someone else, or one that is not a public link, is not changed and nothing at all is sent for it
    if (id <= 0 || shown.kind != ShareKind::PublicLink || !shown.owned_by_me) {
        out.error = fail("share_not_changeable");
        log_failure(ShareOp::Change, 0);
        return out;
    }
    // R-7: read the share again and compare with what the screen showed; a difference sends nothing
    const auto fresh = detail(id, target.is_folder);
    if (!fresh.ok()) {   // share_gone, a refused login, no network ...: message_ was filled by detail()
        out.error = fresh.error;
        return out;
    }
    reset();
    if (snapshot_of(fresh.value.row) != shown) {
        current_ = fresh.value.row;
        message_.key = "share_changed_meanwhile";
        out.error = error_from_key("share_changed_meanwhile");
        log_failure(ShareOp::Change, 0);
        return out;
    }
    const ShareMessage invalid = validate_change(rules, target, fresh.value.row, change, today());
    if (!invalid.empty()) {
        message_ = invalid;
        out.error = error_from_key(invalid.key);
        log_failure(ShareOp::Change, 0);
        return out;
    }
    std::string password;   // a copy for the form encoder only; wiped with the body
    WipeOnExit wipe_password{&password};
    std::vector<std::pair<std::string, std::string>> fields;
    switch (change.action) {
        case LinkChange::Action::SetPassword:
            password = change.password.reveal();
            fields.push_back({"password", password});
            break;
        case LinkChange::Action::ClearPassword: fields.push_back({"password", ""}); break;
        case LinkChange::Action::SetExpiry: fields.push_back({"expireDate", change.expiry}); break;
        case LinkChange::Action::ClearExpiry: fields.push_back({"expireDate", ""}); break;
        case LinkChange::Action::SetPermission: fields.push_back({"permissions", std::to_string(link_bits_for(change.permission))}); break;
    }
    std::string body = form_encode(fields);
    WipeOnExit wipe_body{&body};
    for (auto& f : fields) secure_wipe(f.second);
    const Response r = session_.send("PUT", api_url("/shares/" + std::to_string(id) + "?format=json"), body, share_headers(true), kMaxShareResponseBytes,
                                     kShareTotalTimeoutS, true);
    const Answer a = classify(r);
    if (a.kind != Answer::Kind::Ok) {
        out.error = refusal(r, a, ShareOp::Change, target, nullptr, &change, &fresh.value.row);
        return out;
    }
    auto d = parse_share_detail(r.body, user_id_, host_, target.is_folder);
    if (!d.ok()) {
        log_failure(ShareOp::Change, r.status);
        out.error = fail(d.error.key);
        return out;
    }
    out.value = std::move(d.value);
    log::event("share.change", {log::status(r.status)});
    return out;
}

Error ShareApi::remove_link(int64_t id, const ShareTarget& target, const ShareRules& rules, bool owned_by_me) {
    reset();
    (void)rules;   // a removal has no rule to check; the rules are re-read only to explain a refusal (R-4)
    if (id <= 0 || !owned_by_me) {   // a share of someone else is not removed and nothing at all is sent for it
        log_failure(ShareOp::Remove, 0);
        return fail("share_not_changeable");
    }
    const Response r = session_.send("DELETE", api_url("/shares/" + std::to_string(id) + "?format=json"), std::string(), share_headers(false), kMaxShareResponseBytes,
                                     kShareTotalTimeoutS);
    const Answer a = classify(r);
    if (a.kind != Answer::Kind::Ok) return refusal(r, a, ShareOp::Remove, target, nullptr, nullptr, nullptr);
    log::event("share.remove", {log::status(r.status)});
    return Error();
}

}  // namespace pbc
