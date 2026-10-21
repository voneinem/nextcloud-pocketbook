#include "pbc/upload_session.h"

#include <cstdio>
#include <cstdlib>
#include <map>

#include "pbc/remote_path.h"
#include "pbc/version.h"

namespace pbc {

std::optional<ChunkPlan> plan_chunks(int64_t total, int64_t chunk_size) {
    if (total <= 0 || chunk_size <= 0) return std::nullopt;
    const int64_t count = (total + chunk_size - 1) / chunk_size;
    if (count > kMaxChunks) return std::nullopt;
    ChunkPlan p;
    p.total = total;
    p.chunk_size = chunk_size;
    p.count = static_cast<int>(count);
    return p;
}

std::string chunk_name(int n) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%05d", n);
    return buf;
}

namespace {

bool is_chunk_name(const std::string& name, int count, int* n) {
    if (name.size() != 5) return false;
    for (char c : name)
        if (c < '0' || c > '9') return false;
    const int v = std::atoi(name.c_str());
    if (v < 1 || v > count) return false;
    *n = v;
    return true;
}

std::optional<int64_t> length_of(const XmlNode& response) {
    const XmlNode* p = dav::prop_in_ok_propstat(response, dav::kNsDav, "getcontentlength");
    if (!p) return std::nullopt;
    const std::string t = dav::trim(p->text);
    if (t.empty() || t.size() > 18) return std::nullopt;
    for (char c : t)
        if (c < '0' || c > '9') return std::nullopt;
    return std::atoll(t.c_str());
}

std::string without_trailing_slashes(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

Result<UploadFolder> parse_upload_folder(const Response& r, const ChunkPlan& plan) {
    Result<UploadFolder> out;
    if (r.status == 404) {
        out.value.gone = true;
        return out;
    }
    if (r.body_too_large) {
        out.error = make_error(ErrorClass::Protocol, "listing_too_large", "the answer is larger than the limit");
        return out;
    }
    if (r.status != 207) {
        out.error = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
        return out;
    }
    const auto ms = dav::parse_multistatus(r);
    if (!ms.ok()) {
        out.error = ms.error;
        return out;
    }
    const auto responses = ms.value.children_named(dav::kNsDav, "response");
    // the folder itself is the response that is a collection; what counts as a chunk is exactly one segment below it
    std::string folder;
    for (const XmlNode* resp : responses) {
        const XmlNode* type = dav::prop_in_ok_propstat(*resp, dav::kNsDav, "resourcetype");
        const XmlNode* href = resp->child(dav::kNsDav, "href");
        if (type && type->child(dav::kNsDav, "collection") && href) {
            folder = without_trailing_slashes(dav::trim(href->text));
            break;
        }
    }
    if (folder.empty() && !responses.empty()) {   // an answer without the folder's own entry: the parent of the first one
        const XmlNode* href = responses[0]->child(dav::kNsDav, "href");
        if (href) {
            const std::string h = without_trailing_slashes(dav::trim(href->text));
            folder = h.substr(0, h.rfind('/') == std::string::npos ? 0 : h.rfind('/'));
        }
    }
    std::map<int, std::vector<std::optional<int64_t>>> seen;
    for (const XmlNode* resp : responses) {
        const XmlNode* href = resp->child(dav::kNsDav, "href");
        if (!href) continue;
        const std::string h = without_trailing_slashes(dav::trim(href->text));
        if (folder.empty() || h.size() <= folder.size() + 1 || h.compare(0, folder.size() + 1, folder + "/") != 0) continue;
        const std::string name = h.substr(folder.size() + 1);
        int n = 0;
        if (name.find('/') != std::string::npos || !is_chunk_name(name, plan.count, &n)) continue;
        seen[n].push_back(length_of(*resp));
    }
    for (const auto& kv : seen) {
        // a chunk is good when it appears once with the expected size; one that appears twice is not trusted (the next send writes it again)
        const bool good = kv.second.size() == 1 && kv.second[0] && *kv.second[0] == plan.size_of(kv.first);
        if (good) out.value.good.insert(kv.first);
        else ++out.value.bad;
    }
    return out;
}

UploadSession::UploadSession(DavSession& session, std::string user_id, std::string upload_id, std::string destination, ChunkPlan plan, std::string checksum,
                             int64_t mtime)
    : session_(session), user_id_(std::move(user_id)), id_(std::move(upload_id)), destination_(std::move(destination)), checksum_(std::move(checksum)),
      plan_(plan), mtime_(mtime) {}

std::string UploadSession::folder_url() const { return session_.base_url() + "/remote.php/dav/uploads" + encode_path("/" + user_id_) + "/" + id_; }

namespace {
Headers base_headers() { return {{"User-Agent", std::string("PBCloud/") + version()}}; }
}  // namespace

WriteAnswer UploadSession::open() {
    Headers h = base_headers();
    h.push_back({"Destination", files_url(session_, user_id_, destination_)});
    h.push_back({"OC-Total-Length", std::to_string(plan_.total)});
    return interpret_write_answer(session_.send("MKCOL", folder_url(), std::string(), h), WriteKind::Chunk, false);
}

Result<UploadFolder> UploadSession::list() {
    Headers h = dav::dav_headers("1");
    h.push_back({"User-Agent", std::string("PBCloud/") + version()});
    const std::string body = std::string(dav::kXmlHeader) + "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:getcontentlength/><d:resourcetype/></d:prop></d:propfind>";
    const Response r = session_.send("PROPFIND", folder_url() + "/", body, h, 8ll << 20);
    if (r.status == 0 || r.status == 401 || r.status == 403 || r.status == 429 || r.status == 503 || r.status >= 500) {
        Result<UploadFolder> out;
        out.error = session_.status_error(r);
        return out;
    }
    return parse_upload_folder(r, plan_);
}

WriteAnswer UploadSession::send_chunk(int n, const std::string& source_path, ProgressFn progress) {
    Headers h = base_headers();
    h.push_back({"Content-Type", "application/octet-stream"});
    h.push_back({"Destination", files_url(session_, user_id_, destination_)});
    h.push_back({"OC-Total-Length", std::to_string(plan_.total)});
    const Response r = session_.send_file("PUT", folder_url() + "/" + chunk_name(n), source_path, plan_.offset_of(n), plan_.size_of(n), h, std::move(progress));
    return interpret_write_answer(r, WriteKind::Chunk, false);
}

WriteAnswer UploadSession::assemble(bool replace, const std::string& replace_etag) {
    Headers h = base_headers();
    h.push_back({"Destination", files_url(session_, user_id_, destination_)});
    h.push_back({"OC-Total-Length", std::to_string(plan_.total)});
    if (mtime_ > 0) h.push_back({"X-OC-Mtime", std::to_string(mtime_)});
    if (!checksum_.empty()) h.push_back({"OC-Checksum", "SHA256:" + checksum_});
    h.push_back({"Overwrite", replace ? "T" : "F"});
    if (replace) h.push_back({"If-Match", replace_etag});
    const Response r = session_.send("MOVE", folder_url() + "/.file", std::string(), h);
    return interpret_write_answer(r, WriteKind::Put, replace);   // a 412 means "the name exists" for a new file and "changed" for a replace
}

WriteAnswer UploadSession::abort() {
    const Response r = session_.send("DELETE", folder_url(), std::string(), base_headers());
    WriteAnswer a = interpret_write_answer(r, WriteKind::Chunk, false);
    if (r.status == 404) a.error = Error();   // already gone: what was asked for has happened
    return a;
}

}  // namespace pbc
