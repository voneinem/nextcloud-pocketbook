#include "pbc/nc_server.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "pbc/clock_check.h"
#include "pbc/curl_session.h"
#include "pbc/dav_common.h"
#include "pbc/error_keys.h"
#include "pbc/json_min.h"
#include "pbc/remote_path.h"
#include "pbc/version.h"

namespace pbc {

namespace {

constexpr size_t kStatusBodyLimit = 64 * 1024;
constexpr size_t kMarkerBytes = 512;

Result<std::string> addr_error(const char* key, const char* detail) {
    Result<std::string> r;
    r.error = make_error(key == std::string("scheme_not_allowed") ? ErrorClass::Tls : ErrorClass::Unsupported, key, detail);
    return r;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has_blank_or_control(std::string_view s) {
    for (unsigned char c : s)
        if (c <= 0x20 || c == 0x7f) return true;
    return false;
}

bool valid_host(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    if (host[0] == '[') {   // an IPv6 literal
        if (host.back() != ']' || host.size() < 3) return false;
        for (size_t i = 1; i + 1 < host.size(); ++i) {
            const char c = host[i];
            if (!(std::isxdigit(static_cast<unsigned char>(c)) || c == ':' || c == '.')) return false;
        }
        return true;
    }
    for (unsigned char c : host)
        if (!(std::isalnum(c) || c == '-' || c == '.' || c == '_')) return false;   // ASCII only: no silent IDN conversion
    return host.front() != '.' && host.front() != '-';
}

bool valid_path(std::string_view path) {
    for (unsigned char c : path)
        if (!(std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '%' || c == '/')) return false;
    return true;
}

std::string trim_blanks(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}

// Leading digits and dots, at most 32 characters; "" if there are none.
std::string sanitize_version(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (!((c >= '0' && c <= '9') || c == '.') || out.size() >= 32) break;
        out.push_back(c);
    }
    while (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

// Control characters removed, at most 64 bytes (never inside a UTF-8 character).
std::string sanitize_name(const std::string& n) {
    std::string out;
    for (unsigned char c : n)
        if (c >= 0x20 && c != 0x7f) out.push_back(static_cast<char>(c));
    if (out.size() > 64) {
        size_t cut = 64;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    return out;
}

int major_of(const std::string& version) { return std::atoi(version.c_str()); }

Error transport_error(const Response& r) {
    const ErrorClass cls = r.error == ErrorClass::None ? ErrorClass::Network : r.error;
    return make_error(cls, r.error_key.empty() ? "server_unreachable" : r.error_key, r.error_detail);
}

const JsonValue* path(const JsonValue& root, std::initializer_list<const char*> keys) {
    const JsonValue* v = &root;
    for (const char* k : keys) {
        v = v->get(k);
        if (!v) return nullptr;
    }
    return v;
}

// The OCS envelope: ocs.meta.statuscode 100 (v1) or 200 (v2) and the data member.
const JsonValue* ocs_data(const JsonValue& doc) {
    const JsonValue* code = path(doc, {"ocs", "meta", "statuscode"});
    if (!code) return nullptr;
    const auto n = code->as_int();
    if (!n || (*n != 100 && *n != 200)) return nullptr;
    return path(doc, {"ocs", "data"});
}

bool contains_marker(const std::string& body) {
    const std::string head = lower(body.substr(0, kMarkerBytes));
    return head.find("app password") != std::string::npos || head.find("app-password") != std::string::npos;
}

Headers ocs_headers() { return {{"OCS-APIRequest", "true"}, {"Accept", "application/json"}}; }

// Maps a non-2xx answer of an authenticated call.
Error answer_error(const DavSession& session, const Response& r, bool dav) {
    if (r.status == 401) return make_error(ErrorClass::Auth, contains_marker(r.body) ? "auth_app_password_required" : "auth_failed", "the server refused the login");
    if (dav && (r.status == 404 || r.status == 405 || r.status == 501)) return make_error(ErrorClass::Unsupported, "no_webdav", "the server has no file access at this address");
    return session.status_error(r);
}

// The capabilities call: the credentials are checked here, the server's own version and name are read.
Error read_capabilities(DavSession& session, const std::string& base_url, const std::string& agent, AccountCheck* out, const Clock& clock,
                        std::optional<int64_t>* retry_after) {
    Headers headers = ocs_headers();
    headers.push_back({"User-Agent", agent});
    const Response resp = session.send("GET", base_url + "/ocs/v1.php/cloud/capabilities?format=json", std::string(), headers, 1 << 20);
    if (resp.status < 200 || resp.status >= 300 || resp.status == 0 || resp.body_too_large) {
        const Error e = answer_error(session, resp, false);
        if (e.key == "rate_limited" || e.key == "maintenance") *retry_after = parse_retry_after(resp.headers, clock.now());
        return e;
    }
    const auto doc = parse_json(resp.body);
    const JsonValue* data = doc.ok() ? ocs_data(doc.value) : nullptr;
    const JsonValue* vs = data ? path(*data, {"version", "string"}) : nullptr;
    if (!vs || vs->type != JsonValue::Type::String) return make_error(ErrorClass::Protocol, "protocol_error", "the answer of the server is not usable");
    const std::string v = sanitize_version(vs->text);
    if (!v.empty()) {
        out->server.version = v;
        out->server.major = major_of(v);
    }
    if (const JsonValue* caps = path(*data, {"capabilities"})) out->caps = server_caps_from_capabilities(*caps);
    out->caps.checked_at = clock.now();
    if (const JsonValue* theming = path(*data, {"capabilities", "theming", "name"}))
        if (theming->type == JsonValue::Type::String) {
            const std::string name = sanitize_name(theming->text);
            if (!name.empty()) out->server.name = name;
        }
    return Error();
}

}  // namespace

Result<std::string> canonical_base_url(std::string_view typed) {
    const std::string in = trim_blanks(typed);
    if (in.empty()) return addr_error("address_invalid", "no address");
    for (unsigned char c : in)
        if (c < 0x20 || c == 0x7f) return addr_error("address_invalid", "the address has a control character");

    std::string rest;
    const size_t sep = in.find("://");
    if (sep != std::string::npos) {
        const std::string scheme = lower(in.substr(0, sep));
        if (scheme != "https") return addr_error("scheme_not_allowed", "only https addresses are accepted");
        rest = in.substr(sep + 3);
    } else {
        rest = in;
        if (rest[0] == '/') return addr_error("address_invalid", "the address has no host");
    }
    if (rest.find_first_of("?#") != std::string::npos) return addr_error("address_invalid", "the address has a query or a fragment");
    if (has_blank_or_control(rest)) return addr_error("address_invalid", "the address has a blank");

    const size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? std::string() : rest.substr(slash);
    if (authority.find('@') != std::string::npos) return addr_error("address_invalid", "the address has user information");

    std::string host = authority, port;
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return addr_error("address_invalid", "the address is not valid");
        host = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return addr_error("address_invalid", "the address is not valid");
            port = authority.substr(close + 2);
            if (port.empty()) return addr_error("address_invalid", "the port is empty");
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
            if (port.empty()) return addr_error("address_invalid", "the port is empty");
        }
    }
    host = lower(host);
    if (!valid_host(host)) return addr_error("address_invalid", "the host name is not valid");
    if (!port.empty()) {
        if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return addr_error("address_invalid", "the port is not valid");
        const long n = std::atol(port.c_str());
        if (n < 1 || n > 65535) return addr_error("address_invalid", "the port is not valid");
    }
    if (!valid_path(path)) return addr_error("address_invalid", "the path is not valid");
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path == "/") path.clear();

    Result<std::string> r;
    r.value = "https://" + host + (port.empty() ? std::string() : ":" + port) + path;
    return r;
}

Error classify_status_document(const Response& r, ServerInfo* out) {
    if (r.status == 0 || r.error != ErrorClass::None) return transport_error(r);
    if (r.status >= 500) return make_error(ErrorClass::Server, "server_error", "the server reported a problem");
    if (r.status != 200) return make_error(ErrorClass::Unsupported, "not_nextcloud", "this address does not answer like a Nextcloud");
    if (r.body_too_large || r.body.size() > kStatusBodyLimit) return make_error(ErrorClass::Unsupported, "not_nextcloud", "this address does not answer like a Nextcloud");

    JsonLimits limits;
    limits.max_bytes = kStatusBodyLimit;
    const auto doc = parse_json(r.body, limits);
    const char* not_nc = "this address does not answer like a Nextcloud";
    if (!doc.ok() || doc.value.type != JsonValue::Type::Object) return make_error(ErrorClass::Unsupported, "not_nextcloud", not_nc);
    const JsonValue* installed = doc.value.get("installed");
    const JsonValue* version = doc.value.get("version");
    if (!installed || installed->type != JsonValue::Type::Bool || !installed->boolean) return make_error(ErrorClass::Unsupported, "not_nextcloud", not_nc);
    if (!version || version->type != JsonValue::Type::String) return make_error(ErrorClass::Unsupported, "not_nextcloud", not_nc);
    const std::string v = sanitize_version(version->text);
    if (v.empty() || v[0] == '.') return make_error(ErrorClass::Unsupported, "not_nextcloud", not_nc);

    if (out) {
        out->version = v;
        out->major = major_of(v);
        const JsonValue* product = doc.value.get("productname");
        if (product && product->type == JsonValue::Type::String) {
            const std::string name = sanitize_name(product->text);
            if (!name.empty()) out->name = name;
        }
    }
    const JsonValue* maintenance = doc.value.get("maintenance");
    const JsonValue* upgrade = doc.value.get("needsDbUpgrade");
    if ((maintenance && maintenance->type == JsonValue::Type::Bool && maintenance->boolean) ||
        (upgrade && upgrade->type == JsonValue::Type::Bool && upgrade->boolean))
        return make_error(ErrorClass::Server, "maintenance", "the server is in maintenance mode");
    if (major_of(v) < kMinServerMajor) return make_error(ErrorClass::Unsupported, "server_too_old", "the server version is too old");
    return Error();
}

Result<AccountCheck> check_account(HttpTransport& http, const std::string& base_url, const std::string& login_name,
                                   const std::string& app_password, const Clock& clock) {
    Result<AccountCheck> r;
    auto fail = [&](Error e) {
        log_failure("account.test", e);
        r.error = std::move(e);
        return r;
    };
    // 0: local checks, before any connection
    if (clock.now() < kClockFloorEpoch) return fail(make_error(ErrorClass::Unsupported, "clock_implausible", "the reader's date and time are wrong"));
    const auto canonical = canonical_base_url(base_url);
    if (!canonical.ok() || canonical.value != base_url) return fail(canonical.ok() ? make_error(ErrorClass::Unsupported, "address_invalid", "the address is not in its canonical form") : canonical.error);

    const std::string agent = std::string("PBCloud/") + version();
    std::optional<int64_t> server_time;
    auto remember_date = [&](const Response& resp) {
        if (resp.status == 0) return;
        if (const auto t = parse_http_date(header_value(resp.headers, "Date"))) server_time = *t;
    };

    // 1: status.php, without credentials
    {
        Request req;
        req.method = "GET";
        req.url = base_url + "/status.php";
        req.headers = {{"User-Agent", agent}, {"Accept", "application/json"}};
        req.max_response_bytes = static_cast<long long>(kStatusBodyLimit);
        const Response resp = http.perform(req);
        remember_date(resp);
        Error e = classify_status_document(resp, &r.value.server);
        if (!e.ok()) {
            if (e.key == "rate_limited" || e.key == "maintenance") r.value.retry_after_s = parse_retry_after(resp.headers, clock.now());
            return fail(e);
        }
    }

    DavSession session(http, base_url, login_name, app_password);
    auto send = [&](const std::string& method, const std::string& url, const std::string& body, Headers headers) {
        headers.push_back({"User-Agent", agent});
        return session.send(method, url, body, headers, 1 << 20);
    };
    auto with_retry_after = [&](const Response& resp, Error e) {
        if (e.key == "rate_limited" || e.key == "maintenance") r.value.retry_after_s = parse_retry_after(resp.headers, clock.now());
        return fail(std::move(e));
    };

    // 2: capabilities (the credentials are checked here)
    {
        const Error e = read_capabilities(session, base_url, agent, &r.value, clock, &r.value.retry_after_s);
        if (!e.ok()) return fail(e);
    }

    // 3: the user id (it can differ from the login name; DAV paths use it)
    {
        const Response resp = send("GET", base_url + "/ocs/v1.php/cloud/user?format=json", std::string(), ocs_headers());
        if (resp.status < 200 || resp.status >= 300 || resp.status == 0 || resp.body_too_large) return with_retry_after(resp, answer_error(session, resp, false));
        const auto doc = parse_json(resp.body);
        const JsonValue* data = doc.ok() ? ocs_data(doc.value) : nullptr;
        const JsonValue* id = data ? data->get("id") : nullptr;
        if (!id || id->type != JsonValue::Type::String || id->text.size() > 256 || !is_valid_name(id->text))
            return fail(make_error(ErrorClass::Protocol, "protocol_error", "the answer of the server is not usable"));
        r.value.user_id = id->text;
    }

    // 4: file access and quota
    {
        Headers h = dav::dav_headers("0");
        const std::string body = std::string(dav::kXmlHeader) +
            "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:quota-available-bytes/><d:quota-used-bytes/></d:prop></d:propfind>";
        const Response resp = send("PROPFIND", base_url + "/remote.php/dav/files" + encode_path("/" + r.value.user_id) + "/", body, h);
        if (resp.status != 207) return with_retry_after(resp, answer_error(session, resp, true).ok() ? make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server") : answer_error(session, resp, true));
        const auto ms = dav::parse_multistatus(resp);
        if (!ms.ok()) return fail(ms.error);
        const XmlNode* response = ms.value.child(dav::kNsDav, "response");
        std::optional<int64_t> used, avail;
        if (response) {
            auto number = [&](const char* name) -> std::optional<int64_t> {
                const XmlNode* p = dav::prop_in_ok_propstat(*response, dav::kNsDav, name);
                if (!p) return std::nullopt;
                const std::string t = dav::trim(p->text);
                if (t.empty() || t.size() > 18) return std::nullopt;
                size_t i = t[0] == '-' ? 1 : 0;
                if (i >= t.size()) return std::nullopt;
                for (size_t k = i; k < t.size(); ++k)
                    if (t[k] < '0' || t[k] > '9') return std::nullopt;
                return std::atoll(t.c_str());
            };
            used = number("quota-used-bytes");
            avail = number("quota-available-bytes");
        }
        r.value.quota.used = used;
        if (avail) {
            if (*avail == -3) r.value.quota.unlimited = true;
            else if (*avail >= 0 && used) r.value.quota.total = *used + *avail;
        }
    }

    // the clock: the server's Date header against ours (no request of its own)
    if (const auto t = session.last_server_time()) server_time = *t;
    r.value.server_time = server_time;
    if (server_time) {
        const int64_t skew = clock_skew(clock.now(), *server_time);
        if (clock_skew_is_large(skew)) r.value.clock_warning_minutes = clock_skew_minutes(skew);
    }
    return r;
}

Result<AccountCheck> quick_check(HttpTransport& http, const std::string& base_url, const std::string& login_name,
                                 const std::string& app_password, const Clock& clock) {
    Result<AccountCheck> r;
    auto fail = [&](Error e) {
        log_failure("account.test", e);
        r.error = std::move(e);
        return r;
    };
    if (clock.now() < kClockFloorEpoch) return fail(make_error(ErrorClass::Unsupported, "clock_implausible", "the reader's date and time are wrong"));
    const auto canonical = canonical_base_url(base_url);
    if (!canonical.ok() || canonical.value != base_url) return fail(canonical.ok() ? make_error(ErrorClass::Unsupported, "address_invalid", "the address is not in its canonical form") : canonical.error);
    DavSession session(http, base_url, login_name, app_password);
    const Error e = read_capabilities(session, base_url, std::string("PBCloud/") + version(), &r.value, clock, &r.value.retry_after_s);
    if (!e.ok()) return fail(e);
    r.value.server_time = session.last_server_time();
    if (r.value.server_time) {
        const int64_t skew = clock_skew(clock.now(), *r.value.server_time);
        if (clock_skew_is_large(skew)) r.value.clock_warning_minutes = clock_skew_minutes(skew);
    }
    return r;
}

}  // namespace pbc
