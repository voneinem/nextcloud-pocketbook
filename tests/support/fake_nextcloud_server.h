#pragma once
// A stateful, in-process fake Nextcloud (specs/001-nextcloud-files-client, research R-17). It is an HttpTransport, so the code under
// test talks to it exactly as it talks to CurlTransport, and it implements the contract of contracts/server-requests.md - nothing
// more. What a test against it proves is "the app follows the contract", never "a real server behaves like this": every test that
// includes this header invokes PBC_FAKE_SERVER_TEST() (scripts/check-fake-markers.sh).
//
// Sections implemented here: 1 status, 2/3 OCS capabilities and user, 4 quota, 5 folder listing, 6 plain GET. Later stories add
// Range requests, REPORT/SEARCH and Login Flow v2 in the same class.
//
// Everything is reachable through a few knobs: the tree (add_folder, add_file, set_file, remove), the account (user, app password,
// quota, version, maintenance), failure injection (set_offline, set_interceptor, fail_next) and counters (request_count, count).

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "fake_transport.h"
#include "pbc/dav_common.h"
#include "pbc/http_transport.h"
#include "pbc/remote_path.h"

namespace pbtest {

class FakeNextcloud : public pbc::HttpTransport {
public:
    struct Node {
        bool folder = false;
        std::string content;            // files
        int64_t synthetic_size = -1;    // >= 0: the content is generated (byte i = (i * 31 + seed) & 0xff), nothing is stored
        uint8_t synthetic_seed = 0;
        int64_t mtime = 0;
        std::string etag;
        int64_t fileid = 0;
        std::string mime = "application/octet-stream";
        std::string permissions = "RGDNVW";
        bool favorite = false;
        std::string checksum;           // "SHA1:…" as the server would store it, "" = none
        bool encrypted = false;         // nc:is-encrypted
        // a listing entry the server reports in a broken way (research R-7): "" = normal
        std::string listing_quirk;      // "no_propstat_ok" | "no_etag_no_type"
        bool weak_etag = false;         // the server (or a proxy) reports a weak ETag (W/...)

        int64_t size() const { return synthetic_size >= 0 ? synthetic_size : static_cast<int64_t>(content.size()); }
        std::string bytes(int64_t offset, int64_t length) const {
            if (synthetic_size < 0) return content.substr(static_cast<size_t>(offset), static_cast<size_t>(length));
            std::string out(static_cast<size_t>(length), '\0');
            for (int64_t i = 0; i < length; ++i) out[static_cast<size_t>(i)] = static_cast<char>(((offset + i) * 31 + synthetic_seed) & 0xff);
            return out;
        }
    };

    explicit FakeNextcloud(std::string host = "cloud.example.org") : host_(std::move(host)) {
        Node root;
        root.folder = true;
        root.etag = next_etag();
        root.fileid = ++next_id_;
        tree_["/"] = root;
        inner_.set_handler([this](const pbc::Request& r) { return handle(r); });
    }

    // ---- the account ----
    std::string base_url() const { return "https://" + host_; }
    const std::string& login_name() const { return login_name_; }
    const std::string& user_id() const { return user_id_; }
    const std::string& app_password() const { return app_password_; }
    void set_account(std::string login_name, std::string user_id, std::string app_password) {
        login_name_ = std::move(login_name);
        user_id_ = std::move(user_id);
        app_password_ = std::move(app_password);
    }
    void set_version(std::string version) { version_ = std::move(version); }   // "31.0.4.1"
    void set_server_name(std::string name) { server_name_ = std::move(name); }
    void set_maintenance(bool on) { maintenance_ = on; }
    void set_quota(int64_t used, int64_t available) { quota_ = {used, available}; }
    void set_server_time(int64_t epoch) { server_time_ = epoch; }
    int64_t server_time() const { return server_time_; }
    void omit_date_header(bool on) { omit_date_ = on; }

    // ---- the tree ----
    void add_folder(const std::string& path) { put(path, folder_node()); }
    void add_file(const std::string& path, const std::string& content, int64_t mtime = 1790000000) {
        Node n;
        n.content = content;
        n.mtime = mtime;
        put(path, n);
    }
    void add_synthetic_file(const std::string& path, int64_t size, uint8_t seed = 7, int64_t mtime = 1790000000) {
        Node n;
        n.synthetic_size = size;
        n.synthetic_seed = seed;
        n.mtime = mtime;
        put(path, n);
    }
    void set_file(const std::string& path, const std::string& content, int64_t mtime) {   // a change on the server: new ETag, ancestors too
        Node& n = node(path);
        n.content = content;
        n.synthetic_size = -1;
        n.mtime = mtime;
        touch(path);
    }
    void remove(const std::string& path) {
        const std::string p = pbc::normalize_remote_path(path);
        for (auto it = tree_.begin(); it != tree_.end();) {
            if (it->first == p || it->first.compare(0, p.size() + 1, p + "/") == 0) it = tree_.erase(it);
            else ++it;
        }
        touch(pbc::parent_of(p));
    }
    void set_favorite(const std::string& path, bool on) { node(path).favorite = on; }
    void set_permissions(const std::string& path, const std::string& letters) { node(path).permissions = letters; }
    void set_encrypted(const std::string& path, bool on) { node(path).encrypted = on; }
    void set_mime(const std::string& path, const std::string& mime) { node(path).mime = mime; }
    void set_checksum(const std::string& path, const std::string& value) { node(path).checksum = value; }
    void set_quirk(const std::string& path, const std::string& quirk) { node(path).listing_quirk = quirk; }
    void set_weak_etag(const std::string& path, bool on) { node(path).weak_etag = on; }
    // A server that ignores Range and If-Range and always answers with the whole file.
    void set_ignore_range(bool on) { ignore_range_ = on; }
    Node& node(const std::string& path) { return tree_.at(pbc::normalize_remote_path(path)); }
    bool has(const std::string& path) const { return tree_.count(pbc::normalize_remote_path(path)) == 1; }
    std::string etag_of(const std::string& path) const { return tree_.at(pbc::normalize_remote_path(path)).etag; }

    // ---- Login Flow v2 (section 9) ----
    // variant: "" = a normal server; otherwise one of link_other_host, poll_other_host, poll_other_port, link_http, poll_http,
    // token_long, token_control, token_empty, token_512, missing_poll, not_json.
    void set_login_variant(std::string v) { login_variant_ = std::move(v); }
    // The user approved the login on the other device: the next poll returns these credentials once, and they become valid.
    void approve_login(const std::string& login_name, const std::string& app_password) {
        login_approved_ = true;
        login_delivered_ = false;
        pending_login_name_ = login_name;
        pending_app_password_ = app_password;
    }
    void set_login_server_url(std::string url) { login_server_url_ = std::move(url); }   // the "server" of the final answer
    void set_login_poll_status(long status) { login_poll_status_ = status; }              // any status for the poll (0 = normal)

    // ---- failure injection and counters ----
    void set_offline(bool on) { offline_ = on; }
    // Called before the normal handling; a returned response replaces it (the request is still counted).
    void set_interceptor(std::function<std::optional<pbc::Response>(const pbc::Request&)> f) { interceptor_ = std::move(f); }
    // The next `count` requests whose URL contains `url_part` (empty = all) answer `status` with a short body.
    void fail_next(long status, int count = 1, std::string url_part = std::string()) {
        fail_status_ = status;
        fail_count_ = count;
        fail_part_ = std::move(url_part);
    }
    size_t request_count() const { return log_.size(); }
    size_t count(const std::string& method) const {
        return static_cast<size_t>(std::count_if(log_.begin(), log_.end(), [&](const Logged& l) { return l.method == method; }));
    }
    struct Logged {
        std::string method, path, depth;
    };
    const std::vector<Logged>& log() const { return log_; }
    void clear_log() { log_.clear(); }
    const std::string& last_search_term() const { return last_search_term_; }   // what the server received, after unescaping
    // The depth headers seen on PROPFIND requests (a test proves "Depth: 1 only").
    std::vector<std::string> depths() const {
        std::vector<std::string> d;
        for (const auto& l : log_)
            if (l.method == "PROPFIND") d.push_back(l.depth);
        return d;
    }

    pbc::Response perform(const pbc::Request& request) override { return inner_.perform(request); }
    FakeTransport& transport() { return inner_; }   // the recorded requests (credentials, headers)

private:
    using Response = pbc::Response;

    static Node folder_node() {
        Node n;
        n.folder = true;
        return n;
    }
    std::string next_etag() {
        static const char* hex = "0123456789abcdef";
        uint64_t v = (++etag_counter_) * 0x9E3779B97F4A7C15ull;
        std::string out = "\"";
        for (int i = 0; i < 16; ++i) {
            out.push_back(hex[v & 15]);
            v >>= 4;
        }
        out += "\"";
        return out;
    }
    void put(const std::string& path, Node n) {
        const std::string p = pbc::normalize_remote_path(path);
        // missing parents are created, like a client would
        for (std::string parent = pbc::parent_of(p); parent != "/" && !tree_.count(parent); parent = pbc::parent_of(parent)) {
            Node f = folder_node();
            f.etag = next_etag();
            f.fileid = ++next_id_;
            f.mtime = n.mtime;
            tree_[parent] = f;
        }
        n.etag = next_etag();
        n.fileid = ++next_id_;
        if (n.folder && n.mtime == 0) n.mtime = 1790000000;
        tree_[p] = n;
        touch(pbc::parent_of(p));
    }
    void touch(std::string path) {   // a change below a folder changes its ETag (documented server behaviour, unverified: LV-N7)
        for (;;) {
            auto it = tree_.find(path);
            if (it != tree_.end()) it->second.etag = next_etag();
            if (path == "/") break;
            path = pbc::parent_of(path);
        }
    }
    int64_t folder_size(const std::string& p) const {
        int64_t total = 0;
        for (const auto& kv : tree_)
            if (!kv.second.folder && kv.first.compare(0, p == "/" ? 1 : p.size() + 1, p == "/" ? "/" : p + "/") == 0) total += kv.second.size();
        return total;
    }
    std::vector<std::string> children_of(const std::string& p) const {
        std::vector<std::string> out;
        for (const auto& kv : tree_)
            if (kv.first != "/" && pbc::parent_of(kv.first) == p) out.push_back(kv.first);
        return out;
    }

    // RFC 9110 IMF-fixdate with English names, independent of the process locale (Qt switches it to the user's)
    static std::string http_date(int64_t epoch) {
        static const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        time_t t = static_cast<time_t>(epoch);
        struct tm tmv;
        gmtime_r(&t, &tmv);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tmv.tm_wday], tmv.tm_mday, months[tmv.tm_mon], tmv.tm_year + 1900,
                      tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        return buf;
    }
    static std::string quote_json(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    }

    Response reply(long status, const std::string& body, const std::string& content_type = "application/json") {
        Response r;
        r.status = status;
        r.body = body;
        r.headers.push_back({"Content-Type", content_type});
        r.headers.push_back({"Content-Length", std::to_string(body.size())});
        if (!omit_date_) r.headers.push_back({"Date", http_date(server_time_)});
        return r;
    }
    Response unreachable() {
        Response r;
        r.error = pbc::ErrorClass::Network;
        r.error_key = "server_unreachable";
        r.error_detail = "the fake server is not reachable";
        return r;
    }
    bool authorized(const pbc::Request& r) const { return r.username == login_name_ && r.password == app_password_; }

    Response handle(const pbc::Request& r) {
        std::string path;
        Logged entry;
        entry.method = r.method;
        entry.depth = pbc::header_value(r.headers, "Depth");
        const std::string base = base_url();
        if (offline_ || r.url.compare(0, base.size(), base) != 0 || (r.url.size() > base.size() && r.url[base.size()] != '/' && r.url[base.size()] != '?')) {
            entry.path = "(unreachable)";
            log_.push_back(entry);
            return unreachable();
        }
        path = r.url.substr(base.size());
        entry.path = path;
        log_.push_back(entry);
        if (fail_count_ > 0 && (fail_part_.empty() || r.url.find(fail_part_) != std::string::npos)) {
            --fail_count_;
            return reply(fail_status_, "{\"message\":\"injected\"}");
        }
        if (interceptor_)
            if (auto injected = interceptor_(r)) return *injected;

        const std::string route = path.substr(0, path.find('?'));
        if (route == "/status.php") return status_php();
        if (route == "/index.php/login/v2" && r.method == "POST") return login_start();
        if (route == "/index.php/login/v2/poll" && r.method == "POST") return login_poll(r);
        if (route.compare(0, 7, "/ocs/v1") == 0 || route.compare(0, 7, "/ocs/v2") == 0) return ocs(r, route);
        const std::string files = "/remote.php/dav/files/";
        if (route.compare(0, files.size(), files) == 0) return dav(r, route);
        if ((route == "/remote.php/dav/" || route == "/remote.php/dav") && r.method == "SEARCH") return search(r);
        return reply(404, "{\"message\":\"not found\"}");
    }

    Response login_start() {
        const std::string v = login_variant_;
        if (v == "not_json") return reply(200, "<html>login</html>", "text/html");
        issued_token_ = std::string(64, 'a');
        for (size_t i = 0; i < issued_token_.size(); ++i) issued_token_[i] = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[(i * 7 + 3) % 62];
        std::string token_json = issued_token_;
        if (v == "token_long") token_json = issued_token_ = std::string(513, 'a');
        if (v == "token_512") token_json = issued_token_ = std::string(512, 'a');
        if (v == "token_control") token_json = "ab\\u0001cd";
        if (v == "token_empty") token_json = issued_token_ = "";
        std::string host = base_url();
        std::string link_base = host, poll_base = host;
        if (v == "link_other_host") link_base = "https://evil.example.org";
        if (v == "poll_other_host") poll_base = "https://evil.example.org";
        if (v == "poll_other_port") poll_base = host + ":8443";
        if (v == "link_http") link_base = "http://" + host_;
        if (v == "poll_http") poll_base = "http://" + host_;
        std::string body = "{";
        if (v != "missing_poll") body += "\"poll\":{\"token\":\"" + token_json + "\",\"endpoint\":\"" + poll_base + "/index.php/login/v2/poll\"},";
        body += "\"login\":\"" + link_base + "/index.php/login/v2/flow/" + issued_token_ + "\"}";
        login_approved_ = false;
        login_delivered_ = false;
        return reply(200, body);
    }
    Response login_poll(const pbc::Request& r) {
        if (login_poll_status_ != 0) return reply(login_poll_status_, "{}");
        if (issued_token_.empty() || r.body != "token=" + issued_token_) return reply(404, "{}");
        if (!login_approved_ || login_delivered_) return reply(404, "{}");
        login_delivered_ = true;   // the answer comes only once
        login_name_ = pending_login_name_;
        app_password_ = pending_app_password_;
        const std::string server = login_server_url_.empty() ? base_url() : login_server_url_;
        return reply(200, "{\"server\":\"" + server + "\",\"loginName\":\"" + quote_json(login_name_) + "\",\"appPassword\":\"" + quote_json(app_password_) + "\"}");
    }

    Response status_php() {
        std::string major = version_.substr(0, version_.find('.'));
        const size_t last = version_.rfind('.');
        const std::string versionstring = last == std::string::npos || version_.find('.') == last ? version_ : version_.substr(0, last);
        return reply(200, std::string("{\"installed\":true,\"maintenance\":") + (maintenance_ ? "true" : "false") +
                              ",\"needsDbUpgrade\":false,\"version\":\"" + version_ + "\",\"versionstring\":\"" + versionstring +
                              "\",\"edition\":\"\",\"productname\":\"Nextcloud\",\"extendedSupport\":false}");
    }

    Response ocs(const pbc::Request& r, const std::string& route) {
        if (maintenance_) return reply(503, "{\"message\":\"maintenance\"}");
        if (!authorized(r)) return reply(401, "{\"ocs\":{\"meta\":{\"status\":\"failure\",\"statuscode\":997,\"message\":\"Current user is not logged in\"},\"data\":[]}}");
        const std::string ok = "{\"ocs\":{\"meta\":{\"status\":\"ok\",\"statuscode\":100,\"message\":\"OK\"},\"data\":";
        if (route.find("/cloud/capabilities") != std::string::npos) {
            std::vector<std::string> parts;
            std::string v = version_;
            for (size_t p; (p = v.find('.')) != std::string::npos; v = v.substr(p + 1)) parts.push_back(v.substr(0, p));
            parts.push_back(v);
            while (parts.size() < 3) parts.push_back("0");
            return reply(200, ok + "{\"version\":{\"major\":" + parts[0] + ",\"minor\":" + parts[1] + ",\"micro\":" + parts[2] +
                                  ",\"string\":\"" + parts[0] + "." + parts[1] + "." + parts[2] + "\",\"edition\":\"\"},\"capabilities\":{\"theming\":{\"name\":\"" +
                                  quote_json(server_name_) + "\"},\"dav\":{\"chunking\":\"1.0\"}}}}}");
        }
        if (route.find("/cloud/user") != std::string::npos)
            return reply(200, ok + "{\"id\":\"" + quote_json(user_id_) + "\",\"display-name\":\"Demo User\"}}}");
        return reply(404, "{\"ocs\":{\"meta\":{\"status\":\"failure\",\"statuscode\":404,\"message\":\"not found\"},\"data\":[]}}");
    }

    std::string props_xml(const std::string& p, const Node& n, bool with_quota) {
        std::string x = "<d:propstat><d:prop>";
        x += "<d:getlastmodified>" + http_date(n.mtime) + "</d:getlastmodified>";
        if (n.listing_quirk != "no_etag_no_type") {
            x += "<d:getetag>" + etag_header(n) + "</d:getetag>";
            x += n.folder ? "<d:resourcetype><d:collection/></d:resourcetype>" : "<d:resourcetype/>";
        }
        if (!n.folder) {
            x += "<d:getcontenttype>" + pbc::dav::escape(n.mime) + "</d:getcontenttype>";
            x += "<d:getcontentlength>" + std::to_string(n.size()) + "</d:getcontentlength>";
            if (!n.checksum.empty()) x += "<oc:checksums><oc:checksum>" + n.checksum + "</oc:checksum></oc:checksums>";
        }
        x += "<oc:fileid>" + std::to_string(n.fileid) + "</oc:fileid>";
        x += "<oc:permissions>" + n.permissions + "</oc:permissions>";
        x += "<oc:size>" + std::to_string(n.folder ? folder_size(p) : n.size()) + "</oc:size>";
        x += std::string("<oc:favorite>") + (n.favorite ? "1" : "0") + "</oc:favorite>";
        if (n.encrypted) x += "<nc:is-encrypted>1</nc:is-encrypted>";
        if (with_quota) {
            const int64_t used = quota_ ? quota_->first : folder_size("/");
            const int64_t avail = quota_ ? quota_->second : -3;
            x += "<d:quota-used-bytes>" + std::to_string(used) + "</d:quota-used-bytes>";
            x += "<d:quota-available-bytes>" + std::to_string(avail) + "</d:quota-available-bytes>";
        }
        x += "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>";
        return x;
    }

    std::string response_xml(const std::string& p, const Node& n, bool with_quota) {
        std::string href = "/remote.php/dav/files" + pbc::encode_path("/" + user_id_);
        href += (p == "/") ? "/" : pbc::encode_path(p) + (n.folder ? "/" : "");
        if (n.listing_quirk == "no_propstat_ok")   // no 2xx propstat at all: the entry cannot be read
            return "<d:response><d:href>" + href + "</d:href><d:propstat><d:prop><d:getetag/></d:prop><d:status>HTTP/1.1 403 Forbidden</d:status></d:propstat></d:response>";
        return "<d:response><d:href>" + href + "</d:href>" + props_xml(p, n, with_quota) + "</d:response>";
    }

    Response dav(const pbc::Request& r, const std::string& route) {
        if (maintenance_) return reply(503, "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\"><s:message xmlns:s=\"http://sabredav.org/ns\">maintenance</s:message></d:error>", "application/xml");
        if (!authorized(r)) return reply(401, "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\"/>", "application/xml");
        // the account's own files only
        auto decoded = pbc::decode_href(route, user_id_);
        if (!decoded.ok()) return reply(decoded.error.key == "foreign_href" ? 404 : 400, "", "application/xml");
        const std::string p = decoded.value;
        if (r.method == "PROPFIND") return propfind(r, p);
        if (r.method == "REPORT") return report(r, p);
        if (r.method == "GET") return get(r, p);
        return reply(405, "", "application/xml");
    }

    // REPORT oc:filter-files with the favorite rule: every node flagged as a favorite, in path order
    Response report(const pbc::Request& r, const std::string& p) {
        if (p != "/" || r.body.find("<oc:filter-files") == std::string::npos) return reply(400, "", "application/xml");
        if (r.body.find("<oc:favorite>1</oc:favorite>") == std::string::npos) return reply(207, empty_multistatus(), "application/xml; charset=utf-8");
        std::string xml = multistatus_open();
        for (const auto& kv : tree_)
            if (kv.first != "/" && kv.second.favorite) xml += response_xml(kv.first, kv.second, false);
        return reply(207, xml + "</d:multistatus>", "application/xml; charset=utf-8");
    }
    // SEARCH d:basicsearch: names (displayname) that contain the literal, case-insensitive, newest first, at most nresults
    Response search(const pbc::Request& r) {
        if (maintenance_) return reply(503, "", "application/xml");
        if (!authorized(r)) return reply(401, "", "application/xml");
        if (r.body.find("<d:searchrequest") == std::string::npos) return reply(400, "", "application/xml");
        const size_t a = r.body.find("<d:literal>%");
        const size_t b = r.body.find("%</d:literal>");
        if (a == std::string::npos || b == std::string::npos || b < a + 12) return reply(400, "", "application/xml");
        std::string term = unescape_xml(r.body.substr(a + 12, b - a - 12));
        last_search_term_ = term;
        int limit = 100;
        const size_t n = r.body.find("<d:nresults>");
        if (n != std::string::npos) limit = std::atoi(r.body.c_str() + n + 12);
        auto lower = [](std::string v) {
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return v;
        };
        std::vector<std::pair<std::string, const Node*>> hits;
        for (const auto& kv : tree_)
            if (kv.first != "/" && lower(kv.first.substr(kv.first.rfind('/') + 1)).find(lower(term)) != std::string::npos) hits.push_back({kv.first, &kv.second});
        std::stable_sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) { return x.second->mtime > y.second->mtime; });
        std::string xml = multistatus_open();
        int sent = 0;
        for (const auto& h : hits) {
            if (sent++ >= limit) break;
            xml += response_xml(h.first, *h.second, false);
        }
        return reply(207, xml + "</d:multistatus>", "application/xml; charset=utf-8");
    }
    static std::string multistatus_open() {
        return "<?xml version=\"1.0\"?>\n<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\" xmlns:nc=\"http://nextcloud.org/ns\">";
    }
    static std::string empty_multistatus() { return multistatus_open() + "</d:multistatus>"; }
    static std::string unescape_xml(std::string v) {
        const std::pair<const char*, const char*> map[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
        for (const auto& m : map) {
            size_t pos = 0;
            while ((pos = v.find(m.first, pos)) != std::string::npos) {
                v.replace(pos, std::strlen(m.first), m.second);
                pos += std::strlen(m.second);
            }
        }
        return v;
    }

    Response propfind(const pbc::Request& r, const std::string& p) {
        const std::string depth = pbc::header_value(r.headers, "Depth");
        if (depth != "0" && depth != "1")   // "infinity" and a missing header are refused (the app never sends them)
            return reply(403, "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\"><d:propfind-finite-depth/></d:error>", "application/xml");
        auto it = tree_.find(p);
        if (it == tree_.end()) return reply(404, "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\"/>", "application/xml");
        const bool quota = r.body.find("quota-available-bytes") != std::string::npos && p == "/";
        std::string xml = "<?xml version=\"1.0\"?>\n<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\" xmlns:nc=\"http://nextcloud.org/ns\">";
        xml += response_xml(p, it->second, quota);
        if (depth == "1" && it->second.folder)
            for (const std::string& c : children_of(p)) xml += response_xml(c, tree_.at(c), false);
        xml += "</d:multistatus>";
        return reply(207, xml, "application/xml; charset=utf-8");
    }

    static std::string etag_header(const Node& n) { return n.weak_etag ? "W/" + n.etag : n.etag; }

    Response get(const pbc::Request& req, const std::string& p) {
        auto it = tree_.find(p);
        if (it == tree_.end()) return reply(404, "", "application/xml");
        if (it->second.folder) return reply(405, "", "application/xml");
        const Node& n = it->second;
        // RFC 7233: a ranged request is answered with 206 when the validator in If-Range is the current strong ETag
        const std::string range = pbc::header_value(req.headers, "Range");
        if (!ignore_range_ && range.compare(0, 6, "bytes=") == 0) {
            const std::string if_range = pbc::header_value(req.headers, "If-Range");
            if (if_range.empty() || (!n.weak_etag && if_range == n.etag)) {
                const int64_t size = n.size();
                const char* spec = range.c_str() + 6;
                char* end = nullptr;
                const long long from = std::strtoll(spec, &end, 10);
                long long to = size - 1;
                if (end && *end == '-' && *(end + 1) != '\0') to = std::min<long long>(std::strtoll(end + 1, nullptr, 10), size - 1);
                if (from < 0 || from >= size || to < from) {
                    Response r = reply(416, "", "application/xml");
                    r.headers.push_back({"Content-Range", "bytes */" + std::to_string(size)});
                    return r;
                }
                Response r = reply(206, n.bytes(from, to - from + 1), n.mime);
                r.headers.push_back({"Content-Range", "bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" + std::to_string(size)});
                r.headers.push_back({"ETag", etag_header(n)});
                r.headers.push_back({"Last-Modified", http_date(n.mtime)});
                r.headers.push_back({"Accept-Ranges", "bytes"});
                return r;
            }
        }
        Response r = reply(200, n.bytes(0, n.size()), n.mime);
        r.headers.push_back({"ETag", etag_header(n)});
        r.headers.push_back({"Last-Modified", http_date(n.mtime)});
        r.headers.push_back({"Accept-Ranges", "bytes"});
        return r;
    }

    std::string host_;
    std::string login_name_ = "demo", user_id_ = "demo", app_password_ = "app-pass-0001";
    std::string version_ = "31.0.4.1";
    std::string server_name_ = "Example Cloud";
    std::string last_search_term_;
    bool maintenance_ = false, offline_ = false, omit_date_ = false, ignore_range_ = false;
    std::optional<std::pair<int64_t, int64_t>> quota_;
    int64_t server_time_ = 1792497600;   // 2026-10-20 12:00:00 UTC, after pbc::kClockFloorEpoch
    std::map<std::string, Node> tree_;
    int64_t next_id_ = 100;
    uint64_t etag_counter_ = 0;
    std::vector<Logged> log_;
    std::function<std::optional<pbc::Response>(const pbc::Request&)> interceptor_;
    long fail_status_ = 0;
    int fail_count_ = 0;
    std::string fail_part_;
    std::string login_variant_, issued_token_, login_server_url_, pending_login_name_, pending_app_password_;
    bool login_approved_ = false, login_delivered_ = false;
    long login_poll_status_ = 0;
    FakeTransport inner_;
};

}  // namespace pbtest
