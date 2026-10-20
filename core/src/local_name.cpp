#include "pbc/local_name.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstring>

#include "pbc/remote_path.h"
#include "pbc/utf8.h"

namespace pbc {

namespace {

bool reserved_device_name(const std::string& name) {
    std::string base = name.substr(0, name.find('.'));
    for (char& c : base) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") return true;
    if (base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) && base[3] >= '1' && base[3] <= '9') return true;
    return false;
}

// Splits "name.ext" at the last dot when the extension (with the dot) is at most 16 bytes and the base is not empty.
void split_extension(const std::string& name, std::string* base, std::string* ext) {
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0 && name.size() - dot <= 16) {
        *base = name.substr(0, dot);
        *ext = name.substr(dot);
    } else {
        *base = name;
        ext->clear();
    }
}

// Cuts `s` to at most `max` bytes on a character boundary.
std::string cut_utf8(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

std::vector<std::string> split_path(std::string_view p) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < p.size()) {
        if (p[pos] == '/') { ++pos; continue; }
        size_t next = p.find('/', pos);
        if (next == std::string_view::npos) next = p.size();
        parts.emplace_back(p.substr(pos, next - pos));
        pos = next;
    }
    return parts;
}

Result<std::string> fail(ErrorClass cls, const char* key, const char* detail) {
    Result<std::string> r;
    r.error = make_error(cls, key, detail);
    return r;
}

// The entry of `dir` whose name equals `name` ignoring case, or "" if there is none.
std::string find_ignoring_case(const std::string& dir, const std::string& name) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return std::string();
    const std::string want = case_fold(name);
    std::string found;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (case_fold(n) == want) {
            found = n;
            if (n == name) break;   // an exact match is preferred
        }
    }
    ::closedir(d);
    return found;
}

}  // namespace

std::string sanitize_component(std::string_view server_name, const LocalNameOptions& options) {
    std::string out;
    for (size_t i = 0; i < server_name.size();) {
        const Utf8Char c = utf8_decode(server_name, i);
        if (!c.valid) {
            out.push_back('_');
            ++i;
            continue;
        }
        const uint32_t cp = c.cp;
        const bool bad = cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0) || cp == '\\' || cp == '/' || cp == ':' || cp == '*' || cp == '?' ||
                         cp == '"' || cp == '<' || cp == '>' || cp == '|' || (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069) ||
                         cp == 0x200e || cp == 0x200f || cp == 0x061c;
        if (bad) out.push_back('_');
        else out.append(server_name.substr(i, c.len));
        i += c.len;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty()) return "_";
    if (reserved_device_name(out) || out.compare(0, 5, ".pbc-") == 0) out = "_" + out;
    if (out.size() > options.max_component_bytes) {
        std::string base, ext;
        split_extension(out, &base, &ext);
        out = cut_utf8(base, options.max_component_bytes - ext.size()) + ext;
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();   // the cut may have left a trailing dot
        if (out.empty()) out = "_";
    }
    return out;
}

std::string with_suffix(const std::string& component, int n, bool is_file, const LocalNameOptions& options) {
    const std::string suffix = " (" + std::to_string(n) + ")";
    std::string base = component, ext;
    if (is_file) split_extension(component, &base, &ext);
    const size_t room = options.max_component_bytes > suffix.size() + ext.size() ? options.max_component_bytes - suffix.size() - ext.size() : 1;
    return cut_utf8(base, room) + suffix + ext;
}

TakenMap build_taken(const std::vector<std::pair<std::string, std::string>>& remote_and_local) {
    TakenMap taken;
    for (const auto& pair : remote_and_local) {
        const std::vector<std::string> remote = split_path(pair.first);
        const std::vector<std::string> local = split_path(pair.second);
        std::string remote_prefix, local_prefix;
        for (size_t i = 0; i < local.size() && i < remote.size(); ++i) {
            remote_prefix += "/" + remote[i];
            local_prefix += (i ? "/" : "") + local[i];
            taken[case_fold(local_prefix)] = remote_prefix;
        }
    }
    return taken;
}

Result<std::string> choose_local_path(const std::string& download_root, std::string_view remote_path, const TakenMap& taken, const LocalNameOptions& options) {
    const std::string rp = normalize_remote_path(remote_path);
    if (rp == "/") return fail(ErrorClass::Protocol, "protocol_error", "a folder cannot be downloaded as a file");
    struct stat st;
    if (::lstat(download_root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return fail(ErrorClass::Storage, "path_unsafe", "the download folder is missing or is a link");

    const std::vector<std::string> comps = split_path(rp);
    if (comps.size() > 64) return fail(ErrorClass::Storage, "path_too_long", "the path is too deep");
    std::string rel, remote_prefix;
    for (size_t i = 0; i < comps.size(); ++i) {
        const bool is_file = i + 1 == comps.size();
        remote_prefix += "/" + comps[i];
        const std::string candidate = sanitize_component(comps[i], options);
        const std::string dir = rel.empty() ? download_root : download_root + "/" + rel;
        std::string chosen;
        for (int n = 1; n < 1000; ++n) {
            const std::string name = n == 1 ? candidate : with_suffix(candidate, n, is_file, options);
            const std::string local = rel.empty() ? name : rel + "/" + name;
            const auto owner = taken.find(case_fold(local));
            if (owner != taken.end()) {
                if (owner->second == remote_prefix) { chosen = name; break; }   // this very remote path: its place is kept
                continue;                                                      // another remote path owns it
            }
            const std::string existing = find_ignoring_case(dir, name);
            if (existing.empty()) { chosen = name; break; }
            if (is_file) continue;   // a file the app does not know is never replaced
            // a folder that exists on the card: reused when it is a real folder
            if (::lstat((dir + "/" + existing).c_str(), &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
                return fail(ErrorClass::Storage, "path_unsafe", "a link or a file is where a folder is needed");
            chosen = existing;
            break;
        }
        if (chosen.empty()) return fail(ErrorClass::Storage, "path_too_long", "no free name was found");
        rel = rel.empty() ? chosen : rel + "/" + chosen;
        if (rel.size() > options.max_path_bytes) return fail(ErrorClass::Storage, "path_too_long", "the path is too long");
    }
    Result<std::string> r;
    r.value = rel;
    return r;
}

}  // namespace pbc
