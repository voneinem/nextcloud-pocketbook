#include "pbc/dav_common.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "pbc/clock_check.h"

namespace pbc {

namespace dav {

std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

std::string trim(std::string s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool status_ok(const std::string& status_line) {
    // "HTTP/1.1 200 OK"
    const size_t sp = status_line.find(' ');
    if (sp == std::string::npos) return false;
    return std::atoi(status_line.c_str() + sp + 1) / 100 == 2;
}

int status_code(const std::string& status_line) {
    const size_t sp = status_line.find(' ');
    return sp == std::string::npos ? 0 : std::atoi(status_line.c_str() + sp + 1);
}

const XmlNode* prop_in_ok_propstat(const XmlNode& response, const char* ns, const char* name) {
    for (const XmlNode* ps : response.children_named(kNsDav, "propstat")) {
        const XmlNode* st = ps->child(kNsDav, "status");
        if (st && !status_ok(trim(st->text))) continue;
        const XmlNode* prop = ps->child(kNsDav, "prop");
        if (!prop) continue;
        if (const XmlNode* p = prop->child(ns, name)) return p;
    }
    return nullptr;
}

const XmlNode* ok_prop_set(const XmlNode& response) {
    for (const XmlNode* ps : response.children_named(kNsDav, "propstat")) {
        const XmlNode* st = ps->child(kNsDav, "status");
        if (st && !status_ok(trim(st->text))) continue;
        if (const XmlNode* prop = ps->child(kNsDav, "prop")) return prop;
    }
    return nullptr;
}

std::string path_of(const std::string& origin, const std::string& href) {
    if (href.compare(0, origin.size(), origin) == 0) return href.substr(origin.size());
    return href;
}

Headers dav_headers(const char* depth) { return {{"Depth", depth}, {"Content-Type", "application/xml; charset=utf-8"}}; }

Result<XmlNode> parse_multistatus(const Response& r) {
    Result<XmlNode> x = parse_xml(r.body);
    if (!x.ok()) return x;
    if (x.value.name != "multistatus" || x.value.ns != kNsDav) {
        x.error = make_error(ErrorClass::Protocol, "malformed_xml", "not a multistatus answer");
    }
    return x;
}

}  // namespace dav

DavSession::DavSession(HttpTransport& http, std::string base_url, std::string username, std::string password)
    : http_(http), base_url_(std::move(base_url)), user_(std::move(username)), password_(std::move(password)) {
    const size_t sep = base_url_.find("://");
    if (sep != std::string::npos) {
        const size_t slash = base_url_.find('/', sep + 3);
        origin_ = base_url_.substr(0, slash);
    }
}

std::string DavSession::url_for(const std::string& href) const {
    if (href.empty() || origin_.empty()) return std::string();
    if (href.compare(0, 2, "//") == 0) return std::string();   // a scheme-relative address names another host
    if (href[0] == '/') return origin_ + href;
    if (href.compare(0, origin_.size(), origin_) == 0 && (href.size() == origin_.size() || href[origin_.size()] == '/')) return href;
    if (href.find("://") != std::string::npos) return std::string();   // another host
    return origin_ + "/" + href;
}

Response DavSession::send(const std::string& method, const std::string& url, const std::string& body, const Headers& headers, long long cap) {
    Request r;
    r.method = method;
    r.url = url;
    r.body = body;
    r.headers = headers;
    r.username = user_;
    r.password = password_;
    r.max_response_bytes = cap;
    Response answer = http_.perform(r);
    if (answer.status != 0) {
        const std::string date = header_value(answer.headers, "Date");
        if (!date.empty())
            if (const auto t = parse_http_date(date)) server_time_ = *t;
    }
    return answer;
}

Response DavSession::send_file(const std::string& method, const std::string& url, const std::string& file, long long offset, long long length,
                               const Headers& headers, std::function<bool(long long, long long)> progress, long long cap) {
    Response none;
    if (url.empty() || origin_.empty()) return none;
    if (url.compare(0, origin_.size(), origin_) != 0 || (url.size() > origin_.size() && url[origin_.size()] != '/')) return none;   // another host
    Request r;
    r.method = method;
    r.url = url;
    r.headers = headers;
    r.body_file = file;
    r.body_offset = offset;
    r.body_length = length;
    r.progress = std::move(progress);
    r.username = user_;
    r.password = password_;
    r.max_response_bytes = cap;
    r.total_timeout_s = 0;   // a chunk may take as long as the connection allows; libcurl's low-speed guard ends a stalled transfer
    Response answer = http_.perform(r);
    if (answer.status != 0) {
        const std::string date = header_value(answer.headers, "Date");
        if (!date.empty())
            if (const auto t = parse_http_date(date)) server_time_ = *t;
    }
    return answer;
}

Error DavSession::status_error(const Response& r, bool* handled) const {
    if (handled) *handled = true;
    if (r.body_too_large) return make_error(ErrorClass::Protocol, "too_large", "the answer is larger than the limit");
    if (r.status == 0) {
        const ErrorClass cls = r.error == ErrorClass::None ? ErrorClass::Network : r.error;
        return make_error(cls, r.error_key.empty() ? "server_unreachable" : r.error_key, r.error_detail);
    }
    if (r.status == 401) return make_error(ErrorClass::Auth, "auth_failed", "the server refused the login");
    if (r.status == 403) return make_error(ErrorClass::Server, "forbidden", "the server refused the request");
    if (r.status == 404) return make_error(ErrorClass::Server, "not_found", "not found on the server");
    if (r.status == 409) return make_error(ErrorClass::Server, "target_gone", "a folder on the path is missing on the server");
    if (r.status == 412) return make_error(ErrorClass::Server, "changed_on_server", "the condition of the request was not met");
    if (r.status == 413) return make_error(ErrorClass::Server, "file_too_large_server", "the server refuses a request this large");
    if (r.status == 415) return make_error(ErrorClass::Server, "type_not_accepted", "the server does not accept this type of file");
    if (r.status == 507) return make_error(ErrorClass::Server, "server_storage_full", "the storage on the server is full");
    if (r.status == 423) return make_error(ErrorClass::Server, "locked", "the file is locked on the server");
    if (r.status == 429) return make_error(ErrorClass::Server, "rate_limited", "the server asks for a pause");
    if (r.status == 503) return make_error(ErrorClass::Server, "maintenance", "the server is not available (maintenance or overload)");
    if (r.status >= 500 || r.status == 408) return make_error(ErrorClass::Server, "server_error", "the server reported a problem");
    if (r.status >= 200 && r.status < 300) {
        if (handled) *handled = false;
        return Error();
    }
    return make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
}

std::optional<int64_t> parse_retry_after(const Headers& headers, int64_t now) {
    constexpr int64_t kCapSeconds = 900;
    const std::string raw = dav::trim(header_value(headers, "Retry-After"));
    if (raw.empty()) return std::nullopt;
    if (raw.find_first_not_of("0123456789") == std::string::npos) {   // delta-seconds
        if (raw.size() > 9) return kCapSeconds;                        // far beyond the cap, and too long to convert
        return std::min<int64_t>(std::atoll(raw.c_str()), kCapSeconds);
    }
    if (const auto when = parse_http_date(raw)) return std::min<int64_t>(std::max<int64_t>(*when - now, 0), kCapSeconds);
    return std::nullopt;
}

}  // namespace pbc
