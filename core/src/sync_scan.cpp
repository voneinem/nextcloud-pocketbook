#include "pbc/sync_scan.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <deque>
#include <set>

#include "pbc/hash.h"
#include "pbc/nc_listing.h"
#include "pbc/remote_path.h"
#include "pbc/utf8.h"

namespace pbc {

namespace {

// A problem that makes every further request pointless: the scan ends there. Everything else belongs to one folder.
bool general_problem(const std::string& key) {
    static const char* const keys[] = {"auth_failed", "maintenance", "rate_limited", "server_unreachable", "no_network", "no_connection", "connection_lost",
                                       "cert_untrusted", "cert_expired", "cert_host_mismatch", "redirect_other_host", "tls_failed", "scheme_not_allowed"};
    for (const char* k : keys)
        if (key == k) return true;
    return false;
}

void sort_by_rel(Scan* scan) {
    std::sort(scan->entries.begin(), scan->entries.end(), [](const ScanEntry& a, const ScanEntry& b) { return a.rel < b.rel; });
}

bool safe_rel(const std::string& rel) {
    if (rel.empty() || rel.size() > 4000 || rel[0] == '/' || rel.back() == '/') return false;
    size_t pos = 0;
    while (pos <= rel.size()) {
        const size_t slash = rel.find('/', pos);
        const std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part)
            if (c < 0x20 || c == 0x7f) return false;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

}  // namespace

std::string join_path_checked(const std::string& root, const std::string& rel) {
    if (root.empty() || root[0] != '/' || !safe_rel(rel)) return std::string();
    std::string base = root;
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    return base == "/" ? "/" + rel : base + "/" + rel;
}

// ---------------------------------------------------------------------------------------------------------------------------- the server

Scan scan_server(DavSession& session, std::string_view user_id, const SyncPair& pair, Cancel& cancel) {
    Scan scan;
    struct Item {
        std::string rel;
        int depth;
    };
    std::deque<Item> queue;
    queue.push_back({std::string(), 0});
    int listed = 0, files = 0;
    const std::string base = normalize_remote_path(pair.remote_folder);
    while (!queue.empty()) {
        if (cancel.requested()) {
            scan.stopped_by_key = "cancelled";
            scan.incomplete = true;
            break;
        }
        const Item item = queue.front();
        queue.pop_front();
        if (item.depth > kSyncMaxDepth || listed >= kSyncMaxFolders) {   // a bound: the folder is known by its parent but not looked into
            ++scan.folders_not_listed;
            scan.incomplete = true;
            continue;
        }
        const std::string path = item.rel.empty() ? base : join_remote(base, item.rel);
        ++listed;
        const Result<Listing> fetched = fetch_listing(session, user_id, path);
        if (!fetched.ok()) {
            const std::string& key = fetched.error.key;
            scan.incomplete = true;
            if (item.rel.empty()) {   // the pair's own folder
                scan.stopped_by_key = key == "not_found" ? "server_folder_missing" : key;
                break;
            }
            if (general_problem(key)) {
                scan.stopped_by_key = key;
                ++scan.folders_not_listed;
                break;
            }
            ++scan.folders_not_listed;   // this folder only: what lies below it is unknown
            continue;
        }
        const Listing& listing = fetched.value;
        if (listing.partial) {   // some members could not be read: the folder is only partly compared
            scan.incomplete = true;
            ++scan.folders_not_listed;
        }
        scan.dropped_names += listing.skipped_hostile + listing.skipped_foreign;
        for (const RemoteEntry& e : listing.entries) {
            const std::string rel = rel_join(item.rel, e.name);
            if (rel.empty()) {
                ++scan.dropped_names;
                scan.incomplete = true;
                continue;
            }
            if (e.kind == Kind::File && files >= kSyncMaxFiles) {   // one more than the bound: the scan stops adding files
                scan.incomplete = true;
                continue;
            }
            ScanEntry se;
            se.rel = rel;
            se.kind = e.kind;
            se.size = e.kind == Kind::File ? e.size.value_or(0) : 0;
            se.etag = e.etag;
            se.checksum = e.checksum;
            se.mtime = e.mtime;
            se.availability = e.availability;
            if (e.availability != Availability::Ok) se.skip_reason = "unavailable";
            if (e.kind == Kind::File) ++files;
            else if (e.availability == Availability::Ok) queue.push_back({rel, item.depth + 1});
            scan.entries.push_back(std::move(se));
        }
    }
    sort_by_rel(&scan);
    return scan;
}

// ---------------------------------------------------------------------------------------------------------------------------- the reader

namespace {

struct ReaderWalk {
    Scan* scan;
    Cancel* cancel;
    int folders = 0, files = 0;

    // Reads one folder through its descriptor (`dfd` is consumed), adds its entries and goes into the folders below it.
    void run(int dfd, const std::string& rel, int depth) {
        DIR* d = ::fdopendir(dfd);
        if (!d) {
            ::close(dfd);
            scan->incomplete = true;
            ++scan->folders_not_listed;
            return;
        }
        ++folders;
        std::vector<std::string> names;
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            if (n[0] == '.') {   // hidden, and the temporary files of the downloader (.pbc-<id>.part)
                ++scan->skipped_hidden;
                continue;
            }
            names.push_back(n);
        }
        std::sort(names.begin(), names.end());
        const int fd = ::dirfd(d);
        std::set<std::string> seen_folded;
        struct Sub {
            std::string name, rel;
        };
        std::vector<Sub> below;
        for (const std::string& n : names) {
            if (cancel->requested()) {
                scan->stopped_by_key = "cancelled";
                scan->incomplete = true;
                ::closedir(d);
                return;
            }
            const std::string child = rel_join(rel, n);
            if (child.empty()) continue;
            struct stat st;
            if (::fstatat(fd, n.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;   // gone meanwhile
            ScanEntry se;
            se.rel = child;
            se.mtime = static_cast<int64_t>(st.st_mtime);
            const bool collision = !seen_folded.insert(case_fold(n)).second;   // the storage cannot tell this name from an earlier one
            if (S_ISDIR(st.st_mode)) {
                se.kind = Kind::Folder;
                if (collision) {
                    se.skip_reason = "name_collision";
                    ++scan->folders_not_listed;
                    scan->incomplete = true;
                } else if (::faccessat(fd, n.c_str(), R_OK | X_OK, AT_EACCESS) != 0) {
                    se.skip_reason = "source_unreadable";
                    ++scan->folders_not_listed;
                    scan->incomplete = true;
                } else {
                    below.push_back({n, child});
                }
            } else if (S_ISREG(st.st_mode)) {
                if (files >= kSyncMaxFiles) {
                    scan->incomplete = true;
                    continue;
                }
                ++files;
                se.kind = Kind::File;
                se.size = static_cast<int64_t>(st.st_size);
                if (collision) se.skip_reason = "name_collision";
                else if (::faccessat(fd, n.c_str(), R_OK, AT_EACCESS) != 0) se.skip_reason = "source_unreadable";
            } else {   // a link, a pipe, a socket, a device: never followed, never sent
                se.kind = Kind::File;
                se.skip_reason = "not_regular";
            }
            scan->entries.push_back(std::move(se));
        }
        for (const Sub& s : below) {
            if (scan->stopped_by_key == "cancelled") break;
            if (depth + 1 > kSyncMaxDepth || folders >= kSyncMaxFolders) {
                ++scan->folders_not_listed;
                scan->incomplete = true;
                continue;
            }
            const int sub = ::openat(fd, s.name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub < 0) {
                ++scan->folders_not_listed;
                scan->incomplete = true;
                continue;
            }
            run(sub, s.rel, depth + 1);
        }
        ::closedir(d);
    }
};

}  // namespace

Scan scan_reader(const SyncPair& pair, const SourceRules& rules, Cancel& cancel) {
    Scan scan;
    auto missing = [&]() {
        scan.stopped_by_key = "reader_folder_missing";
        scan.incomplete = true;
        return scan;
    };
    if (cancel.requested()) {
        scan.stopped_by_key = "cancelled";
        scan.incomplete = true;
        return scan;
    }
    const Result<std::string> folder = check_source_path(pair.local_folder, rules);   // inside the storage, no link on the way
    if (!folder.ok()) return missing();
    const int root = ::open(folder.value.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return missing();
    ReaderWalk walk{&scan, &cancel};
    walk.run(root, std::string(), 0);
    sort_by_rel(&scan);
    return scan;
}

// ---------------------------------------------------------------------------------------------------------------------------- records

bool reader_changed(const std::string& path, const SyncRecord& record, Cancel* cancel) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return true;
    if (!record.local_size || static_cast<int64_t>(st.st_size) != *record.local_size || !record.local_mtime) return true;
    const int64_t diff = static_cast<int64_t>(st.st_mtime) - *record.local_mtime;
    if (diff >= -1 && diff <= 1) return false;   // FAT keeps even seconds
    if (record.local_sha256.empty()) return true;   // the time differs and nothing says the bytes are the same
    struct Never : Cancel {
        bool requested() const override { return false; }
    } never;
    const Result<std::string> digest = hash_file(path, HashAlgo::Sha256, cancel ? *cancel : static_cast<Cancel&>(never));
    if (!digest.ok()) return true;   // not known to be unchanged
    std::string want = record.local_sha256;
    std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return digest.value != want;
}

bool server_changed(const ScanEntry& entry, const SyncRecord& record) { return entry.etag.empty() || entry.etag != record.server_etag; }

bool identical_by_checksum(const std::string& reader_file, const ScanEntry& server_entry, Cancel& cancel) {
    const auto sum = strongest(parse_checksums(server_entry.checksum));
    if (!sum) return false;
    struct stat st;
    if (::lstat(reader_file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || static_cast<int64_t>(st.st_size) != server_entry.size) return false;
    const Result<std::string> digest = hash_file(reader_file, sum->algo, cancel);
    return digest.ok() && digest.value == sum->hex;
}

}  // namespace pbc
