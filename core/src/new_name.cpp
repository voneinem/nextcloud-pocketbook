#include "pbc/new_name.h"

#include <algorithm>

#include "pbc/utf8.h"

namespace pbc {

namespace {

NameCheck refused() {
    NameCheck r;
    r.ok = false;
    r.error_key = "name_forbidden";
    return r;
}

bool valid_utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const Utf8Char c = utf8_decode(s, i);
        if (!c.valid) return false;
        i += c.len;
    }
    return true;
}

std::string lower_ascii_and_fold(std::string_view s) { return case_fold(s); }

bool contains_char(std::string_view name, std::string_view chars) {
    // `chars` is a list of UTF-8 characters; a name matches when it holds any of them
    for (size_t i = 0; i < chars.size();) {
        const Utf8Char c = utf8_decode(chars, i);
        const size_t len = c.valid ? c.len : 1;
        if (c.valid && name.find(chars.substr(i, len)) != std::string_view::npos) return true;
        i += len;
    }
    return false;
}

std::string with_dot(const std::string& ext) { return !ext.empty() && ext[0] == '.' ? ext : "." + ext; }

}  // namespace

NameCheck check_new_name(std::string_view name, const ServerCaps& caps) {
    if (name.empty() || name.size() > kMaxNameBytes) return refused();
    if (name == "." || name == "..") return refused();
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return refused();
    if (name.back() == ' ' || name.back() == '.') return refused();
    if (!valid_utf8(name)) return refused();

    const std::string folded = lower_ascii_and_fold(name);
    if (!caps.forbidden_chars.empty() && contains_char(name, caps.forbidden_chars)) return refused();
    for (const std::string& n : caps.forbidden_names)
        if (folded == lower_ascii_and_fold(n)) return refused();
    const std::string base = folded.substr(0, folded.find('.'));
    for (const std::string& b : caps.forbidden_basenames)
        if (!b.empty() && base == lower_ascii_and_fold(b)) return refused();
    for (const std::string& e : caps.forbidden_extensions) {
        if (e.empty()) continue;
        const std::string ext = lower_ascii_and_fold(with_dot(e));
        if (folded.size() > ext.size() && folded.compare(folded.size() - ext.size(), ext.size(), ext) == 0) return refused();
    }
    NameCheck ok;
    ok.ok = true;
    return ok;
}

std::string keep_both_name(std::string_view name, const std::function<bool(std::string_view)>& taken, const ServerCaps& caps) {
    if (!check_new_name(name, caps).ok) return std::string();
    // split into stem and extension: the extension starts at the last dot unless that is the first byte (a dotfile) or the last (a trailing dot)
    const size_t dot = name.rfind('.');
    std::string stem, ext;
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) {
        stem = std::string(name);
    } else {
        stem = std::string(name.substr(0, dot));
        ext = std::string(name.substr(dot));
    }
    for (int n = 2; n <= 10001; ++n) {
        const std::string suffix = " (" + std::to_string(n) + ")";
        std::string use_stem = stem;
        if (use_stem.size() + suffix.size() + ext.size() > kMaxNameBytes) {
            size_t room = kMaxNameBytes > suffix.size() + ext.size() ? kMaxNameBytes - suffix.size() - ext.size() : 0;
            while (room > 0 && room < use_stem.size() && (static_cast<unsigned char>(use_stem[room]) & 0xC0) == 0x80) --room;   // not inside a character
            use_stem.resize(std::min(room, use_stem.size()));
        }
        const std::string candidate = use_stem + suffix + ext;
        if (!check_new_name(candidate, caps).ok) continue;
        if (!taken(candidate)) return candidate;
    }
    return std::string();
}

}  // namespace pbc
