#pragma once
// A stateful, in-process fake Nextcloud (specs/001-nextcloud-files-client, research R-17). It is an HttpTransport, so the code under
// test talks to it exactly as it talks to CurlTransport, and it implements the contract of contracts/server-requests.md - nothing
// more. What a test against it proves is "the app follows the contract", never "a real server behaves like this": every test that
// includes this header invokes PBC_FAKE_SERVER_TEST() (scripts/check-fake-markers.sh).
//
// Sections implemented here: 1 status, 2/3 OCS capabilities and user, 4 quota, 5 folder listing, 6 plain GET, Range requests,
// REPORT/SEARCH, Login Flow v2 and, for specs/002-manage-server-files, the write side (contracts/server-requests.md W3-W9): PUT, MKCOL,
// MOVE, COPY, DELETE with the conditions If-Match, If-None-Match and Overwrite, the trash, quota, locks, size and type limits, forbidden
// names, 207 partial failures, the chunked-upload area (/remote.php/dav/uploads/) and failure injection (lost answers, dropped bodies,
// ignored conditions, changes between a listing and a send).
//
// Everything is reachable through a few knobs: the tree (add_folder, add_file, set_file, remove), the account (user, app password,
// quota, version, maintenance), failure injection (set_offline, set_interceptor, fail_next) and counters (request_count, count).

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <cctype>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "fake_transport.h"
#include "pbc/dav_common.h"
#include "pbc/http_transport.h"
#include "pbc/remote_path.h"
#include "pbc/share_date.h"

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
        bool locked = false;            // a lock held by someone else: writes answer 423
        std::string sha256;             // of an uploaded file (hex), "" otherwise

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
    // A server that outlives a client that was killed (kill tests): the whole tree and its counters can be taken out and put back into another instance, so
    // that the ETags and file ids the client has recorded stay true.
    const std::map<std::string, Node>& nodes() const { return tree_; }
    uint64_t etag_counter() const { return etag_counter_; }
    int64_t file_id_counter() const { return next_id_; }
    void restore(const std::map<std::string, Node>& nodes, uint64_t etags, int64_t ids) {
        tree_ = nodes;
        etag_counter_ = etags;
        next_id_ = ids;
    }
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
    // Folder synchronization (specs/003-folder-sync): the checksums a server lists in oc:checksums, as "SHA256:<hex>[ SHA1:<hex>]" in one element.
    void set_checksums(const std::string& path, const std::string& sha256_hex, const std::string& sha1_hex = std::string()) {
        node(path).checksum = "SHA256:" + sha256_hex + (sha1_hex.empty() ? std::string() : " SHA1:" + sha1_hex);
    }
    // true: every file without an explicit checksum is listed with the SHA-256 of its content (a server that computes it by itself).
    void list_checksums(bool on) { list_checksums_ = on; }
    // A generated tree: `folders` folders d000.. below `root`, each with `files_per_folder` files f000.bin.. of `bytes` bytes of their own content.
    void add_tree(const std::string& root, int folders, int files_per_folder, int bytes = 1) {
        add_folder(root);
        char name[32];
        for (int d = 0; d < folders; ++d) {
            std::snprintf(name, sizeof name, "/d%03d", d);
            const std::string folder = pbc::normalize_remote_path(root) + name;
            add_folder(folder);
            for (int f = 0; f < files_per_folder; ++f) {
                std::snprintf(name, sizeof name, "/f%03d.bin", f);
                std::string content(static_cast<size_t>(bytes), static_cast<char>('a' + (d + f) % 26));
                Node n;
                n.content = content;
                n.mtime = 1790000000;
                put(folder + name, n);
            }
        }
    }
    // How the Depth 1 listing of one folder fails: "partial" (some members carry a non-200 status), "truncated" (the XML stops halfway),
    // "timeout" (no answer), "error" (a 500). "" clears. Listings of other folders and Depth 0 requests are not affected.
    void set_listing_failure(const std::string& folder, const std::string& mode) {
        const std::string p = pbc::normalize_remote_path(folder);
        if (mode.empty()) listing_failures_.erase(p);
        else listing_failures_[p] = mode;
    }
    // A change by someone else: new content, size, modification time and ETag (the ancestors' ETags change too).
    void touch_file(const std::string& path, const std::string& content, int64_t mtime) { set_file(path, content, mtime); }
    // An entry the user cannot use: "no_permission" (no read right), "encrypted" (end-to-end encrypted) or "unsupported" (an odd state).
    void set_unavailable(const std::string& path, const std::string& kind) {
        Node& n = node(path);
        if (kind == "no_permission") n.permissions = "RDNVW";
        else if (kind == "encrypted") n.encrypted = true;
        else if (kind == "unsupported") n.listing_quirk = "no_etag_no_type";
    }
    // A member the Depth 1 listing of `folder` carries in addition to the tree: <d:response> with the given (already encoded) name as the last
    // segment of its address. For names the tree cannot hold ("..", an encoded separator).
    void add_raw_member(const std::string& folder, const std::string& encoded_name) { raw_members_[pbc::normalize_remote_path(folder)].push_back(encoded_name); }
    void set_quirk(const std::string& path, const std::string& quirk) { node(path).listing_quirk = quirk; }
    void set_weak_etag(const std::string& path, bool on) { node(path).weak_etag = on; }
    // A server that ignores Range and If-Range and always answers with the whole file.
    void set_ignore_range(bool on) { ignore_range_ = on; }
    // A server (or a proxy) that ignores If-Match on a GET: the answer then carries whatever version the file has now.
    void set_ignore_get_if_match(bool on) { ignore_get_if_match_ = on; }
    Node& node(const std::string& path) { return tree_.at(pbc::normalize_remote_path(path)); }
    bool has(const std::string& path) const { return tree_.count(pbc::normalize_remote_path(path)) == 1; }
    std::vector<std::string> children(const std::string& path) const { return children_of(pbc::normalize_remote_path(path)); }   // sorted by path
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

    // ---- the write side (specs/002-manage-server-files) ----
    void set_trash(std::optional<bool> on) { trash_cap_ = on; trash_enabled_ = on.value_or(false); }   // nullopt: the capability is absent (unknown)
    void set_chunking(bool on) { chunking_cap_ = on; }
    void set_max_chunk(std::optional<int64_t> n) { max_chunk_cap_ = n; }
    // The lists the capabilities state and the server enforces (400 with an InvalidPath body).
    void set_forbidden(std::string chars, std::vector<std::string> names = {}, std::vector<std::string> basenames = {}, std::vector<std::string> extensions = {}) {
        forbidden_chars_ = std::move(chars);
        forbidden_names_ = std::move(names);
        forbidden_basenames_ = std::move(basenames);
        forbidden_extensions_ = std::move(extensions);
    }
    void set_state_forbidden_in_caps(bool on) { forbidden_in_caps_ = on; }   // false: the server enforces the lists but does not state them
    // A real quota: writes that would exceed `total` bytes in the tree answer 507; PROPFIND reports used and available from it.
    void set_keep_bytes_limit(long long n) { keep_limit_ = n; }   // uploads (and chunks) longer than this are hashed and counted, not kept (budget tests)
    void set_quota_limit(std::optional<int64_t> total) { quota_limit_ = total; }
    void set_max_file_size(std::optional<int64_t> n) { max_file_size_ = n; }              // 413 above it
    void set_blocked_extensions(std::vector<std::string> exts) { blocked_ext_ = std::move(exts); }   // 415, e.g. ".exe"
    void lock(const std::string& path, bool on = true) { node(path).locked = on; }
    void set_opaque_file_ids(bool on) { opaque_ids_ = on; }          // OC-FileId like "00000123ocabc" instead of a plain number
    void set_answer_etag_headers(bool on) { answer_etag_headers_ = on; }   // false: PUT/MKCOL/MOVE answers carry no OC-Etag/ETag (the app asks with a PROPFIND)
    int64_t trash_count() const { return static_cast<int64_t>(trash_.size()); }
    bool trash_has(const std::string& path) const {
        for (const auto& t : trash_)
            if (t.path == pbc::normalize_remote_path(path)) return true;
        return false;
    }
    int overwrites() const { return overwrites_; }                    // targets removed by Overwrite: T
    int permanent_deletes() const { return permanent_deletes_; }       // deletes that happened with no trash to catch them
    std::string sha256_of(const std::string& path) const { return tree_.at(pbc::normalize_remote_path(path)).sha256; }
    long long bytes_uploaded() const { return bytes_uploaded_; }       // request body bytes the server read
    long long bytes_downloaded() const { return bytes_downloaded_; }   // GET body bytes it sent
    // The chunked-upload area.
    size_t upload_sessions() const { return sessions_.size(); }
    std::vector<std::string> upload_session_ids() const {
        std::vector<std::string> ids;
        for (const auto& kv : sessions_) ids.push_back(kv.first);
        return ids;
    }
    std::vector<std::string> chunk_names(const std::string& id) const {
        std::vector<std::string> names;
        auto it = sessions_.find(id);
        if (it != sessions_.end())
            for (const auto& kv : it->second.chunks) names.push_back(kv.first);
        return names;
    }
    long long chunk_size_of(const std::string& id, const std::string& name) const { return sessions_.at(id).chunks.at(name).size; }
    void expire_uploads() { sessions_.clear(); }                        // the 24 hours passed
    // A chunk the server holds with the wrong size (a body that was cut): the next listing of the upload folder shows it.
    void plant_partial_chunk(const std::string& id, const std::string& name, long long size) {
        Chunk c;
        c.size = size;
        c.bytes.assign(static_cast<size_t>(std::min<long long>(size, 1 << 20)), 'x');
        sessions_.at(id).chunks[name] = c;
    }
    // Failure injection for the write side.
    // The next request of this method whose URL contains url_part is processed in full and then the transport reports a timeout: no answer.
    void lose_next_answer(const std::string& method, const std::string& url_part = std::string()) {
        lose_method_ = method;
        lose_part_ = url_part;
        lose_armed_ = true;
    }
    // The next request body longer than `bytes` is cut after that many bytes: nothing is stored, the transport reports a dropped connection.
    void drop_after_bytes(long long bytes) { drop_after_ = bytes; }
    // true: a cut chunk body leaves a partial chunk in the upload folder (a server that keeps what arrived)
    void set_partial_chunks_visible(bool on) { partial_visible_ = on; }
    // The next request of this method whose URL contains url_part answers `status` (with Retry-After when given) without being processed.
    void answer_next(const std::string& method, const std::string& url_part, long status, int count = 1, const std::string& retry_after = std::string()) {
        answer_method_ = method;
        answer_part_ = url_part;
        answer_status_ = status;
        answer_count_ = count;
        answer_retry_ = retry_after;
    }
    // The server ignores If-Match, If-None-Match and Overwrite on the final MOVE of a chunked upload (and, with the second switch, on MOVE and COPY).
    void ignore_conditions_on_assemble(bool on) { ignore_assemble_conditions_ = on; }
    void ignore_conditions_on_move(bool on) { ignore_move_conditions_ = on; }
    // Runs `fn` just before the next request of this method whose URL contains url_part is handled (a change between a listing and a send).
    // `skip` matching requests pass first (before_next("PROPFIND", part, fn, 1) acts before the second one).
    void before_next(const std::string& method, const std::string& url_part, std::function<void()> fn, int skip = 0) {
        before_method_ = method;
        before_part_ = url_part;
        before_fn_ = std::move(fn);
        before_skip_ = skip;
    }

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
    // The requests of any of these methods (a test proves "no write request at all").
    size_t count_matching(const std::vector<std::string>& methods) const {
        return static_cast<size_t>(std::count_if(log_.begin(), log_.end(), [&](const Logged& l) {
            return std::find(methods.begin(), methods.end(), l.method) != methods.end();
        }));
    }
    struct Logged {
        std::string method, path, depth;
        // the conditions and headers of a write request (specs/002-manage-server-files): "" = not sent
        std::string if_match, if_none_match, overwrite, destination, total_length, mtime_header, checksum_header;
        long long body_bytes = 0;
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

    // ---- share links (specs/004-share-links, contracts/share-requests.md) ----
    // The shares side: list, inherited, one share, create, change, remove; the sharing rules of the server are switches that show in the
    // capabilities AND are enforced with statuses that do not name the rule in text (the wording of meta.message is another switch).
    struct Share {
        int64_t id = 0;
        int type = 3;                 // 0 user, 1 group, 3 public link, 4 mail, ...
        std::string path;
        std::string owner;            // "" = the signed-in user
        int permissions = 1;
        std::string expiration;       // "YYYY-MM-DD" or ""
        std::string password;         // the clear text the fake keeps to check; "" = none
        int password_style = 0;       // 0: password and share_with hold a hash, 1: only password, 2: only share_with
        std::string label, with, with_display, token;
        std::string url_override;     // "" = <base>/s/<token>
    };
    struct ShareLogged {
        std::string method, route, url, query, content_type, body, ocs_header, accept, user_agent;
    };
    int64_t add_share(Share s) {
        s.id = ++next_share_id_;
        if (s.owner.empty()) s.owner = user_id_;
        if (s.token.empty() && s.type == 3) s.token = "TOK" + std::to_string(s.id);
        shares_.push_back(s);
        return s.id;
    }
    void add_generated_shares(const std::string& path, int n) {
        for (int i = 0; i < n; ++i) {
            Share s;
            s.path = path;
            s.label = "Link " + std::to_string(i + 1);
            add_share(s);
        }
    }
    const Share* find_share(int64_t id) const {
        for (const Share& s : shares_)
            if (s.id == id) return &s;
        return nullptr;
    }
    size_t share_count() const { return shares_.size(); }
    // A change by someone else (tests of "changed meanwhile"): another password state, permission bits or expiry on a stored share.
    void set_share_password(int64_t id, const std::string& password) { if (Share* s = mutable_share(id)) s->password = password; }
    void set_share_permissions(int64_t id, int permissions) { if (Share* s = mutable_share(id)) s->permissions = permissions; }
    void set_share_expiration(int64_t id, const std::string& day) { if (Share* s = mutable_share(id)) s->expiration = day; }
    void remove_share(int64_t id) {
        shares_.erase(std::remove_if(shares_.begin(), shares_.end(), [&](const Share& s) { return s.id == id; }), shares_.end());
    }
    void set_inherited_shares(const std::string& path, std::vector<Share> shares) {
        for (Share& s : shares) {
            s.id = ++next_share_id_;
            if (s.owner.empty()) s.owner = user_id_;
            if (s.token.empty() && s.type == 3) s.token = "TOK" + std::to_string(s.id);
        }
        inherited_[path] = std::move(shares);
    }
    void set_inherited_status(long status) { inherited_status_ = status; }   // 0 = offered; 404/405/501 = not offered
    void set_sharing_enabled(bool on) { sharing_enabled_ = on; }
    void set_public_links_enabled(bool on) { public_links_enabled_ = on; }
    void set_password_enforced(bool on) { pw_enforced_ = on; pw_for_set_ = false; }
    void set_password_enforced_for(bool read_only, bool read_write, bool upload_only) {
        pw_for_set_ = true;
        pw_for_ro_ = read_only;
        pw_for_rw_ = read_write;
        pw_for_uo_ = upload_only;
    }
    void set_expiry(bool enabled, bool enforced, int days) { exp_enabled_ = enabled; exp_enforced_ = enforced; exp_days_ = days; }
    void set_public_upload(bool on) { public_upload_ = on; }
    void set_sharing_rules_in_caps(bool on) { sharing_caps_ = on; }   // false: the capabilities say nothing about sharing
    void set_unshareable(const std::string& path) { unshareable_.insert(path); }
    void set_share_wording(int wording) { share_wording_ = wording; }   // meta.message in another wording/language; the statuses stay
    void drop_before_processing(const std::string& method, const std::string& url_part = std::string()) {
        drop_method_ = method;
        drop_part_ = url_part;
        drop_armed_ = true;
    }
    const std::vector<ShareLogged>& share_log() const { return share_log_; }
    size_t link_hits() const { return link_hits_; }   // requests whose address is that of a share link: the app must make none

    pbc::Response perform(const pbc::Request& request) override { return inner_.perform(request); }
    FakeTransport& transport() { return inner_; }   // the recorded requests (credentials, headers)

private:
    using Response = pbc::Response;

    // ---- the write side ----
    struct Chunk {
        std::string bytes;
        long long size = 0;
    };
    struct Session {
        std::string destination;           // the final path in the files tree
        long long total_length = -1;
        std::map<std::string, Chunk> chunks;
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> running{EVP_MD_CTX_new(), EVP_MD_CTX_free};   // SHA-256 over the chunks in order
        int next_in_order = 1;
        bool running_valid = true;
        Session() { EVP_DigestInit_ex(running.get(), EVP_sha256(), nullptr); }
    };
    struct Trashed {
        std::string path;
        bool folder = false;
        int64_t size = 0;
    };


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
            if (c == '"' || c == '\\') {
                out.push_back('\\');
                out.push_back(c);
            } else if (static_cast<unsigned char>(c) < 0x20) {   // valid JSON: a real server escapes control characters
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out.push_back(c);
            }
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
        if (is_link_url(r.url)) ++link_hits_;
        if (offline_ || r.url.compare(0, base.size(), base) != 0 || (r.url.size() > base.size() && r.url[base.size()] != '/' && r.url[base.size()] != '?')) {
            entry.path = "(unreachable)";
            log_.push_back(entry);
            return unreachable();
        }
        path = r.url.substr(base.size());
        entry.path = path;
        entry.if_match = pbc::header_value(r.headers, "If-Match");
        entry.if_none_match = pbc::header_value(r.headers, "If-None-Match");
        entry.overwrite = pbc::header_value(r.headers, "Overwrite");
        entry.destination = pbc::header_value(r.headers, "Destination");
        entry.total_length = pbc::header_value(r.headers, "OC-Total-Length");
        entry.mtime_header = pbc::header_value(r.headers, "X-OC-MTime");
        if (entry.mtime_header.empty()) entry.mtime_header = pbc::header_value(r.headers, "X-OC-Mtime");
        entry.checksum_header = pbc::header_value(r.headers, "OC-Checksum");
        entry.body_bytes = r.body_file.empty() ? static_cast<long long>(r.body.size()) : r.body_length;
        log_.push_back(entry);
        if (before_fn_ && r.method == before_method_ && (before_part_.empty() || r.url.find(before_part_) != std::string::npos)) {
            if (before_skip_ > 0) {
                --before_skip_;
            } else {
                auto fn = std::move(before_fn_);
                before_fn_ = nullptr;
                fn();
            }
        }
        if (answer_count_ > 0 && r.method == answer_method_ && (answer_part_.empty() || r.url.find(answer_part_) != std::string::npos)) {
            --answer_count_;
            Response injected = reply(answer_status_, "", "application/xml");
            if (!answer_retry_.empty()) injected.headers.push_back({"Retry-After", answer_retry_});
            return injected;
        }
        if (fail_count_ > 0 && (fail_part_.empty() || r.url.find(fail_part_) != std::string::npos)) {
            --fail_count_;
            return reply(fail_status_, "{\"message\":\"injected\"}");
        }
        if (interceptor_)
            if (auto injected = interceptor_(r)) return *injected;
        if (drop_armed_ && r.method == drop_method_ && (drop_part_.empty() || r.url.find(drop_part_) != std::string::npos)) {
            drop_armed_ = false;
            return unreachable();   // the connection never got the request: nothing was processed
        }
        if (lose_armed_ && r.method == lose_method_ && (lose_part_.empty() || r.url.find(lose_part_) != std::string::npos)) {
            lose_armed_ = false;
            (void)dispatch(r, path);   // the server did the work ...
            Response lost;             // ... and the answer never arrived
            lost.error = pbc::ErrorClass::Network;
            lost.error_key = "timeout";
            lost.error_detail = "no answer";
            lost.delivery = pbc::Delivery::FullySent;
            return lost;
        }
        return dispatch(r, path);
    }

    Response dispatch(const pbc::Request& r, const std::string& path) {

        const std::string route = path.substr(0, path.find('?'));
        if (route == "/status.php") return status_php();
        if (route == "/index.php/login/v2" && r.method == "POST") return login_start();
        if (route == "/index.php/login/v2/poll" && r.method == "POST") return login_poll(r);
        if (route.compare(0, 7, "/ocs/v1") == 0 || route.compare(0, 7, "/ocs/v2") == 0) return ocs(r, route);
        const std::string files = "/remote.php/dav/files/";
        if (route.compare(0, files.size(), files) == 0) return dav(r, route);
        if ((route == "/remote.php/dav/" || route == "/remote.php/dav") && r.method == "SEARCH") return search(r);
        const std::string uploads = "/remote.php/dav/uploads/";
        if (route.compare(0, uploads.size(), uploads) == 0) return uploads_area(r, route);
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
                                  quote_json(server_name_) + "\"}," + dav_and_files_caps() + sharing_caps_json() + "}}}}");
        }
        if (route.find("/apps/files_sharing/api/v1/shares") != std::string::npos) return shares(r, route);
        if (route.find("/cloud/user") != std::string::npos)
            return reply(200, ok + "{\"id\":\"" + quote_json(user_id_) + "\",\"display-name\":\"Demo User\"}}}");
        return reply(404, "{\"ocs\":{\"meta\":{\"status\":\"failure\",\"statuscode\":404,\"message\":\"not found\"},\"data\":[]}}");
    }

    // ---- share links ----
    Share* mutable_share(int64_t id) {
        for (Share& s : shares_)
            if (s.id == id) return &s;
        return nullptr;
    }
    static std::string pct_decode(const std::string& v) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '%' && i + 2 < v.size() + 0 && std::isxdigit(static_cast<unsigned char>(v[i + 1])) && std::isxdigit(static_cast<unsigned char>(v[i + 2]))) {
                out.push_back(static_cast<char>(std::stoi(v.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(v[i]);
            }
        }
        return out;
    }
    static std::map<std::string, std::string> parse_pairs(const std::string& text) {
        std::map<std::string, std::string> out;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t amp = text.find('&', pos);
            if (amp == std::string::npos) amp = text.size();
            const std::string pair = text.substr(pos, amp - pos);
            if (!pair.empty()) {
                const size_t eq = pair.find('=');
                const std::string name = pct_decode(pair.substr(0, eq));
                if (!out.count(name)) out[name] = eq == std::string::npos ? std::string() : pct_decode(pair.substr(eq + 1));
            }
            pos = amp + 1;
        }
        return out;
    }
    std::string share_link(const Share& s) const { return s.url_override.empty() ? base_url() + "/s/" + s.token : s.url_override; }
    bool is_link_url(const std::string& url) const {
        if (url.compare(0, base_url().size() + 3, base_url() + "/s/") == 0) return true;
        for (const Share& s : shares_)
            if (s.type == 3 && !s.url_override.empty() && s.url_override == url) return true;
        return false;
    }
    std::string share_message(const char* key) const {
        static const std::map<std::string, std::pair<std::string, std::string>> table = {
            {"public", {"Public sharing is not allowed", "Oeffentliches Teilen ist nicht erlaubt"}},
            {"password", {"A password is required for link shares", "Fuer Link-Freigaben ist ein Passwort erforderlich"}},
            {"expiry", {"An expiration date is required", "Ein Ablaufdatum ist erforderlich"}},
            {"upload", {"Public upload is not allowed", "Oeffentliches Hochladen ist nicht erlaubt"}},
            {"item", {"You are not allowed to share this item", "Sie duerfen dieses Objekt nicht teilen"}},
            {"path", {"Wrong path, file or folder does not exist", "Falscher Pfad"}},
            {"off", {"Sharing is disabled", "Teilen ist deaktiviert"}},
            {"owner", {"You are not the owner of this share", "Sie sind nicht der Eigentuemer"}},
            {"type", {"Unsupported share type", "Nicht unterstuetzter Freigabetyp"}},
            {"id", {"Wrong share ID, share does not exist", "Falsche Freigabe-ID"}},
            {"auth", {"Current user is not logged in", "Nicht angemeldet"}},
            {"bad", {"Invalid request", "Ungueltige Anfrage"}},
        };
        const auto it = table.find(key);
        if (it == table.end()) return "error";
        return share_wording_ == 0 ? it->second.first : it->second.second;
    }
    Response ocs_ok(bool v2, const std::string& data) {
        return reply(200, std::string("{\"ocs\":{\"meta\":{\"status\":\"ok\",\"statuscode\":") + (v2 ? "200" : "100") + ",\"message\":\"OK\"},\"data\":" + data + "}}");
    }
    Response ocs_fail(bool v2, int code, const char* what) {
        return reply(v2 ? code : 200, "{\"ocs\":{\"meta\":{\"status\":\"failure\",\"statuscode\":" + std::to_string(code) + ",\"message\":\"" +
                                          quote_json(share_message(what)) + "\"},\"data\":[]}}");
    }
    std::string share_json(const Share& s) const {
        const bool folder = [&] {
            const auto it = tree_.find(s.path);
            return it == tree_.end() ? true : it->second.folder;
        }();
        auto str = [&](const std::string& v) { return "\"" + quote_json(v) + "\""; };
        const std::string hash = "$2y$10$examplehashexamplehashexample";
        std::string j = "{\"id\":" + str(std::to_string(s.id)) + ",\"share_type\":" + std::to_string(s.type) + ",\"uid_owner\":" + str(s.owner) +
                        ",\"displayname_owner\":" + str(s.owner) + ",\"permissions\":" + std::to_string(s.permissions) + ",\"item_type\":" +
                        str(folder ? "folder" : "file") + ",\"path\":" + str(s.path) + ",\"stime\":1790000000,\"label\":" + str(s.label);
        if (s.type == 3) {
            j += ",\"token\":" + str(s.token) + ",\"url\":" + str(share_link(s));
            const bool has = !s.password.empty();
            j += std::string(",\"password\":") + (has && s.password_style != 2 ? str(hash) : "null");
            j += std::string(",\"share_with\":") + (has && s.password_style != 1 ? str(hash) : "null");
        } else {
            j += ",\"share_with\":" + str(s.with) + ",\"share_with_displayname\":" + str(s.with_display.empty() ? s.with : s.with_display) + ",\"token\":null";
        }
        j += std::string(",\"expiration\":") + (s.expiration.empty() ? "null" : str(s.expiration + " 00:00:00")) + "}";
        return j;
    }
    bool password_needed(int perms) const {
        if (pw_for_set_) {
            if (perms == 4) return pw_for_uo_;
            if (perms & 4) return pw_for_rw_;
            return pw_for_ro_;
        }
        return pw_enforced_;
    }
    // 0 = fine, else the HTTP/OCS status and *what; used by create and change
    int check_link_fields(const std::map<std::string, std::string>& f, bool creating, const Share* existing, const char** what) const {
        const int64_t today = server_time_ / 86400;
        int perms = existing ? existing->permissions : 1;
        if (f.count("permissions")) {
            char* end = nullptr;
            perms = static_cast<int>(std::strtol(f.at("permissions").c_str(), &end, 10));
            if (end == f.at("permissions").c_str() || perms < 1 || perms > 31) { *what = "bad"; return 400; }
        }
        const std::string path = existing ? existing->path : (f.count("path") ? f.at("path") : std::string());
        const auto node = tree_.find(path);
        const bool folder = node != tree_.end() && node->second.folder;
        if ((perms & 4) && !folder) { *what = "bad"; return 400; }
        if ((perms & 4) && !public_upload_) { *what = "upload"; return 403; }
        std::string password = existing ? existing->password : std::string();
        if (f.count("password")) password = f.at("password");
        if (password_needed(perms) && password.empty()) { *what = "password"; return 400; }
        std::string expiry = existing ? existing->expiration : std::string();
        if (f.count("expireDate")) expiry = f.at("expireDate");
        if (exp_enforced_ && expiry.empty()) { *what = "expiry"; return 400; }
        if (!expiry.empty()) {
            const auto day = pbc::parse_iso_day(expiry);
            if (!day || *day < today) { *what = "expiry"; return 400; }
            if (exp_enforced_ && *day > today + exp_days_) { *what = "expiry"; return 400; }
        }
        (void)creating;
        return 0;
    }

    Response shares(const pbc::Request& r, const std::string& route) {
        const bool v2 = route.compare(0, 7, "/ocs/v2") == 0;
        const size_t api = route.find("/api/v1");
        const std::string sub = api == std::string::npos ? route : route.substr(api + 7);   // "/shares", "/shares/inherited", "/shares/12"
        const size_t q = r.url.find('?');
        const std::string query = q == std::string::npos ? std::string() : r.url.substr(q + 1);
        ShareLogged entry;
        entry.method = r.method;
        entry.route = sub;
        entry.url = r.url;
        entry.query = query;
        entry.content_type = pbc::header_value(r.headers, "Content-Type");
        entry.body = r.body;
        entry.ocs_header = pbc::header_value(r.headers, "OCS-APIRequest");
        entry.accept = pbc::header_value(r.headers, "Accept");
        entry.user_agent = pbc::header_value(r.headers, "User-Agent");
        share_log_.push_back(entry);
        if (entry.ocs_header != "true") return ocs_fail(v2, 401, "auth");
        if (!sharing_enabled_) return ocs_fail(v2, 404, "off");
        const auto params = parse_pairs(query);
        const std::string user = user_id_;
        if (sub == "/shares/inherited" && r.method == "GET") {
            if (inherited_status_ != 0) return ocs_fail(v2, static_cast<int>(inherited_status_), "off");
            const std::string path = params.count("path") ? params.at("path") : std::string();
            std::string data = "[";
            const auto it = inherited_.find(path);
            if (it != inherited_.end())
                for (size_t i = 0; i < it->second.size(); ++i) data += (i ? "," : "") + share_json(it->second[i]);
            return ocs_ok(v2, data + "]");
        }
        if (sub == "/shares" && r.method == "GET") {
            if (!params.count("path")) return ocs_fail(v2, 400, "bad");
            const std::string path = params.at("path");
            if (!tree_.count(path)) return ocs_fail(v2, 404, "path");
            const bool reshares = params.count("reshares") && params.at("reshares") == "true";
            std::string data = "[";
            bool first = true;
            for (const Share& s : shares_) {
                if (s.path != path || (!reshares && s.owner != user)) continue;
                data += (first ? "" : ",") + share_json(s);
                first = false;
            }
            return ocs_ok(v2, data + "]");
        }
        if (sub == "/shares" && r.method == "POST") {
            const auto f = parse_pairs(r.body);
            if (!f.count("shareType") || f.at("shareType") != "3") return ocs_fail(v2, 400, "type");
            if (!public_links_enabled_) return ocs_fail(v2, 403, "public");
            const std::string path = f.count("path") ? f.at("path") : std::string();
            if (!tree_.count(path)) return ocs_fail(v2, 404, "path");
            if (unshareable_.count(path)) return ocs_fail(v2, 403, "item");
            const char* what = "bad";
            if (const int code = check_link_fields(f, true, nullptr, &what)) return ocs_fail(v2, code, what);
            Share s;
            s.type = 3;
            s.path = path;
            s.permissions = f.count("permissions") ? std::atoi(f.at("permissions").c_str()) : 1;
            s.password = f.count("password") ? f.at("password") : std::string();
            s.expiration = f.count("expireDate") ? f.at("expireDate") : std::string();
            const int64_t id = add_share(s);
            return ocs_ok(v2, share_json(*find_share(id)));
        }
        const std::string prefix = "/shares/";
        if (sub.compare(0, prefix.size(), prefix) == 0) {
            const std::string idtext = sub.substr(prefix.size());
            const int64_t id = idtext.empty() || idtext.find_first_not_of("0123456789") != std::string::npos ? 0 : std::atoll(idtext.c_str());
            Share* sh = nullptr;
            for (Share& s : shares_)
                if (s.id == id) sh = &s;
            if (!sh) return ocs_fail(v2, 404, "id");
            if (r.method == "GET") return ocs_ok(v2, "[" + share_json(*sh) + "]");
            if (sh->owner != user) return ocs_fail(v2, 403, "owner");
            if (r.method == "DELETE") {
                remove_share(id);
                return ocs_ok(v2, "[]");
            }
            if (r.method == "PUT") {
                if (sh->type != 3) return ocs_fail(v2, 400, "type");
                const auto f = parse_pairs(r.body);
                const char* what = "bad";
                if (const int code = check_link_fields(f, false, sh, &what)) return ocs_fail(v2, code, what);
                if (f.count("password")) sh->password = f.at("password");
                if (f.count("expireDate")) sh->expiration = f.at("expireDate");
                if (f.count("permissions")) sh->permissions = std::atoi(f.at("permissions").c_str());
                return ocs_ok(v2, share_json(*sh));
            }
        }
        return ocs_fail(v2, 400, "bad");
    }

    std::string sharing_caps_json() const {
        if (!sharing_caps_) return std::string();
        auto b = [](bool v) { return v ? "true" : "false"; };
        std::string pw = std::string("\"password\":{\"enforced\":") + b(pw_enforced_);
        if (pw_for_set_) pw += std::string(",\"enforced_for\":{\"read_only\":") + b(pw_for_ro_) + ",\"read_write\":" + b(pw_for_rw_) + ",\"upload_only\":" + b(pw_for_uo_) + "}";
        pw += "}";
        return std::string(",\"files_sharing\":{\"api_enabled\":") + b(sharing_enabled_) + ",\"public\":{\"enabled\":" + b(public_links_enabled_) + "," + pw +
               ",\"expire_date\":{\"enabled\":" + b(exp_enabled_) + ",\"days\":" + std::to_string(exp_days_) + ",\"enforced\":" + b(exp_enforced_) +
               "},\"upload\":" + b(public_upload_) + ",\"upload_files_drop\":true},\"resharing\":true}";
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
            else if (list_checksums_) x += "<oc:checksums><oc:checksum>SHA256:" + sha256_hex_of(n.bytes(0, n.size())) + "</oc:checksum></oc:checksums>";
        }
        x += "<oc:fileid>" + std::to_string(n.fileid) + "</oc:fileid>";
        x += "<oc:permissions>" + n.permissions + "</oc:permissions>";
        x += "<oc:size>" + std::to_string(n.folder ? folder_size(p) : n.size()) + "</oc:size>";
        x += std::string("<oc:favorite>") + (n.favorite ? "1" : "0") + "</oc:favorite>";
        if (n.encrypted) x += "<nc:is-encrypted>1</nc:is-encrypted>";
        if (with_quota) {
            const int64_t used = quota_limit_ ? folder_size("/") : (quota_ ? quota_->first : folder_size("/"));
            const int64_t avail = quota_limit_ ? std::max<int64_t>(*quota_limit_ - used, 0) : (quota_ ? quota_->second : -3);
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
        if (r.method == "PUT") return put_file(r, p);
        if (r.method == "MKCOL") return mkcol(r, p);
        if (r.method == "MOVE" || r.method == "COPY") return move_copy(r, p);
        if (r.method == "DELETE") return delete_entry(r, p);
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
        std::string failure;
        if (depth == "1") {
            auto f = listing_failures_.find(p);
            if (f != listing_failures_.end()) failure = f->second;
        }
        if (failure == "timeout") {
            Response t;
            t.error = pbc::ErrorClass::Network;
            t.error_key = "timeout";
            t.error_detail = "the fake server did not answer";
            return t;
        }
        if (failure == "error") return reply(500, "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\"/>", "application/xml");
        const bool quota = r.body.find("quota-available-bytes") != std::string::npos && p == "/";
        std::string xml = "<?xml version=\"1.0\"?>\n<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\" xmlns:nc=\"http://nextcloud.org/ns\">";
        xml += response_xml(p, it->second, quota);
        if (depth == "1" && it->second.folder) {
            const std::vector<std::string> kids = children_of(p);
            for (size_t i = 0; i < kids.size(); ++i) {
                if (failure == "partial" && i % 2 == 1) {   // every second member could not be read: a 403 propstat, the others are fine
                    Node broken = tree_.at(kids[i]);
                    broken.listing_quirk = "no_propstat_ok";
                    xml += response_xml(kids[i], broken, false);
                } else {
                    xml += response_xml(kids[i], tree_.at(kids[i]), false);
                }
            }
            auto raw = raw_members_.find(p);
            if (raw != raw_members_.end())
                for (const std::string& name : raw->second) {
                    const std::string href = "/remote.php/dav/files" + pbc::encode_path("/" + user_id_) + (p == "/" ? "" : pbc::encode_path(p)) + "/" + name;
                    xml += "<d:response><d:href>" + href + "</d:href><d:propstat><d:prop><d:getetag>\"hostile\"</d:getetag><d:resourcetype/></d:prop>"
                           "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
                }
        }
        xml += "</d:multistatus>";
        if (failure == "truncated") xml.resize(xml.size() * 6 / 10);   // the answer stops in the middle of an element
        return reply(207, xml, "application/xml; charset=utf-8");
    }

    // =================================================================================================================================
    // The write side (specs/002-manage-server-files, contracts/server-requests.md W3-W9). UNVERIFIED against a real server (LV-W*).
    // =================================================================================================================================

    long long keep_limit_ = 64ll << 20;   // larger uploads are hashed and counted, not kept

    static std::string lower_text(std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    }
    static std::string sabre_body(const std::string& cls) {
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<d:error xmlns:d=\"DAV:\" xmlns:s=\"http://sabredav.org/ns\">\n  <s:exception>Sabre\\DAV\\Exception\\" + cls +
               "</s:exception>\n  <s:message>refused</s:message>\n</d:error>\n";
    }
    Response sabre_error(long status, const std::string& cls) { return reply(status, sabre_body(cls), "application/xml; charset=utf-8"); }
    Response cut_response() {   // the connection broke while the body was going out
        Response r;
        r.error = pbc::ErrorClass::Network;
        r.error_key = "connection_lost";
        r.error_detail = "the connection was cut";
        r.delivery = pbc::Delivery::PartlySent;
        return r;
    }
    Response cancelled_response() {
        Response r;
        r.cancelled = true;
        r.error = pbc::ErrorClass::Cancelled;
        r.error_key = "cancelled";
        r.error_detail = "cancelled by the user";
        r.delivery = pbc::Delivery::PartlySent;
        return r;
    }
    std::string file_id_text(int64_t id) const {
        if (!opaque_ids_) return std::to_string(id);
        std::string n = std::to_string(id);
        return std::string(n.size() < 8 ? 8 - n.size() : 0, '0') + n + "ocabc";
    }
    // 201/204 answer of a write: the new ETag and file id of the node, as the real server sends them in headers
    Response written(long status, const Node& n) {
        Response r = reply(status, "", "application/xml");
        if (answer_etag_headers_) {
            r.headers.push_back({"OC-Etag", n.etag});
            r.headers.push_back({"OC-FileId", file_id_text(n.fileid)});
        }
        r.headers.push_back({"X-OC-MTime", "accepted"});
        return r;
    }

    bool name_forbidden(const std::string& name) const {
        for (char c : name)
            if (forbidden_chars_.find(c) != std::string::npos) return true;
        const std::string low = lower_text(name);
        for (const std::string& n : forbidden_names_)
            if (low == lower_text(n)) return true;
        const std::string base = low.substr(0, low.find('.'));
        for (const std::string& b : forbidden_basenames_)
            if (base == lower_text(b)) return true;
        for (std::string e : forbidden_extensions_) {
            e = lower_text(e);
            if (!e.empty() && e[0] != '.') e = "." + e;
            if (low.size() > e.size() && low.compare(low.size() - e.size(), e.size(), e) == 0) return true;
        }
        return false;
    }
    bool blocked_type(const std::string& name) const {
        const std::string low = lower_text(name);
        for (std::string e : blocked_ext_) {
            e = lower_text(e);
            if (low.size() > e.size() && low.compare(low.size() - e.size(), e.size(), e) == 0) return true;
        }
        return false;
    }
    static bool if_match_ok(const std::string& header, const Node* n) {
        if (!n) return false;
        if (header == "*") return true;
        if (header.compare(0, 2, "W/") == 0) return false;   // a weak validator never matches If-Match
        return !n->weak_etag && header == n->etag;
    }
    int64_t free_bytes() const { return quota_limit_ ? std::max<int64_t>(*quota_limit_ - folder_size("/"), 0) : INT64_MAX; }

    std::vector<std::string> subtree(const std::string& p) const {
        std::vector<std::string> out;
        for (const auto& kv : tree_)
            if (kv.first == p || kv.first.compare(0, p.size() + 1, p + "/") == 0) out.push_back(kv.first);
        return out;
    }
    std::vector<std::string> locked_members(const std::string& p) const {
        std::vector<std::string> out;
        for (const std::string& sp : subtree(p))
            if (tree_.at(sp).locked) out.push_back(sp);
        return out;
    }
    Response partial_207(const std::vector<std::string>& failed) {
        std::string xml = multistatus_open();
        for (const std::string& f : failed)
            xml += "<d:response><d:href>/remote.php/dav/files" + pbc::encode_path("/" + user_id_) + pbc::encode_path(f) + "</d:href><d:status>HTTP/1.1 423 Locked</d:status></d:response>";
        return reply(207, xml + "</d:multistatus>", "application/xml; charset=utf-8");
    }
    bool destination_path(const std::string& destination, std::string* out) const {
        const std::string prefix = base_url() + "/remote.php/dav/files/";
        if (destination.compare(0, prefix.size(), prefix) != 0) return false;
        const auto decoded = pbc::decode_href(destination.substr(base_url().size()), user_id_);
        if (!decoded.ok()) return false;
        *out = decoded.value;
        return true;
    }

    struct Collector {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
        std::string bytes;
        bool keep = true;
        long long size = 0;
        explicit Collector(bool keep_bytes) : keep(keep_bytes) { EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr); }
        void add(const char* p, size_t n) {
            EVP_DigestUpdate(ctx.get(), p, n);
            if (keep) bytes.append(p, n);
            size += static_cast<long long>(n);
        }
        std::string hex() {
            unsigned char md[EVP_MAX_MD_SIZE];
            unsigned int len = 0;
            EVP_DigestFinal_ex(ctx.get(), md, &len);
            return hex_of(md, len);
        }
    };
    static std::string hex_of(const unsigned char* md, unsigned int len) {
        static const char* kHex = "0123456789abcdef";
        std::string out;
        for (unsigned int i = 0; i < len; ++i) {
            out.push_back(kHex[md[i] >> 4]);
            out.push_back(kHex[md[i] & 15]);
        }
        return out;
    }

    enum class BodyResult { Done, Cancelled, Dropped };
    // Reads the request body (in memory or a range of a file) in pieces of 1 MiB, honoring the progress callback (cancel) and drop_after_bytes.
    BodyResult read_body(const pbc::Request& r, const std::function<void(const char*, size_t)>& sink, long long* got, long long length) {
        long long limit = length;
        bool drop = false;
        if (drop_after_ >= 0 && drop_after_ < length) {
            limit = drop_after_;
            drop_after_ = -1;
            drop = true;
        }
        long long done = 0;
        if (r.body_file.empty()) {
            while (done < limit) {
                const size_t n = static_cast<size_t>(std::min<long long>(1 << 20, limit - done));
                sink(r.body.data() + done, n);
                done += static_cast<long long>(n);
                bytes_uploaded_ += static_cast<long long>(n);
                if (r.progress && !r.progress(done, length)) {
                    *got = done;
                    return BodyResult::Cancelled;
                }
            }
        } else {
            const int fd = ::open(r.body_file.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                *got = 0;
                return BodyResult::Dropped;
            }
            std::vector<char> buf(1 << 20);
            while (done < limit) {
                const ssize_t n = ::pread(fd, buf.data(), static_cast<size_t>(std::min<long long>(static_cast<long long>(buf.size()), limit - done)),
                                          static_cast<off_t>(r.body_offset + done));
                if (n <= 0) break;
                sink(buf.data(), static_cast<size_t>(n));
                done += n;
                bytes_uploaded_ += n;
                if (r.progress && !r.progress(done, length)) {
                    ::close(fd);
                    *got = done;
                    return BodyResult::Cancelled;
                }
            }
            ::close(fd);
        }
        *got = done;
        return drop ? BodyResult::Dropped : BodyResult::Done;
    }
    static long long body_length_of(const pbc::Request& r) { return r.body_file.empty() ? static_cast<long long>(r.body.size()) : r.body_length; }

    Response put_file(const pbc::Request& r, const std::string& p) {
        if (p == "/") return reply(405, "", "application/xml");
        const std::string name = p.substr(p.rfind('/') + 1), parent = pbc::parent_of(p);
        if (parent != "/" && !tree_.count(parent)) return reply(409, "", "application/xml");
        auto it = tree_.find(p);
        const bool exists = it != tree_.end();
        if (exists && it->second.folder) return reply(405, "", "application/xml");
        if (exists && it->second.locked) return sabre_error(423, "FileLocked");
        if (name_forbidden(name)) return sabre_error(400, "InvalidPath");
        if (blocked_type(name)) return sabre_error(415, "UnsupportedMediaType");
        const std::string if_match = pbc::header_value(r.headers, "If-Match");
        const std::string if_none_match = pbc::header_value(r.headers, "If-None-Match");
        if (!if_none_match.empty() && exists && if_none_match == "*") return reply(412, "", "application/xml");
        if (!if_match.empty() && !if_match_ok(if_match, exists ? &it->second : nullptr)) return reply(412, "", "application/xml");
        const long long length = body_length_of(r);
        if (max_file_size_ && length > *max_file_size_) return sabre_error(413, "EntityTooLarge");
        if (length - (exists ? it->second.size() : 0) > free_bytes()) return sabre_error(507, "InsufficientStorage");
        Collector c(length <= keep_limit_);
        long long got = 0;
        const BodyResult res = read_body(r, [&](const char* d, size_t n) { c.add(d, n); }, &got, length);
        if (res == BodyResult::Cancelled) return cancelled_response();
        if (res == BodyResult::Dropped) return cut_response();
        Node n;
        n.mime = "application/octet-stream";
        n.content = c.bytes;
        if (!c.keep) n.synthetic_size = c.size;
        n.sha256 = c.hex();
        const std::string mtime = pbc::header_value(r.headers, "X-OC-MTime");
        n.mtime = mtime.empty() ? server_time_ : std::atoll(mtime.c_str());
        n.checksum = pbc::header_value(r.headers, "OC-Checksum");
        if (exists) {
            n.fileid = it->second.fileid;
            n.favorite = it->second.favorite;
            n.etag = next_etag();
            tree_[p] = n;
            touch(parent);
        } else {
            put(p, n);
        }
        return written(exists ? 204 : 201, tree_.at(p));
    }

    Response mkcol(const pbc::Request&, const std::string& p) {
        if (p == "/" || tree_.count(p)) return reply(405, "", "application/xml");
        const std::string parent = pbc::parent_of(p);
        if (parent != "/" && !tree_.count(parent)) return reply(409, "", "application/xml");
        if (name_forbidden(p.substr(p.rfind('/') + 1))) return sabre_error(400, "InvalidPath");
        put(p, folder_node());
        return written(201, tree_.at(p));
    }

    Response move_copy(const pbc::Request& r, const std::string& src) {
        const bool is_move = r.method == "MOVE";
        auto s = tree_.find(src);
        if (src == "/" || s == tree_.end()) return reply(404, "", "application/xml");
        std::string dst;
        if (!destination_path(pbc::header_value(r.headers, "Destination"), &dst)) return reply(400, "", "application/xml");
        const bool conditions = !ignore_move_conditions_;
        const std::string if_match = pbc::header_value(r.headers, "If-Match");
        if (conditions && !if_match.empty() && !if_match_ok(if_match, &s->second)) return reply(412, "", "application/xml");
        const bool overwrite = pbc::header_value(r.headers, "Overwrite") != "F";
        const std::string dparent = pbc::parent_of(dst);
        if (dparent != "/" && !tree_.count(dparent)) return reply(409, "", "application/xml");
        if (dst == src || dst == "/" || (s->second.folder && dst.compare(0, src.size() + 1, src + "/") == 0)) return sabre_error(403, "Forbidden");
        if (name_forbidden(dst.substr(dst.rfind('/') + 1))) return sabre_error(400, "InvalidPath");
        const bool dst_exists = tree_.count(dst) == 1;
        if (conditions && dst_exists && !overwrite) return reply(412, "", "application/xml");
        std::vector<std::string> locked = locked_members(src);
        if (dst_exists)
            for (const std::string& l : locked_members(dst)) locked.push_back(l);
        if (!locked.empty()) {
            if (locked.size() == 1 && locked[0] == src && !s->second.folder) return sabre_error(423, "FileLocked");
            return partial_207(locked);
        }
        if (!is_move && folder_size(src) + (s->second.folder ? 0 : s->second.size()) > free_bytes()) return sabre_error(507, "InsufficientStorage");
        if (dst_exists) {
            for (const std::string& sp : subtree(dst)) tree_.erase(sp);
            ++overwrites_;
        }
        std::map<std::string, Node> placed;
        for (const std::string& sp : subtree(src)) {
            Node n = tree_.at(sp);
            n.etag = next_etag();
            if (!is_move) n.fileid = ++next_id_;
            placed[dst + sp.substr(src.size())] = n;
        }
        if (is_move)
            for (const std::string& sp : subtree(src)) tree_.erase(sp);
        for (auto& kv : placed) tree_[kv.first] = kv.second;
        touch(pbc::parent_of(src));
        touch(dparent);
        return written(dst_exists ? 204 : 201, tree_.at(dst));
    }

    Response delete_entry(const pbc::Request& r, const std::string& p) {
        auto it = tree_.find(p);
        if (p == "/") return sabre_error(403, "Forbidden");
        if (it == tree_.end()) return reply(404, "", "application/xml");
        const std::string if_match = pbc::header_value(r.headers, "If-Match");
        if (!if_match.empty() && !if_match_ok(if_match, &it->second)) return reply(412, "", "application/xml");
        const auto locked = locked_members(p);
        if (!locked.empty()) {
            if (locked.size() == 1 && locked[0] == p && !it->second.folder) return sabre_error(423, "FileLocked");
            return partial_207(locked);
        }
        for (const std::string& sp : subtree(p)) {
            const Node& n = tree_.at(sp);
            if (trash_enabled_) trash_.push_back({sp, n.folder, n.size()});
            else ++permanent_deletes_;
        }
        for (const std::string& sp : subtree(p)) tree_.erase(sp);
        touch(pbc::parent_of(p));
        return reply(204, "", "application/xml");
    }

    // ---- /remote.php/dav/uploads/<uid>/: chunked upload v2 ----
    static bool five_digits(const std::string& s) {
        if (s.size() != 5) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        const int n = std::atoi(s.c_str());
        return n >= 1 && n <= 10000;
    }
    Response uploads_area(const pbc::Request& r, const std::string& route) {
        if (maintenance_) return reply(503, "", "application/xml");
        if (!authorized(r)) return reply(401, "", "application/xml");
        const std::string prefix = "/remote.php/dav/uploads/" + user_id_ + "/";
        if (route.compare(0, prefix.size(), prefix) != 0) return reply(404, "", "application/xml");
        std::string rest = route.substr(prefix.size());
        while (!rest.empty() && rest.back() == '/') rest.pop_back();
        if (rest.empty()) return reply(405, "", "application/xml");
        const size_t slash = rest.find('/');
        const std::string id = rest.substr(0, slash);
        const std::string name = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
        const std::string destination_header = pbc::header_value(r.headers, "Destination");
        if (name.empty()) {
            if (r.method == "MKCOL") {
                std::string dst;
                if (!destination_path(destination_header, &dst)) return reply(400, "", "application/xml");
                if (sessions_.count(id)) return reply(405, "", "application/xml");
                Session& se = sessions_[id];
                se.destination = dst;
                const std::string total = pbc::header_value(r.headers, "OC-Total-Length");
                se.total_length = total.empty() ? -1 : std::atoll(total.c_str());
                return reply(201, "", "application/xml");
            }
            if (r.method == "DELETE") {
                if (!sessions_.erase(id)) return reply(404, "", "application/xml");
                return reply(204, "", "application/xml");
            }
            if (r.method == "PROPFIND") {
                const std::string depth = pbc::header_value(r.headers, "Depth");
                if (depth != "0" && depth != "1") return reply(403, "", "application/xml");
                auto it = sessions_.find(id);
                if (it == sessions_.end()) return reply(404, "", "application/xml");
                std::string xml = multistatus_open();
                const std::string href = prefix + id + "/";
                xml += "<d:response><d:href>" + href + "</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
                if (depth == "1")
                    for (const auto& kv : it->second.chunks)
                        xml += "<d:response><d:href>" + href + kv.first + "</d:href><d:propstat><d:prop><d:getcontentlength>" + std::to_string(kv.second.size) +
                               "</d:getcontentlength><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
                return reply(207, xml + "</d:multistatus>", "application/xml; charset=utf-8");
            }
            return reply(405, "", "application/xml");
        }
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return reply(404, "", "application/xml");
        Session& se = it->second;
        if (r.method == "PUT") {
            if (!five_digits(name)) return reply(400, "", "application/xml");
            std::string dst;
            if (!destination_path(destination_header, &dst) || dst != se.destination) return reply(400, "", "application/xml");
            const std::string total_header = pbc::header_value(r.headers, "OC-Total-Length");
            if (!total_header.empty() && std::atoll(total_header.c_str()) > free_bytes()) return sabre_error(507, "InsufficientStorage");
            const long long length = body_length_of(r);
            Chunk c;
            long long got = 0;
            const bool keep = length <= keep_limit_;
            const BodyResult res = read_body(r, [&](const char* d, size_t n) {
                if (keep) c.bytes.append(d, n);
                if (se.running_valid && std::atoi(name.c_str()) == se.next_in_order) EVP_DigestUpdate(se.running.get(), d, n);
            }, &got, length);
            if (res != BodyResult::Done) {
                if (partial_visible_ && res == BodyResult::Dropped) {
                    c.size = got;
                    se.chunks[name] = c;
                }
                return res == BodyResult::Cancelled ? cancelled_response() : cut_response();
            }
            c.size = got;
            if (std::atoi(name.c_str()) == se.next_in_order) ++se.next_in_order;
            else se.running_valid = false;
            se.chunks[name] = c;
            return reply(201, "", "application/xml");
        }
        if (r.method == "MOVE" && name == ".file") return assemble(r, id, se);
        return reply(405, "", "application/xml");
    }

    Response assemble(const pbc::Request& r, const std::string& id, Session& se) {
        std::string dst;
        if (!destination_path(pbc::header_value(r.headers, "Destination"), &dst) || dst != se.destination) return reply(400, "", "application/xml");
        const std::string parent = pbc::parent_of(dst);
        if (parent != "/" && !tree_.count(parent)) return reply(409, "", "application/xml");
        const std::string name = dst.substr(dst.rfind('/') + 1);
        if (name_forbidden(name)) return sabre_error(400, "InvalidPath");
        if (blocked_type(name)) return sabre_error(415, "UnsupportedMediaType");
        auto existing = tree_.find(dst);
        const bool exists = existing != tree_.end();
        if (exists && existing->second.folder) return reply(405, "", "application/xml");
        if (exists && existing->second.locked) return sabre_error(423, "FileLocked");
        long long total = 0;
        for (const auto& kv : se.chunks) total += kv.second.size;
        const std::string want = pbc::header_value(r.headers, "OC-Total-Length");
        if (!want.empty() && std::atoll(want.c_str()) != total) return reply(400, "", "application/xml");
        if (max_file_size_ && total > *max_file_size_) return sabre_error(413, "EntityTooLarge");
        if (total - (exists ? existing->second.size() : 0) > free_bytes()) return sabre_error(507, "InsufficientStorage");
        if (!ignore_assemble_conditions_) {
            const std::string overwrite = pbc::header_value(r.headers, "Overwrite");
            const std::string if_match = pbc::header_value(r.headers, "If-Match");
            if (exists && overwrite == "F") return reply(412, "", "application/xml");
            if (!if_match.empty() && !if_match_ok(if_match, exists ? &existing->second : nullptr)) return reply(412, "", "application/xml");
        }
        Node n;
        n.mime = "application/octet-stream";
        const bool keep = total <= keep_limit_;
        if (keep) {
            for (const auto& kv : se.chunks) n.content += kv.second.bytes;
            Collector c(false);
            c.add(n.content.data(), n.content.size());
            n.sha256 = c.hex();
        } else {
            n.synthetic_size = total;
            if (se.running_valid && static_cast<int>(se.chunks.size()) + 1 == se.next_in_order) {
                unsigned char md[EVP_MAX_MD_SIZE];
                unsigned int len = 0;
                EVP_DigestFinal_ex(se.running.get(), md, &len);
                n.sha256 = hex_of(md, len);
            }
        }
        const std::string mtime = pbc::header_value(r.headers, "X-OC-Mtime").empty() ? pbc::header_value(r.headers, "X-OC-MTime") : pbc::header_value(r.headers, "X-OC-Mtime");
        n.mtime = mtime.empty() ? server_time_ : std::atoll(mtime.c_str());
        n.checksum = pbc::header_value(r.headers, "OC-Checksum");
        if (exists) {
            n.fileid = existing->second.fileid;
            n.favorite = existing->second.favorite;
            n.etag = next_etag();
            tree_[dst] = n;
            touch(parent);
        } else {
            put(dst, n);
        }
        sessions_.erase(id);
        return written(exists ? 204 : 201, tree_.at(dst));
    }

    std::string dav_and_files_caps() const {
        auto array = [](const std::vector<std::string>& v) {
            std::string out = "[";
            for (size_t i = 0; i < v.size(); ++i) out += (i ? ",\"" : "\"") + quote_json(v[i]) + "\"";
            return out + "]";
        };
        std::string files;
        auto add = [&](const std::string& piece) {
            if (!files.empty()) files += ",";
            files += piece;
        };
        if (trash_cap_) add(std::string("\"undelete\":") + (*trash_cap_ ? "true" : "false"));
        if (max_chunk_cap_) add("\"chunked_upload\":{\"max_size\":" + std::to_string(*max_chunk_cap_) + "}");
        if (forbidden_in_caps_) {
            if (!forbidden_chars_.empty()) {
                std::vector<std::string> chars;
                for (char c : forbidden_chars_) chars.push_back(std::string(1, c));
                add("\"forbidden_filename_characters\":" + array(chars));
            }
            if (!forbidden_names_.empty()) add("\"forbidden_filenames\":" + array(forbidden_names_));
            if (!forbidden_basenames_.empty()) add("\"forbidden_filename_basenames\":" + array(forbidden_basenames_));
            if (!forbidden_extensions_.empty()) add("\"forbidden_filename_extensions\":" + array(forbidden_extensions_));
        }
        return std::string("\"dav\":{") + (chunking_cap_ ? "\"chunking\":\"1.0\"" : "") + "},\"files\":{" + files + "}";
    }

    static std::string etag_header(const Node& n) { return n.weak_etag ? "W/" + n.etag : n.etag; }

    Response get(const pbc::Request& req, const std::string& p) {
        auto it = tree_.find(p);
        if (it == tree_.end()) return reply(404, "", "application/xml");
        if (it->second.folder) return reply(405, "", "application/xml");
        const Node& n = it->second;
        // RFC 9110: a GET that names the version it wants (If-Match) is refused with 412 when the file is not that version any more
        {
            const std::string want = pbc::header_value(req.headers, "If-Match");
            if (!ignore_get_if_match_ && !want.empty() && !if_match_ok(want, &n)) return reply(412, sabre_body("PreconditionFailed"), "application/xml; charset=utf-8");
        }
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
        bytes_downloaded_ += n.size();
        Response r = reply(200, n.bytes(0, n.size()), n.mime);
        r.headers.push_back({"ETag", etag_header(n)});
        r.headers.push_back({"Last-Modified", http_date(n.mtime)});
        r.headers.push_back({"Accept-Ranges", "bytes"});
        return r;
    }

    static std::string sha256_hex_of(const std::string& data) {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_Digest(data.data(), data.size(), md, &len, EVP_sha256(), nullptr);
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (unsigned int i = 0; i < len; ++i) {
            out.push_back(hex[md[i] >> 4]);
            out.push_back(hex[md[i] & 15]);
        }
        return out;
    }

    bool list_checksums_ = false, ignore_get_if_match_ = false;
    std::map<std::string, std::string> listing_failures_;
    std::map<std::string, std::vector<std::string>> raw_members_;
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
    // write side state
    std::optional<bool> trash_cap_ = true;
    bool trash_enabled_ = true, chunking_cap_ = true, forbidden_in_caps_ = true, opaque_ids_ = false, answer_etag_headers_ = true, partial_visible_ = false;
    std::optional<int64_t> max_chunk_cap_, quota_limit_, max_file_size_;
    std::string forbidden_chars_;
    std::vector<std::string> forbidden_names_, forbidden_basenames_, forbidden_extensions_, blocked_ext_;
    std::vector<Trashed> trash_;
    int permanent_deletes_ = 0, overwrites_ = 0;
    long long bytes_uploaded_ = 0, bytes_downloaded_ = 0;
    std::map<std::string, Session> sessions_;
    std::string lose_method_, lose_part_;
    bool lose_armed_ = false;
    long long drop_after_ = -1;
    std::string answer_method_, answer_part_, answer_retry_;
    long answer_status_ = 0;
    int answer_count_ = 0;
    bool ignore_assemble_conditions_ = false, ignore_move_conditions_ = false;
    std::string before_method_, before_part_;
    std::function<void()> before_fn_;
    int before_skip_ = 0;
    // share links
    std::vector<Share> shares_;
    std::map<std::string, std::vector<Share>> inherited_;
    std::vector<ShareLogged> share_log_;
    int64_t next_share_id_ = 1000;
    long inherited_status_ = 0;
    bool sharing_enabled_ = true, public_links_enabled_ = true, pw_enforced_ = false, pw_for_set_ = false, pw_for_ro_ = false, pw_for_rw_ = false,
         pw_for_uo_ = false, exp_enabled_ = false, exp_enforced_ = false, public_upload_ = true, sharing_caps_ = true;
    int exp_days_ = 0, share_wording_ = 0;
    std::set<std::string> unshareable_;
    std::string drop_method_, drop_part_;
    bool drop_armed_ = false;
    size_t link_hits_ = 0;
    FakeTransport inner_;
};

}  // namespace pbtest
