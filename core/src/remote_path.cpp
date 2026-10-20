#include "pbc/remote_path.h"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace pbc {

namespace {

bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool percent_decode(std::string_view in, std::string* out) {
    out->clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out->push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) return false;   // an escape needs two characters after the %
        const int hi = hex_value(in[i + 1]);
        const int lo = hex_value(in[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    return true;
}

bool has_control(std::string_view s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true) {
        const size_t p = s.find(sep, start);
        if (p == std::string_view::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return parts;
}

Result<std::string> failure(const char* key, const char* detail) {
    Result<std::string> r;
    r.error = make_error(ErrorClass::Protocol, key, detail);
    return r;
}

}  // namespace

std::string normalize_remote_path(std::string_view raw) {
    std::string out;
    for (std::string_view seg : split(raw, '/')) {
        if (seg.empty()) continue;
        out.push_back('/');
        out.append(seg);
    }
    return out.empty() ? std::string("/") : out;
}

std::string parent_of(std::string_view remote_path) {
    const std::string p = normalize_remote_path(remote_path);
    if (p == "/") return p;
    const size_t slash = p.rfind('/');
    return slash == 0 ? std::string("/") : p.substr(0, slash);
}

std::string join_remote(std::string_view folder, std::string_view name) {
    std::string base = normalize_remote_path(folder);
    if (base != "/") base.push_back('/');
    else base = "/";
    base.append(name);
    return base;
}

std::string encode_path(std::string_view remote_path) {
    static const char* hex = "0123456789ABCDEF";
    const std::string p = normalize_remote_path(remote_path);
    std::string out;
    for (unsigned char c : p) {
        if (c == '/' || unreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

Result<std::string> decode_href(std::string_view href, std::string_view user_id) {
    std::string_view path = href;
    const size_t scheme = path.find("://");
    if (scheme != std::string_view::npos && path.find('/') > scheme) {   // an absolute address: keep the path
        const size_t slash = path.find('/', scheme + 3);
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash);
    }
    if (path.empty() || path[0] != '/') return failure("foreign_href", "the address is not a path of this account");
    if (path.find_first_of("?#") != std::string_view::npos) return failure("protocol_error", "the address has a query or a fragment");

    // /remote.php/dav/files/<user>/<segments…>
    const std::vector<std::string_view> raw = split(path.substr(1), '/');
    static const char* const kPrefix[] = {"remote.php", "dav", "files"};
    if (raw.size() < 4) return failure("foreign_href", "the address is not a path of this account");
    for (int i = 0; i < 3; ++i)
        if (raw[i] != kPrefix[i]) return failure("foreign_href", "the address is not a path of this account");
    std::string uid;
    if (!percent_decode(raw[3], &uid)) return failure("protocol_error", "the address is not valid");
    if (uid != user_id) return failure("foreign_href", "the address belongs to another account");

    std::string out;
    for (size_t i = 4; i < raw.size(); ++i) {
        // a single trailing empty segment is the "/" that folders carry; anywhere else an empty segment is malformed
        if (raw[i].empty()) {
            if (i + 1 == raw.size()) break;
            return failure("protocol_error", "the address has an empty segment");
        }
        std::string seg;
        if (!percent_decode(raw[i], &seg)) return failure("protocol_error", "the address is not valid");
        if (has_control(seg) || seg.find('/') != std::string::npos || seg == "." || seg == "..")
            return failure("protocol_error", "the address has an unusable name");
        out.push_back('/');
        out.append(seg);
    }
    Result<std::string> r;
    r.value = out.empty() ? std::string("/") : out;
    return r;
}

bool is_valid_name(std::string_view name) {
    if (name.empty() || name.size() > 1024) return false;
    if (name == "." || name == "..") return false;
    if (name.find('/') != std::string_view::npos) return false;
    return !has_control(name);
}

bool parse_url_origin(std::string_view url, std::string* scheme, std::string* host, int* port) {
    const size_t sep = url.find("://");
    if (sep == std::string_view::npos || sep == 0) return false;
    scheme->assign(url.substr(0, sep));
    for (char& c : *scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t start = sep + 3;
    const size_t end = url.find_first_of("/?#", start);
    std::string authority(url.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    *port = *scheme == "http" ? 80 : 443;
    std::string h = authority;
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        h = authority.substr(0, close + 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') *port = std::atoi(authority.c_str() + close + 2);
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            h = authority.substr(0, colon);
            *port = std::atoi(authority.c_str() + colon + 1);
        }
    }
    if (h.empty()) return false;
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    *host = h;
    return true;
}

bool same_https_origin(std::string_view a, std::string_view b) {
    std::string sa, ha, sb, hb;
    int pa = 0, pb = 0;
    if (!parse_url_origin(a, &sa, &ha, &pa) || !parse_url_origin(b, &sb, &hb, &pb)) return false;
    return sa == "https" && sb == "https" && ha == hb && pa == pb;
}

}  // namespace pbc
