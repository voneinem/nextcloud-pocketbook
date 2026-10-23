#include "pbc/conflict_name.h"

#include <algorithm>

#include "pbc/new_name.h"
#include "pbc/utf8.h"

namespace pbc {

namespace {

bool is_date(std::string_view d) {
    if (d.size() != 10) return false;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i == 4 || i == 7) {
            if (d[i] != '-') return false;
        } else if (d[i] < '0' || d[i] > '9') {
            return false;
        }
    }
    return true;
}

bool valid_name(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return false;
    for (size_t i = 0; i < name.size();) {
        const Utf8Char c = utf8_decode(name, i);
        if (!c.valid) return false;
        i += c.len;
    }
    return true;
}

}  // namespace

std::string conflict_name(std::string_view name, bool server_copy, std::string_view date, const std::function<bool(std::string_view)>& taken) {
    if (!valid_name(name) || !is_date(date)) return std::string();
    const size_t dot = name.rfind('.');
    std::string stem, ext;
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) {
        stem = std::string(name);
    } else {
        stem = std::string(name.substr(0, dot));
        ext = std::string(name.substr(dot));
    }
    const std::string marker = std::string(" (") + (server_copy ? "server" : "reader") + " copy " + std::string(date) + ")";
    for (int n = 1; n <= 10000; ++n) {
        const std::string suffix = marker + (n > 1 ? " (" + std::to_string(n) + ")" : std::string());
        if (suffix.size() + ext.size() >= kMaxNameBytes) return std::string();   // no room for even one byte of the stem
        std::string use_stem = stem;
        if (use_stem.size() + suffix.size() + ext.size() > kMaxNameBytes) {
            size_t room = kMaxNameBytes - suffix.size() - ext.size();
            while (room > 0 && room < use_stem.size() && (static_cast<unsigned char>(use_stem[room]) & 0xC0) == 0x80) --room;   // not inside a character
            use_stem.resize(std::min(room, use_stem.size()));
            if (use_stem.empty()) return std::string();
        }
        const std::string candidate = use_stem + suffix + ext;
        if (!taken || !taken(candidate)) return candidate;
    }
    return std::string();
}

}  // namespace pbc
