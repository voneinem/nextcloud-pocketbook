#include "pbc/dav_write.h"

#include <cctype>
#include <cstdlib>
#include <set>

#include "pbc/error_keys.h"
#include "pbc/remote_path.h"
#include "pbc/version.h"
#include "pbc/xml_min.h"

namespace pbc {

namespace {

constexpr size_t kMaxErrorBody = 64u << 10;

bool plain_digits(const std::string& s) {
    if (s.empty() || s.size() > 18) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

Error keyed(const char* key, std::string detail = std::string()) {
    Error e = error_from_key(key);
    e.detail = std::move(detail);
    return e;
}

}  // namespace

std::string sabre_exception_class(const std::string& body) {
    if (body.empty() || body.size() > kMaxErrorBody) return std::string();
    XmlLimits limits;
    limits.max_bytes = kMaxErrorBody;
    limits.max_depth = 16;
    limits.max_nodes = 200;
    const Result<XmlNode> doc = parse_xml(body, limits);
    if (!doc.ok() || doc.value.name != "error") return std::string();
    const XmlNode* ex = doc.value.child("http://sabredav.org/ns", "exception");
    if (!ex) return std::string();
    std::string text = ex->text;
    const size_t b = text.find_first_not_of(" \t\r\n");
    const size_t e = text.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    text = text.substr(b, e - b + 1);
    if (text.size() > 200) return std::string();
    for (char c : text)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\\')) return std::string();
    std::string last = text.substr(text.rfind('\\') == std::string::npos ? 0 : text.rfind('\\') + 1);
    static const std::string kSuffix = "Exception";
    if (last.size() > kSuffix.size() && last.compare(last.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) last.resize(last.size() - kSuffix.size());
    if (last == "Locked") last = "FileLocked";
    static const std::set<std::string> kKnown = {"InvalidPath", "ForbiddenFileName", "FileLocked", "NotFound", "EntityTooLarge",
                                                 "UnsupportedMediaType", "InsufficientStorage", "Forbidden"};
    return kKnown.count(last) ? last : std::string();
}

WriteAnswer interpret_write_answer(const Response& r, WriteKind kind, bool replacing) {
    WriteAnswer a;
    a.status = r.status;
    a.delivery = r.delivery;

    if (r.status == 0) {   // no answer: what matters is how far the request got
        if (r.cancelled || r.error_key == "cancelled") {
            a.error = keyed("cancelled");
        } else if (r.error == ErrorClass::Tls || r.error_key == "source_unreadable" || r.error_key == "redirect_other_host" ||
                   r.error_key == "scheme_not_allowed") {
            a.error = error_from_key(r.error_key.empty() ? "no_connection" : r.error_key);
        } else if (r.delivery == Delivery::NotSent) {
            a.error = keyed("no_connection");
        } else if (kind == WriteKind::Chunk) {
            a.error = keyed("connection_lost");
        } else {
            a.error = keyed("outcome_unknown");
            a.outcome_unknown = true;
        }
        return a;
    }

    a.etag = header_value(r.headers, "OC-Etag");
    if (a.etag.empty()) a.etag = header_value(r.headers, "ETag");
    const std::string fid = header_value(r.headers, "OC-FileId");
    if (plain_digits(fid)) a.fileid = std::atoll(fid.c_str());

    const long s = r.status;
    if (s == 207) {
        if (kind == WriteKind::Delete || kind == WriteKind::Move || kind == WriteKind::Copy) a.error = keyed("partial_failure");
        else a.error = keyed("protocol_error", "unexpected multistatus answer");
        return a;
    }
    if (s >= 200 && s < 300) return a;   // 200, 201, 204: the server confirmed

    if (s >= 400) a.exception_class = sabre_exception_class(r.body);
    const std::string& cls = a.exception_class;

    // the exception class refines the status (a refusal the server explains)
    if (cls == "InsufficientStorage") { a.error = keyed("server_storage_full"); return a; }
    if (cls == "EntityTooLarge") { a.error = keyed("file_too_large_server"); return a; }
    if (cls == "UnsupportedMediaType") { a.error = keyed("type_not_accepted"); return a; }
    if (cls == "FileLocked" && s != 401 && s != 429 && s != 503) { a.error = keyed("locked"); return a; }
    if ((cls == "InvalidPath" || cls == "ForbiddenFileName") && (s == 400 || s == 403)) { a.error = keyed("name_forbidden"); return a; }

    switch (s) {
        case 401: a.error = keyed("auth_failed"); return a;
        case 403: a.error = keyed("not_allowed"); return a;
        case 404:
            if (kind == WriteKind::Delete) return a;   // already gone: what was asked for has happened
            if (kind == WriteKind::Move || kind == WriteKind::Copy || (kind == WriteKind::Put && replacing)) a.error = keyed("source_gone");
            else a.error = keyed("target_gone");
            return a;
        case 405:
            a.error = kind == WriteKind::Mkcol ? keyed("name_exists") : keyed("server_refused", "status 405");
            return a;
        case 409: a.error = keyed("target_gone"); return a;
        case 412:
            if (kind == WriteKind::Put) a.error = keyed(replacing ? "changed_on_server" : "name_exists");
            else if (kind == WriteKind::Move || kind == WriteKind::Copy) {
                a.error = keyed("changed_on_server");
                a.ambiguous_412 = true;
            } else if (kind == WriteKind::Delete) a.error = keyed("changed_on_server");
            else a.error = keyed("server_refused", "status 412");
            return a;
        case 413: a.error = keyed("file_too_large_server"); return a;
        case 415: a.error = keyed("type_not_accepted"); return a;
        case 423: a.error = keyed("locked"); return a;
        case 429: a.error = keyed("rate_limited"); return a;
        case 503: a.error = keyed("maintenance"); return a;
        case 507: a.error = keyed("server_storage_full"); return a;
        case 400: a.error = keyed("protocol_error", "the server could not use the request"); return a;
        case 500:
        case 502:
        case 504:
            if (kind == WriteKind::Chunk) {
                a.error = keyed("server_error");
            } else {   // a proxy may have forwarded the request before it failed
                a.error = keyed("outcome_unknown");
                a.outcome_unknown = true;
            }
            return a;
        default: break;
    }
    if (s >= 500 || s == 408) a.error = keyed("server_error");
    else a.error = keyed("server_refused", "status " + std::to_string(s));
    return a;
}

Error classify_write_answer(const Response& r, WriteKind kind, bool replacing) { return interpret_write_answer(r, kind, replacing).error; }

Cause412 disambiguate_412(const std::optional<RemoteEntry>& target, const std::optional<RemoteEntry>& source, const std::string& expected_source_etag) {
    if (target) return Cause412::NameExists;
    if (!source) return Cause412::SourceGone;
    (void)expected_source_etag;   // an unchanged source with no target: neither cause is visible any more, so the item is held for the user
    return Cause412::ChangedOnServer;
}

std::string files_url(const DavSession& session, std::string_view user_id, std::string_view remote_path, bool folder) {
    const std::string path = normalize_remote_path(remote_path);
    std::string url = session.base_url() + "/remote.php/dav/files" + encode_path("/" + std::string(user_id));
    if (path == "/") return url + "/";
    return url + encode_path(path) + (folder ? "/" : "");
}

WriteAnswer put_small(DavSession& session, std::string_view user_id, const QueueItem& item, const std::string& checksum, int64_t mtime, ProgressFn progress) {
    Headers h;
    h.push_back({"User-Agent", std::string("PBCloud/") + version()});
    h.push_back({"Content-Type", "application/octet-stream"});
    if (mtime > 0) h.push_back({"X-OC-MTime", std::to_string(mtime)});
    if (!checksum.empty()) h.push_back({"OC-Checksum", "SHA256:" + checksum});
    if (item.replace && !item.dst_etag.empty()) h.push_back({"If-Match", item.dst_etag});
    else h.push_back({"If-None-Match", "*"});
    const std::string url = files_url(session, user_id, join_remote(item.dst_parent, item.dst_name));
    const Response r = session.send_file("PUT", url, item.src_local, 0, item.src_size.value_or(0), h, std::move(progress));
    return interpret_write_answer(r, WriteKind::Put, item.replace);
}

WriteAnswer delete_entry(DavSession& session, std::string_view user_id, std::string_view remote_path, const std::string& etag, bool folder) {
    Headers h;
    h.push_back({"User-Agent", std::string("PBCloud/") + version()});
    h.push_back({"If-Match", etag});
    const Response r = session.send("DELETE", files_url(session, user_id, remote_path, folder), std::string(), h);
    return interpret_write_answer(r, WriteKind::Delete, false);
}

namespace {

WriteAnswer move_or_copy(DavSession& session, std::string_view user_id, const QueueItem& item, const char* method, WriteKind kind) {
    const bool folder = item.entry_kind == Kind::Folder;
    Headers h;
    h.push_back({"User-Agent", std::string("PBCloud/") + version()});
    h.push_back({"Destination", files_url(session, user_id, join_remote(item.dst_parent, item.dst_name), folder)});
    h.push_back({"Overwrite", "F"});
    h.push_back({"If-Match", item.src_etag});
    const Response r = session.send(method, files_url(session, user_id, item.src_remote, folder), std::string(), h);
    return interpret_write_answer(r, kind, false);
}

}  // namespace

WriteAnswer move_entry(DavSession& session, std::string_view user_id, const QueueItem& item) { return move_or_copy(session, user_id, item, "MOVE", WriteKind::Move); }
WriteAnswer copy_entry(DavSession& session, std::string_view user_id, const QueueItem& item) { return move_or_copy(session, user_id, item, "COPY", WriteKind::Copy); }

WriteAnswer mkcol(DavSession& session, std::string_view user_id, std::string_view remote_path) {
    Headers h;
    h.push_back({"User-Agent", std::string("PBCloud/") + version()});
    const Response r = session.send("MKCOL", files_url(session, user_id, remote_path, true), std::string(), h);
    return interpret_write_answer(r, WriteKind::Mkcol, false);
}

}  // namespace pbc
