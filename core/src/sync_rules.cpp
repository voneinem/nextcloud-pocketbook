#include "pbc/sync_rules.h"

#include <sys/stat.h>

#include <climits>
#include <cstdlib>

#include "pbc/remote_path.h"
#include "pbc/utf8.h"

namespace pbc {

namespace {

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

std::vector<std::string> components(const std::string& path) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == '/') ++pos;
        const size_t slash = path.find('/', pos);
        if (pos >= path.size()) break;
        out.push_back(path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos));
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return out;
}

// true when `inner` equals `outer` or lies below it, component-wise; with `fold` the names are compared case-folded (the storage ignores case)
bool within(const std::string& inner, const std::string& outer, bool fold) {
    const auto a = components(inner), b = components(outer);
    if (b.size() > a.size()) return false;
    for (size_t i = 0; i < b.size(); ++i) {
        if (fold ? case_fold(a[i]) != case_fold(b[i]) : a[i] != b[i]) return false;
    }
    return true;
}

PairCheck fail(const char* key) {
    PairCheck c;
    c.error_key = key;
    return c;
}

// The canonical form of a path that may not exist: the real path of its longest existing prefix plus the rest as written.
std::string canonical_or_lexical(const std::string& path) {
    const std::string lexical = strip_slash(path);
    if (const std::string r = real_of(lexical); !r.empty()) return r;
    std::string head = lexical, tail;
    while (head.size() > 1) {
        const size_t slash = head.rfind('/');
        tail = head.substr(slash) + tail;
        head = slash == 0 ? "/" : head.substr(0, slash);
        if (const std::string r = real_of(head); !r.empty()) return (r == "/" ? std::string() : r) + tail;
    }
    return lexical;
}

bool has_dot_segment(const std::string& path) {
    for (const std::string& c : components(path))
        if (c == "." || c == "..") return true;
    return false;
}

}  // namespace

bool reader_folder_forbidden(const std::string& folder, const SourceRules& rules, const std::string& download_root) {
    const std::string lexical = strip_slash(folder);
    const std::string root_real = real_of(rules.root);
    const std::string root_lexical = strip_slash(rules.root);
    if (lexical == root_lexical || (!root_real.empty() && lexical == root_real)) return true;   // the picker root itself
    // the folders the app owns, and every folder that contains one of them
    std::vector<std::string> owned = rules.deny;
    if (!download_root.empty()) owned.push_back(download_root);
    const std::string cand_canon = canonical_or_lexical(lexical);
    for (const std::string& o : owned) {
        if (o.empty()) continue;
        const std::string o_lex = strip_slash(o), o_canon = canonical_or_lexical(o);
        if (within(lexical, o_lex, false) || within(cand_canon, o_canon, false) || within(o_lex, lexical, false) || within(o_canon, cand_canon, false)) return true;
    }
    return false;
}

PairCheck check_pair(const SyncPair& p, const std::vector<SyncPair>& existing, const SourceRules& rules, const std::string& download_root) {
    // limits (R-2.5)
    if (p.max_files < kSyncMinFiles || p.max_files > kSyncMaxFilesLimit || p.max_bytes < kSyncMinBytes || p.max_bytes > kSyncMaxBytesLimit)
        return fail("pair_limit_invalid");
    // the server folder is a remote path as the queue uses them (R-2.4)
    if (!is_valid_remote_path(p.remote_folder)) return fail("protocol_error");

    // the reader folder (R-2.1, R-2.2)
    const std::string raw = p.local_folder;
    if (raw.empty() || raw[0] != '/' || has_dot_segment(raw)) return fail("source_not_allowed");
    const std::string lexical = strip_slash(raw);
    const std::string root_real = real_of(rules.root);
    const std::string root_lexical = strip_slash(rules.root);
    if (root_real.empty()) return fail("source_not_allowed");
    if (!within(lexical, root_real, false) && !within(lexical, root_lexical, false)) return fail("source_not_allowed");

    if (reader_folder_forbidden(lexical, rules, download_root)) return fail("pair_folder_forbidden");

    const Result<std::string> allowed = check_source_path(lexical, rules);   // links on the way, outside the root
    if (!allowed.ok()) return fail(allowed.error.key == "source_unreadable" ? "reader_folder_missing" : "source_not_allowed");
    struct stat st;
    if (::stat(allowed.value.c_str(), &st) != 0) return fail("reader_folder_missing");
    if (!S_ISDIR(st.st_mode)) return fail("source_not_allowed");

    // overlap with the other pairs (R-2.3): the reader folders case-folded, the server folders byte-exact
    for (const SyncPair& o : existing) {
        if (p.id != 0 && o.id == p.id) continue;
        const std::string o_canon = canonical_or_lexical(o.local_folder);
        if (within(allowed.value, o_canon, true) || within(o_canon, allowed.value, true)) return fail("pair_overlap");
        if (within(p.remote_folder, o.remote_folder, false) || within(o.remote_folder, p.remote_folder, false)) return fail("pair_overlap");
    }
    // the bound (a changed pair is not a new one)
    int others = 0;
    for (const SyncPair& o : existing)
        if (p.id == 0 || o.id != p.id) ++others;
    if (p.id == 0 && others >= kMaxSyncPairs) return fail("pair_limit");

    PairCheck ok;
    ok.ok = true;
    ok.local_folder = allowed.value;
    return ok;
}

bool needs_send_confirmation(Direction from, Direction to) {
    return direction_allows_to_server(to) && !direction_allows_to_server(from);
}

bool direction_allows_to_reader(Direction d) { return d == Direction::Down || d == Direction::Both; }
bool direction_allows_to_server(Direction d) { return d == Direction::Up || d == Direction::Both; }

SyncPair default_pair(const std::string& server, const std::string& user_id) {
    SyncPair p;
    p.server = server;
    p.user_id = user_id;
    p.direction = Direction::Down;
    p.max_files = 500;
    p.max_bytes = 2LL << 30;
    p.startup_sync = false;
    p.auto_sync = false;
    return p;
}

int64_t reserve_for(int64_t volume_bytes) {
    if (volume_bytes <= 0) return kSyncReserveMin;
    const int64_t percent = volume_bytes / 100 * kSyncReservePercent;
    return percent > kSyncReserveMin ? percent : kSyncReserveMin;
}

}  // namespace pbc
