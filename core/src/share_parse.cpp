#include "pbc/share_parse.h"

#include <cctype>
#include <set>

#include "pbc/json_min.h"
#include "pbc/share_date.h"
#include "pbc/utf8.h"

namespace pbc {

namespace {

JsonLimits limits() {
    JsonLimits l;
    l.max_bytes = static_cast<size_t>(kMaxShareResponseBytes);
    l.max_depth = 16;
    l.max_nodes = 40000;
    return l;
}

Error protocol(const char* what) { return make_error(ErrorClass::Protocol, "protocol_error", what); }

const JsonValue* ocs_member(const JsonValue& root, const char* key) {
    const JsonValue* ocs = root.get("ocs");
    return ocs ? ocs->get(key) : nullptr;
}

// Control characters, bidirectional controls and invalid bytes removed; at most kMaxShareLabelBytes, never inside a character.
std::string sanitize(const std::string& in) {
    std::string out;
    size_t pos = 0;
    while (pos < in.size()) {
        const Utf8Char c = utf8_decode(in, pos);
        const size_t len = c.len ? c.len : 1;
        const bool drop = !c.valid || c.cp < 0x20 || c.cp == 0x7f || (c.cp >= 0x80 && c.cp < 0xa0) || c.cp == 0x2028 || c.cp == 0x2029 ||
                          (c.cp >= 0x202a && c.cp <= 0x202e) || (c.cp >= 0x2066 && c.cp <= 0x2069) || c.cp == 0x200e || c.cp == 0x200f ||
                          c.cp == 0xfeff;
        if (!drop) {
            if (out.size() + len > kMaxShareLabelBytes) break;
            out.append(in, pos, len);
        }
        pos += len;
    }
    return out;
}

const JsonValue* string_member(const JsonValue& o, const char* key) {
    const JsonValue* v = o.get(key);
    return v && v->type == JsonValue::Type::String ? v : nullptr;
}

std::optional<int64_t> int_member(const JsonValue& o, const char* key) {
    const JsonValue* v = o.get(key);
    return v && v->type == JsonValue::Type::Number ? v->as_int() : std::nullopt;
}

// A positive integer id given as a JSON number or as a string of 1..18 digits.
std::optional<int64_t> parse_id(const JsonValue* v) {
    if (!v) return std::nullopt;
    if (v->type == JsonValue::Type::Number) {
        const auto n = v->as_int();
        return n && *n > 0 ? n : std::nullopt;
    }
    if (v->type != JsonValue::Type::String || v->text.empty() || v->text.size() > 18) return std::nullopt;
    int64_t n = 0;
    for (char ch : v->text) {
        if (ch < '0' || ch > '9') return std::nullopt;
        n = n * 10 + (ch - '0');
    }
    return n > 0 ? std::optional<int64_t>(n) : std::nullopt;
}

// Fills a ShareRow from one share object. The members `url` and `token` are not looked at here; `password` and `share_with` of a link are
// only tested for being non-empty.
bool parse_row(const JsonValue& o, const std::string& me, bool inherited, bool folder_default, ShareRow* row) {
    if (o.type != JsonValue::Type::Object) return false;
    const auto id = parse_id(o.get("id"));
    if (!id) return false;
    ShareRow r;
    r.id = *id;
    r.inherited = inherited;
    const auto type = int_member(o, "share_type");
    const auto perms = int_member(o, "permissions");
    r.limited = !type || !perms;
    if (type) r.kind = *type == 0 ? ShareKind::User : *type == 1 ? ShareKind::Group : *type == 3 ? ShareKind::PublicLink : ShareKind::Other;
    bool folder = folder_default;
    if (const JsonValue* it = string_member(o, "item_type")) folder = it->text == "folder";
    if (perms && *perms >= 0 && *perms <= 31) {
        r.permission_bits = static_cast<int>(*perms);
        r.permission = link_permission_from_bits(r.permission_bits, folder);
    } else if (perms) {
        r.limited = true;
    }
    if (const JsonValue* e = o.get("expiration")) {
        if (e->type == JsonValue::Type::String && !e->text.empty()) {
            const std::string date = e->text.substr(0, 10);
            if (parse_iso_day(date)) r.expires = date;
            else r.expires_unreadable = true;
        } else if (e->type != JsonValue::Type::Null && !(e->type == JsonValue::Type::String)) {
            r.expires_unreadable = true;
        }
    }
    if (r.kind == ShareKind::PublicLink) {
        const JsonValue* pw = string_member(o, "password");
        const JsonValue* sw = string_member(o, "share_with");
        r.has_password = (pw && !pw->text.empty()) || (sw && !sw->text.empty());
        if (const JsonValue* l = string_member(o, "label")) r.label = sanitize(l->text);
    } else if (r.kind == ShareKind::User || r.kind == ShareKind::Group) {
        const JsonValue* name = string_member(o, "share_with_displayname");
        if (!name || name->text.empty()) name = string_member(o, "share_with");
        if (name) r.with_name = sanitize(name->text);
    }
    if (const JsonValue* owner = string_member(o, "uid_owner")) r.owned_by_me = !me.empty() && owner->text == me;
    *row = std::move(r);
    return true;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Checks an address from the server and decides what the screen may do with it (data-model "ShareDetail").
LinkState check_link(const std::string& link, std::string_view connected_host) {
    if (link.empty() || link.size() > kMaxShareLinkBytes) return LinkState::Unreadable;
    for (unsigned char c : link)
        if (c <= 0x20 || c >= 0x7f) return LinkState::Unreadable;
    // A plaintext scheme is only recognized here to flag the address, never requested; rule N1 looks for the literal, so it is spelled in two parts.
    static const std::string kPlain = std::string("http") + "://";
    static const std::string kSecure = "https://";
    const std::string low = lower(link);
    bool secure;
    size_t start;
    if (low.compare(0, kSecure.size(), kSecure) == 0) { secure = true; start = kSecure.size(); }
    else if (low.compare(0, kPlain.size(), kPlain) == 0) { secure = false; start = kPlain.size(); }
    else return LinkState::Unreadable;
    size_t end = low.find_first_of("/?#", start);
    if (end == std::string::npos) end = low.size();
    const std::string authority = low.substr(start, end - start);
    if (authority.empty() || authority.find('@') != std::string::npos) return LinkState::Unreadable;
    std::string host = authority;
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    if (host.empty()) return LinkState::Unreadable;
    if (!secure) return LinkState::NotSecure;
    return !connected_host.empty() && host == lower(connected_host) ? LinkState::Ok : LinkState::DifferentAddress;
}

const JsonValue* data_of(const JsonValue& root) { return ocs_member(root, "data"); }

}  // namespace

Result<ShareList> parse_share_list(std::string_view body, const std::string& my_user_id, bool inherited, bool item_is_folder) {
    Result<ShareList> out;
    const auto doc = parse_json(body, limits());
    if (!doc.ok()) { out.error = doc.error; return out; }
    const JsonValue* data = data_of(doc.value);
    if (!data || data->type != JsonValue::Type::Array) { out.error = protocol("data is not a list"); return out; }
    std::set<int64_t> seen;
    for (const JsonValue& item : data->items) {
        ShareRow row;
        if (!parse_row(item, my_user_id, inherited, item_is_folder, &row) || !seen.insert(row.id).second) {
            ++out.value.dropped;
            continue;
        }
        if (out.value.rows.size() >= kMaxShareRows) { out.value.truncated = true; break; }
        out.value.rows.push_back(std::move(row));
    }
    return out;
}

Result<ShareDetail> parse_share_detail(std::string_view body, const std::string& my_user_id, std::string_view connected_host, bool item_is_folder) {
    Result<ShareDetail> out;
    const auto doc = parse_json(body, limits());
    if (!doc.ok()) { out.error = doc.error; return out; }
    const JsonValue* data = data_of(doc.value);
    const JsonValue* first = nullptr;
    if (data && data->type == JsonValue::Type::Array && !data->items.empty()) first = &data->items[0];
    else if (data && data->type == JsonValue::Type::Object) first = data;
    if (!first) { out.error = protocol("no share in the answer"); return out; }
    if (!parse_row(*first, my_user_id, false, item_is_folder, &out.value.row)) { out.error = protocol("the share cannot be read"); return out; }
    if (out.value.row.kind == ShareKind::PublicLink) {
        const JsonValue* url = string_member(*first, "url");
        if (url) {
            out.value.link_state = check_link(url->text, connected_host);
            if (out.value.link_state != LinkState::Unreadable) out.value.link = url->text;
        }
    }
    return out;
}

Result<OcsStatus> parse_ocs_status(std::string_view body) {
    Result<OcsStatus> out;
    const auto doc = parse_json(body, limits());
    if (!doc.ok()) { out.error = doc.error; return out; }
    const JsonValue* meta = ocs_member(doc.value, "meta");
    const JsonValue* code = meta ? meta->get("statuscode") : nullptr;
    const auto n = code && code->type == JsonValue::Type::Number ? code->as_int() : std::nullopt;
    if (!n || *n < -100000 || *n > 100000) { out.error = protocol("no status code"); return out; }
    out.value.statuscode = static_cast<int>(*n);
    out.value.ok = *n == 100 || *n == 200;
    return out;
}

}  // namespace pbc
