#include "pbc/source_walk.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <functional>

namespace pbc {

namespace {

Result<std::string> not_allowed(const char* detail) {
    Result<std::string> r;
    r.error = make_error(ErrorClass::Storage, "source_not_allowed", detail);
    return r;
}

std::string real_of(const std::string& path) {
    char* p = ::realpath(path.c_str(), nullptr);
    if (!p) return std::string();
    std::string out(p);
    std::free(p);
    return out;
}

std::string strip_slash(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// true when `path` is `prefix` or lies below it
bool under(const std::string& path, const std::string& prefix) {
    if (prefix.empty()) return false;
    if (path == prefix) return true;
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 && path[prefix.size()] == '/';
}

bool has_dot_segment(const std::string& path) {
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string seg = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (seg == "." || seg == "..") return true;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return false;
}

bool denied(const std::string& lexical, const SourceRules& rules) {
    const std::string real = real_of(lexical);
    for (const std::string& d : rules.deny) {
        const std::string dl = strip_slash(d);
        const std::string dr = real_of(dl);
        if (under(lexical, dl) || (!real.empty() && !dr.empty() && under(real, dr)) || (!dr.empty() && under(lexical, dr))) return true;
    }
    return false;
}

}  // namespace

Result<std::string> check_source_path(const std::string& path, const SourceRules& rules) {
    if (path.empty() || path[0] != '/' || has_dot_segment(path)) return not_allowed("the path is not an absolute path without dot segments");
    const std::string root = real_of(rules.root);
    if (root.empty()) return not_allowed("the storage root is not available");
    const std::string lexical = strip_slash(path);
    // the same root, possibly reached through a link in the name of the root itself (the host tests use /tmp)
    std::string rel;
    if (under(lexical, root) && lexical != root) rel = lexical.substr(root.size() + 1);
    else {
        const std::string root_lexical = strip_slash(rules.root);
        if (under(lexical, root_lexical) && lexical != root_lexical) rel = lexical.substr(root_lexical.size() + 1);
        else return not_allowed("the path is outside the storage root");
    }
    // no link on the way below the root
    std::string cur = root;
    size_t pos = 0;
    while (pos <= rel.size()) {
        const size_t slash = rel.find('/', pos);
        const std::string seg = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!seg.empty()) {
            cur += "/" + seg;
            struct stat st;
            if (::lstat(cur.c_str(), &st) != 0) {
                Result<std::string> r;
                r.error = make_error(ErrorClass::Storage, "source_unreadable", "the path does not exist");
                return r;
            }
            if (S_ISLNK(st.st_mode)) return not_allowed("a symbolic link is not followed");
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    if (denied(cur, rules)) return not_allowed("the path is in a folder that can never be a source");
    Result<std::string> ok;
    ok.value = real_of(cur);
    if (ok.value.empty()) {
        ok.error = make_error(ErrorClass::Storage, "source_unreadable", "the path does not exist");
    }
    return ok;
}

namespace {

struct Walker {
    const SourceRules& rules;
    SourceWalk& out;
    bool stop = false;

    bool room() {
        if (out.folders + out.files >= rules.max_items) {
            out.over_limit = true;
            stop = true;
            return false;
        }
        return true;
    }

    void dir(const std::string& abs, const std::string& rel, int depth) {
        if (depth > rules.max_depth) {
            out.over_limit = true;
            stop = true;
            return;
        }
        DIR* d = ::opendir(abs.c_str());
        if (!d) {
            ++out.skipped;
            return;
        }
        std::vector<std::pair<std::string, bool>> names;   // (name, is a folder)
        while (dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            if (name[0] == '.') {   // hidden
                ++out.skipped;
                continue;
            }
            struct stat st;
            const std::string child = abs + "/" + name;
            if (::lstat(child.c_str(), &st) != 0 || S_ISLNK(st.st_mode) || !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) {
                ++out.skipped;
                continue;
            }
            if (S_ISDIR(st.st_mode) && denied(child, rules)) {
                ++out.skipped;
                continue;
            }
            if (S_ISREG(st.st_mode) && ::access(child.c_str(), R_OK) != 0) {
                ++out.skipped;
                continue;
            }
            names.emplace_back(name, S_ISDIR(st.st_mode));
        }
        ::closedir(d);
        std::sort(names.begin(), names.end());
        // folders first (each before its content), then the files of this folder
        for (const auto& n : names) {
            if (stop) return;
            if (!n.second) continue;
            if (!room()) return;
            SourceEntry e;
            e.rel = rel.empty() ? n.first : rel + "/" + n.first;
            e.name = n.first;
            e.kind = SourceKind::Folder;
            out.entries.push_back(e);
            ++out.folders;
            dir(abs + "/" + n.first, e.rel, depth + 1);
        }
        for (const auto& n : names) {
            if (stop) return;
            if (n.second) continue;
            if (!room()) return;
            struct stat st;
            if (::lstat((abs + "/" + n.first).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                ++out.skipped;
                continue;
            }
            SourceEntry e;
            e.rel = rel.empty() ? n.first : rel + "/" + n.first;
            e.name = n.first;
            e.kind = SourceKind::File;
            e.size = static_cast<int64_t>(st.st_size);
            out.entries.push_back(e);
            ++out.files;
            out.bytes += e.size;
        }
    }
};

}  // namespace

SourceListing list_source_dir(const std::string& path, const SourceRules& rules, const std::vector<std::string>& top_folders) {
    SourceListing out;
    out.unreadable = true;
    const std::string clean = strip_slash(path);
    const std::string root_real = real_of(rules.root);
    const bool at_root = !root_real.empty() && (clean == strip_slash(rules.root) || real_of(clean) == root_real);
    if (!at_root) {
        const auto allowed = check_source_path(clean, rules);
        if (!allowed.ok()) return out;
    } else if (!clean.empty() && clean[0] != '/') {
        return out;
    }
    struct stat st;
    if (::lstat(clean.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return out;
    DIR* d = ::opendir(clean.c_str());
    if (!d) return out;
    out.unreadable = false;
    auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == ".." || name[0] == '.') continue;
        struct stat cs;
        const std::string child = clean + "/" + name;
        if (::lstat(child.c_str(), &cs) != 0 || S_ISLNK(cs.st_mode) || !(S_ISREG(cs.st_mode) || S_ISDIR(cs.st_mode))) continue;
        if (S_ISDIR(cs.st_mode) && denied(child, rules)) continue;
        if (S_ISREG(cs.st_mode) && ::access(child.c_str(), R_OK) != 0) continue;
        if (S_ISDIR(cs.st_mode) && ::access(child.c_str(), R_OK | X_OK) != 0) continue;
        if (at_root && !top_folders.empty()) {
            bool listed = false;
            if (S_ISDIR(cs.st_mode))
                for (const std::string& t : top_folders)
                    if (lower(t) == lower(name)) listed = true;
            if (!listed) continue;
        }
        SourceEntry se;
        se.name = name;
        se.kind = S_ISDIR(cs.st_mode) ? SourceKind::Folder : SourceKind::File;
        se.size = S_ISREG(cs.st_mode) ? static_cast<int64_t>(cs.st_size) : 0;
        out.entries.push_back(std::move(se));
    }
    ::closedir(d);
    std::sort(out.entries.begin(), out.entries.end(), [&](const SourceEntry& a, const SourceEntry& b) {
        if (a.kind != b.kind) return a.kind == SourceKind::Folder;
        const std::string la = lower(a.name), lb = lower(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
    return out;
}

SourceWalk walk_source(const std::string& folder, const SourceRules& rules) {
    SourceWalk w;
    struct stat st;
    if (::lstat(folder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return w;   // links and everything that is not a folder
    const std::string top = strip_slash(folder);
    SourceEntry first;
    first.rel = std::string();
    first.name = top.substr(top.rfind('/') == std::string::npos ? 0 : top.rfind('/') + 1);
    first.kind = SourceKind::Folder;
    w.entries.push_back(first);
    w.folders = 1;
    Walker walker{rules, w};
    walker.dir(top, std::string(), 1);
    return w;
}

}  // namespace pbc
