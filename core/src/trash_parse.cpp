#include "pbc/trash_parse.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <vector>

#include "pbc/clock_check.h"
#include "pbc/dav_common.h"
#include "pbc/utf8.h"
#include "pbc/xml_min.h"

namespace pbc {

namespace {

constexpr int64_t kDay = 86400;

Error bad(const char* key, const char* detail) { return make_error(ErrorClass::Protocol, key, detail); }

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// One percent-decoding pass; false for an escape that is cut short or not hexadecimal.
bool percent_decode(std::string_view in, std::string* out) {
    out->clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out->push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) return false;
        const int hi = hex_value(in[i + 1]);
        const int lo = hex_value(in[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    return true;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t p = s.find(sep, start);
        if (p == std::string_view::npos) {
            parts.emplace_back(s.substr(start));
            break;
        }
        parts.emplace_back(s.substr(start, p - start));
        start = p + 1;
    }
    return parts;
}

// The path part of a d:href: an absolute address loses its scheme and authority.
std::string path_of_href(const std::string& href) {
    const size_t scheme = href.find("://");
    if (scheme != std::string::npos && scheme < 10) {
        const size_t slash = href.find('/', scheme + 3);
        return slash == std::string::npos ? std::string() : href.substr(slash);
    }
    return href;
}

std::optional<int64_t> digits(const std::string& raw) {
    const std::string t = dav::trim(raw);
    if (t.empty() || t.size() > 18) return std::nullopt;
    for (char c : t)
        if (c < '0' || c > '9') return std::nullopt;
    return std::atoll(t.c_str());
}

std::string prop_text(const XmlNode& response, const char* ns, const char* name) {
    const XmlNode* p = dav::prop_in_ok_propstat(response, ns, name);
    return p ? dav::trim(p->text) : std::string();
}

bool is_control(unsigned char c) { return c < 0x20 || c == 0x7f; }

// What a d:href of the trash answer is.
struct Address {
    enum class Kind { Foreign, Root, Item } kind = Kind::Foreign;
    std::string raw_name;   // the last segment as sent
    bool trailing_slash = false;
};

Address classify(const std::string& href, std::string_view user_id) {
    Address a;
    const std::vector<std::string> segs = split(path_of_href(href), '/');
    if (segs.size() < 6 || !segs[0].empty() || segs[1] != "remote.php" || segs[2] != "dav" || segs[3] != "trashbin" || segs[5] != "trash") return a;
    std::string uid;
    if (!percent_decode(segs[4], &uid) || uid != user_id) return a;
    const size_t rest = segs.size() - 6;
    if (rest == 0 || (rest == 1 && segs[6].empty())) {
        a.kind = Address::Kind::Root;
        return a;
    }
    if (rest == 1) {
        a.kind = Address::Kind::Item;
        a.raw_name = segs[6];
        return a;
    }
    if (rest == 2 && segs[7].empty()) {
        a.kind = Address::Kind::Item;
        a.raw_name = segs[6];
        a.trailing_slash = true;
    }
    return a;   // a deeper level is not ours
}

// "story.epub.d1760000000" -> "story.epub" (the display name when the server sent none)
std::string without_deletion_suffix(const std::string& trash_name) {
    const size_t dot = trash_name.rfind(".d");
    if (dot == std::string::npos || dot + 2 >= trash_name.size()) return trash_name;
    for (size_t i = dot + 2; i < trash_name.size(); ++i)
        if (trash_name[i] < '0' || trash_name[i] > '9') return trash_name;
    return trash_name.substr(0, dot);
}

std::string xml_header_propfind(const char* props, bool with_nc) {
    std::string b = dav::kXmlHeader;
    b += "<d:propfind xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\"";
    if (with_nc) b += " xmlns:nc=\"http://nextcloud.org/ns\"";
    b += "><d:prop>";
    b += props;
    b += "</d:prop></d:propfind>";
    return b;
}

// the checks every answer passes first: size, status and the multistatus root
Error check_answer(const Response& r, size_t limit, XmlNode* root) {
    if (r.body_too_large || r.body.size() > limit) return bad("listing_too_large", "the answer is larger than the limit");
    if (r.status != 207) return bad("protocol_error", "unexpected answer from the server");
    auto ms = dav::parse_multistatus(r);
    if (!ms.ok()) return ms.error;
    *root = std::move(ms.value);
    return Error();
}

// digit strings compare as numbers: shorter is smaller, then lexicographic
bool id_less(const std::string& a, const std::string& b) { return a.size() != b.size() ? a.size() < b.size() : a < b; }

}  // namespace

bool is_safe_segment(std::string_view s) {
    if (s.empty() || s.size() > kMaxSegmentBytes) return false;
    if (s == "." || s == "..") return false;
    for (unsigned char c : s)
        if (is_control(c) || c == '/' || c == '\\') return false;
    for (size_t i = 0; i < s.size();) {
        const Utf8Char ch = utf8_decode(s, i);
        if (!ch.valid) return false;
        i += ch.len;
    }
    return true;
}

std::string display_name(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const Utf8Char ch = utf8_decode(s, i);
        const size_t len = ch.len ? ch.len : 1;
        if (len == 1 && is_control(static_cast<unsigned char>(s[i]))) {
            i += 1;
            continue;
        }
        if (out.size() + len > kMaxSegmentBytes) break;
        out.append(s.substr(i, len));
        i += len;
    }
    return out;
}

OriginalPlace parse_original_location(std::string_view location) {
    OriginalPlace p;
    const std::string trimmed = dav::trim(std::string(location));
    if (trimmed.empty() || trimmed.size() > kMaxLocationBytes) return p;
    size_t start = 0;
    while (start < trimmed.size() && trimmed[start] == '/') ++start;
    const std::vector<std::string> segs = split(std::string_view(trimmed).substr(start), '/');
    if (segs.empty()) return p;
    for (const std::string& s : segs)
        if (!is_safe_segment(s)) return p;   // an empty segment, "." and ".." are refused here too
    std::string dir;
    for (size_t i = 0; i + 1 < segs.size(); ++i) dir += "/" + segs[i];
    p.dir = dir.empty() ? std::string("/") : dir;
    p.name = segs.back();
    p.known = true;
    return p;
}

std::string trash_request_body() {
    return xml_header_propfind("<nc:trashbin-filename/><nc:trashbin-original-location/><nc:trashbin-deletion-time/>"
                               "<d:getcontentlength/><d:resourcetype/><oc:fileid/><oc:permissions/><nc:is-encrypted/>",
                               true);
}

std::string versions_request_body() {
    return xml_header_propfind("<d:getlastmodified/><d:getcontentlength/><d:getcontenttype/><nc:version-label/>", true);
}

Result<TrashList> parse_trash(const Response& r, std::string_view user_id, int64_t now) {
    Result<TrashList> out;
    XmlNode root;
    out.error = check_answer(r, kMaxTrashBody, &root);
    if (!out.ok()) return out;
    TrashList& l = out.value;
    std::set<std::string> seen;
    for (const XmlNode* resp : root.children_named(dav::kNsDav, "response")) {
        const XmlNode* href_node = resp->child(dav::kNsDav, "href");
        const std::string href = href_node ? dav::trim(href_node->text) : std::string();
        const Address addr = classify(href, user_id);
        if (addr.kind == Address::Kind::Root) continue;   // the trash itself
        if (addr.kind == Address::Kind::Foreign) {
            ++l.skipped_hostile;
            continue;
        }
        if (!seen.insert(href).second) {   // the same address twice: the first one counts
            ++l.skipped_hostile;
            continue;
        }
        const XmlNode* props = dav::ok_prop_set(*resp);
        if (!props) {   // the member failed: the others are unaffected
            l.partial = true;
            continue;
        }
        TrashItem item;
        std::string decoded;
        const bool decoded_ok = percent_decode(addr.raw_name, &decoded);
        item.unreadable = !decoded_ok || !is_safe_segment(decoded);
        if (!item.unreadable) {
            item.trash_name = decoded;
            const std::string shown = prop_text(*resp, dav::kNsNc, "trashbin-filename");
            item.name = display_name(shown.empty() ? without_deletion_suffix(decoded) : shown);
        }
        const XmlNode* type = dav::prop_in_ok_propstat(*resp, dav::kNsDav, "resourcetype");
        const bool collection = type ? type->child(dav::kNsDav, "collection") != nullptr : addr.trailing_slash;
        item.kind = collection ? Kind::Folder : Kind::File;
        if (!item.unreadable) {
            const OriginalPlace place = parse_original_location(prop_text(*resp, dav::kNsNc, "trashbin-original-location"));
            if (place.known) {
                item.original_known = true;
                item.original_dir = place.dir;
                item.original_name = place.name;
            }
        }
        if (const auto t = digits(prop_text(*resp, dav::kNsNc, "trashbin-deletion-time")))
            if (*t <= now + kDay) item.deleted_at = *t;
        if (!collection) item.size = digits(prop_text(*resp, dav::kNsDav, "getcontentlength"));
        item.fileid = digits(prop_text(*resp, dav::kNsOc, "fileid"));
        const std::string permissions = prop_text(*resp, dav::kNsOc, "permissions");
        if (prop_text(*resp, dav::kNsNc, "is-encrypted") == "1") item.availability = Availability::Encrypted;
        else if (!permissions.empty() && permissions.find('D') == std::string::npos) item.availability = Availability::NoPermission;
        else if (type && !type->children.empty() && !(type->children.size() == 1 && type->child(dav::kNsDav, "collection"))) item.availability = Availability::Unsupported;
        l.items.push_back(std::move(item));
    }
    l.total_seen = static_cast<int>(l.items.size());
    std::stable_sort(l.items.begin(), l.items.end(), [](const TrashItem& a, const TrashItem& b) {
        if (a.deleted_at.has_value() != b.deleted_at.has_value()) return a.deleted_at.has_value();   // unknown last
        if (a.deleted_at && *a.deleted_at != *b.deleted_at) return *a.deleted_at > *b.deleted_at;
        return a.trash_name < b.trash_name;
    });
    if (l.items.size() > kMaxTrashRows) {
        l.truncated = true;
        l.items.resize(kMaxTrashRows);
    }
    return out;
}

Result<VersionList> parse_versions(const Response& r, int64_t now) {
    Result<VersionList> out;
    XmlNode root;
    out.error = check_answer(r, kMaxVersionBody, &root);
    if (!out.ok()) return out;
    VersionList& l = out.value;
    for (const XmlNode* resp : root.children_named(dav::kNsDav, "response")) {
        const XmlNode* href_node = resp->child(dav::kNsDav, "href");
        const std::string path = path_of_href(href_node ? dav::trim(href_node->text) : std::string());
        if (path.empty() || path.back() == '/') continue;   // the collection of the file's versions itself
        const std::string raw = path.substr(path.rfind('/') + 1);
        std::string id;
        bool ok = percent_decode(raw, &id) && !id.empty() && id.size() <= 20;
        if (ok)
            for (char c : id)
                if (c < '0' || c > '9') ok = false;
        const XmlNode* props = ok ? dav::ok_prop_set(*resp) : nullptr;
        if (!ok || !props) {
            ++l.skipped;
            continue;
        }
        VersionRow row;
        row.version_id = id;
        row.mtime = parse_http_date(prop_text(*resp, dav::kNsDav, "getlastmodified"));
        row.size = digits(prop_text(*resp, dav::kNsDav, "getcontentlength"));
        row.future = row.mtime && *row.mtime > now + kDay;
        l.rows.push_back(std::move(row));
    }
    std::stable_sort(l.rows.begin(), l.rows.end(), [](const VersionRow& a, const VersionRow& b) {
        if (a.mtime.has_value() != b.mtime.has_value()) return a.mtime.has_value();   // unknown last
        if (a.mtime && *a.mtime != *b.mtime) return *a.mtime > *b.mtime;
        return id_less(b.version_id, a.version_id);   // the larger id first
    });
    if (l.rows.size() > kMaxVersionRows) {
        l.truncated = true;
        l.rows.resize(kMaxVersionRows);
    }
    return out;
}

}  // namespace pbc
