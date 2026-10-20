#include "pbc/nc_listing.h"

#include <cstdlib>
#include <set>

#include "pbc/clock_check.h"
#include "pbc/version.h"
#include "pbc/xml_min.h"

namespace pbc {

namespace {

Error bad(const char* key, const char* detail) { return make_error(ErrorClass::Protocol, key, detail); }

std::optional<int64_t> digits(const std::string& raw) {
    const std::string t = dav::trim(raw);
    if (t.empty() || t.size() > 18) return std::nullopt;
    for (char c : t)
        if (c < '0' || c > '9') return std::nullopt;
    return std::atoll(t.c_str());
}

std::string prop_text(const XmlNode& response, const char* ns, const char* name, bool* present = nullptr) {
    const XmlNode* p = dav::prop_in_ok_propstat(response, ns, name);
    if (present) *present = p != nullptr;
    return p ? dav::trim(p->text) : std::string();
}

std::string cut(const std::string& s, size_t max) { return s.size() <= max ? s : s.substr(0, max); }

// "status_403" from the first status line of a response that has no usable propstat
std::string failure_reason(const XmlNode& response) {
    if (const XmlNode* own = response.child(dav::kNsDav, "status")) {
        const int c = dav::status_code(dav::trim(own->text));
        if (c) return "status_" + std::to_string(c);
    }
    for (const XmlNode* ps : response.children_named(dav::kNsDav, "propstat"))
        if (const XmlNode* st = ps->child(dav::kNsDav, "status")) {
            const int c = dav::status_code(dav::trim(st->text));
            if (c) return "status_" + std::to_string(c);
        }
    return "unreadable";
}

// Fills the entry from the properties of one response. Returns false when the response has no usable property set at all: the entry is
// then marked unreadable and cannot be opened.
bool fill_entry(const XmlNode& resp, const std::string& href, RemoteEntry& e) {
    const bool trailing_slash = !href.empty() && href.back() == '/';
    if (!dav::ok_prop_set(resp)) {   // no usable property set at all: the entry is listed, marked, and cannot be opened
        e.kind = trailing_slash ? Kind::Folder : Kind::File;
        e.availability = Availability::Unreadable;
        e.reason = failure_reason(resp);
        return false;
    }
    bool has_type = false;
    const XmlNode* type = dav::prop_in_ok_propstat(resp, dav::kNsDav, "resourcetype");
    has_type = type != nullptr;
    e.kind = has_type ? (type->child(dav::kNsDav, "collection") ? Kind::Folder : Kind::File) : (trailing_slash ? Kind::Folder : Kind::File);
    e.etag = cut(prop_text(resp, dav::kNsDav, "getetag"), 256);
    if (const auto t = parse_http_date(prop_text(resp, dav::kNsDav, "getlastmodified"))) e.mtime = *t;
    e.size = digits(prop_text(resp, dav::kNsOc, "size"));
    if (!e.size) e.size = digits(prop_text(resp, dav::kNsDav, "getcontentlength"));
    e.fileid = digits(prop_text(resp, dav::kNsOc, "fileid"));
    e.mime = cut(prop_text(resp, dav::kNsDav, "getcontenttype"), 100);
    e.permissions = cut(prop_text(resp, dav::kNsOc, "permissions"), 32);
    e.favorite = prop_text(resp, dav::kNsOc, "favorite") == "1";
    if (const XmlNode* sums = dav::prop_in_ok_propstat(resp, dav::kNsOc, "checksums")) {
        std::string joined;
        for (const XmlNode& c : sums->children) {
            const std::string t = dav::trim(c.text);
            if (t.empty()) continue;
            if (!joined.empty()) joined += " ";
            joined += t;
        }
        e.checksum = cut(joined, 256);
    }
    const bool no_etag = e.etag.empty();
    if (prop_text(resp, dav::kNsNc, "is-encrypted") == "1") {
        e.availability = Availability::Encrypted;
        e.reason = "end_to_end_encrypted";
    } else if (!e.permissions.empty() && e.permissions.find('G') == std::string::npos) {
        e.availability = Availability::NoPermission;
        e.reason = "no_read_permission";
    } else if (no_etag && !has_type) {
        e.availability = Availability::Unsupported;
        e.reason = "unsupported_state";
    }
    return true;
}

}  // namespace

std::string listing_request_body() {
    return std::string(dav::kXmlHeader) +
           "<d:propfind xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\" xmlns:nc=\"http://nextcloud.org/ns\"><d:prop>"
           "<d:getetag/><d:getlastmodified/><d:getcontentlength/><d:getcontenttype/><d:resourcetype/>"
           "<oc:fileid/><oc:permissions/><oc:size/><oc:favorite/><oc:checksums/><nc:is-encrypted/>"
           "</d:prop></d:propfind>";
}

Result<Listing> parse_listing(const Response& r, std::string_view user_id, std::string_view requested_path) {
    Result<Listing> out;
    if (r.body_too_large) {
        out.error = bad("listing_too_large", "the answer is larger than the limit");
        return out;
    }
    if (r.status != 207) {
        out.error = bad("protocol_error", "unexpected answer from the server");
        return out;
    }
    const auto ms = dav::parse_multistatus(r);
    if (!ms.ok()) {
        out.error = ms.error;
        return out;
    }
    const auto responses = ms.value.children_named(dav::kNsDav, "response");
    if (responses.size() > static_cast<size_t>(kMaxEntriesPerFolder) + 1) {   // the folder itself and its children
        out.error = bad("listing_too_large", "the folder has too many entries");
        return out;
    }
    const std::string folder = normalize_remote_path(requested_path);
    Listing& l = out.value;
    int unreadable = 0;
    for (const XmlNode* resp : responses) {
        const XmlNode* href_node = resp->child(dav::kNsDav, "href");
        const std::string href = href_node ? dav::trim(href_node->text) : std::string();
        const auto decoded = decode_href(href, user_id);
        if (!decoded.ok()) {
            if (decoded.error.key == "foreign_href") ++l.skipped_foreign;
            else ++l.skipped_hostile;
            continue;
        }
        const std::string path = decoded.value;
        if (path == folder) {   // the folder itself: found by its address, never by its place in the answer
            l.folder_etag = cut(prop_text(*resp, dav::kNsDav, "getetag"), 256);
            l.folder_fileid = digits(prop_text(*resp, dav::kNsOc, "fileid"));
            continue;
        }
        if (parent_of(path) != folder) {
            ++l.skipped_foreign;
            continue;
        }
        RemoteEntry e;
        e.path = path;
        e.name = path.substr(path.rfind('/') + 1);
        if (!is_valid_name(e.name)) {
            ++l.skipped_hostile;
            continue;
        }
        if (!fill_entry(*resp, href, e)) ++unreadable;
        l.entries.push_back(std::move(e));
    }
    l.partial = unreadable > 0 || l.skipped_hostile > 0;
    return out;
}

Result<std::vector<RemoteEntry>> parse_flat_result(const Response& r, std::string_view user_id, size_t max_entries) {
    Result<std::vector<RemoteEntry>> out;
    if (r.body_too_large) {
        out.error = bad("listing_too_large", "the answer is larger than the limit");
        return out;
    }
    if (r.status != 207) {
        out.error = bad("protocol_error", "unexpected answer from the server");
        return out;
    }
    const auto ms = dav::parse_multistatus(r);
    if (!ms.ok()) {
        out.error = ms.error;
        return out;
    }
    std::set<std::string> seen;
    for (const XmlNode* resp : ms.value.children_named(dav::kNsDav, "response")) {
        if (out.value.size() >= max_entries) break;
        const XmlNode* href_node = resp->child(dav::kNsDav, "href");
        const std::string href = href_node ? dav::trim(href_node->text) : std::string();
        const auto decoded = decode_href(href, user_id);
        if (!decoded.ok() || decoded.value == "/") continue;   // another account, a hostile address, or the root itself
        RemoteEntry e;
        e.path = decoded.value;
        e.name = e.path.substr(e.path.rfind('/') + 1);
        if (!is_valid_name(e.name) || !seen.insert(e.path).second) continue;
        fill_entry(*resp, href, e);
        out.value.push_back(std::move(e));
    }
    return out;
}

Result<Listing> fetch_listing(DavSession& session, std::string_view user_id, std::string_view remote_path) {
    Result<Listing> out;
    const std::string path = normalize_remote_path(remote_path);
    const std::string url = session.base_url() + "/remote.php/dav/files" + encode_path("/" + std::string(user_id)) + (path == "/" ? std::string("/") : encode_path(path) + "/");
    Headers headers = dav::dav_headers("1");
    headers.push_back({"User-Agent", std::string("PBCloud/") + version()});
    const Response r = session.send("PROPFIND", url, listing_request_body(), headers, 8ll << 20);
    if (r.body_too_large) {
        out.error = bad("listing_too_large", "the answer is larger than the limit");
        return out;
    }
    bool handled = true;
    const Error e = session.status_error(r, &handled);
    if (handled) {
        out.error = e;
        return out;
    }
    return parse_listing(r, user_id, path);
}

std::string favorites_request_body() {
    return std::string(dav::kXmlHeader) +
           "<oc:filter-files xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\" xmlns:nc=\"http://nextcloud.org/ns\"><d:prop>"
           "<d:getetag/><d:getlastmodified/><d:getcontentlength/><d:getcontenttype/><d:resourcetype/>"
           "<oc:fileid/><oc:permissions/><oc:size/><oc:favorite/><oc:checksums/><nc:is-encrypted/>"
           "</d:prop><oc:filter-rules><oc:favorite>1</oc:favorite></oc:filter-rules></oc:filter-files>";
}

std::string clean_search_term(std::string_view term) {
    std::string t;
    for (char c : term) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 && u != '\t' && u != '\n') continue;   // control characters are dropped; a tab or newline reads as a space
        if (u == 0x7f || c == '%' || c == '_' || c == '\\') continue;
        t.push_back(u < 0x20 ? ' ' : c);
    }
    size_t a = 0, b = t.size();
    while (a < b && t[a] == ' ') ++a;
    while (b > a && t[b - 1] == ' ') --b;
    t = t.substr(a, b - a);
    if (t.size() > 100) {   // cut at a character boundary
        size_t n = 100;
        while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xC0) == 0x80) --n;
        t.resize(n);
    }
    return t;
}

namespace {

std::string xml_text(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

}  // namespace

std::string search_request_body(std::string_view user_id, std::string_view term) {
    const std::string t = clean_search_term(term);
    if (t.empty()) return std::string();
    return std::string(dav::kXmlHeader) +
           "<d:searchrequest xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\"><d:basicsearch>"
           "<d:select><d:prop><d:displayname/><d:getcontenttype/><d:getlastmodified/><d:getetag/><d:resourcetype/><oc:size/><oc:fileid/>"
           "<d:getcontentlength/><oc:permissions/><oc:favorite/></d:prop></d:select>"
           "<d:from><d:scope><d:href>/files/" + xml_text(std::string(user_id)) + "</d:href><d:depth>infinity</d:depth></d:scope></d:from>"
           "<d:where><d:like><d:prop><d:displayname/></d:prop><d:literal>%" + xml_text(t) + "%</d:literal></d:like></d:where>"
           "<d:orderby><d:order><d:prop><d:getlastmodified/></d:prop><d:descending/></d:order></d:orderby>"
           "<d:limit><d:nresults>" + std::to_string(kMaxSearchResults + 1) + "</d:nresults></d:limit>"   // one more than shown: it tells that more exist
           "</d:basicsearch></d:searchrequest>";
}

Result<std::vector<RemoteEntry>> fetch_favorites(DavSession& session, std::string_view user_id) {
    Result<std::vector<RemoteEntry>> out;
    const std::string url = session.base_url() + "/remote.php/dav/files" + encode_path("/" + std::string(user_id)) + "/";
    Headers headers = {{"Content-Type", "application/xml; charset=utf-8"}, {"User-Agent", std::string("PBCloud/") + version()}};
    const Response r = session.send("REPORT", url, favorites_request_body(), headers, 8ll << 20);
    bool handled = true;
    const Error e = session.status_error(r, &handled);
    if (handled) {
        out.error = e;
        return out;
    }
    return parse_flat_result(r, user_id, static_cast<size_t>(kMaxFavorites) + 1);
}

Result<std::vector<RemoteEntry>> search_server(DavSession& session, std::string_view user_id, std::string_view term) {
    Result<std::vector<RemoteEntry>> out;
    const std::string body = search_request_body(user_id, term);
    if (body.empty()) return out;   // nothing usable to search for: no request
    const std::string url = session.base_url() + "/remote.php/dav/";
    Headers headers = {{"Content-Type", "text/xml; charset=utf-8"}, {"User-Agent", std::string("PBCloud/") + version()}};
    const Response r = session.send("SEARCH", url, body, headers, 4ll << 20);
    bool handled = true;
    const Error e = session.status_error(r, &handled);
    if (handled) {
        out.error = e;
        return out;
    }
    return parse_flat_result(r, user_id, static_cast<size_t>(kMaxSearchResults) + 1);
}

}  // namespace pbc
