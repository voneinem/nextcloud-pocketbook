#include "pbc/downloader.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "pbc/auth_guard.h"
#include "pbc/credentials_file.h"
#include "pbc/dav_common.h"
#include "pbc/hash.h"
#include "pbc/local_files.h"
#include "pbc/local_name.h"
#include "pbc/log.h"
#include "pbc/remote_path.h"
#include "pbc/utf8.h"
#include "pbc/secure_wipe.h"
#include "pbc/version.h"

namespace pbc {

namespace {

constexpr int64_t kMiB = 1024 * 1024;

bool strong(const std::string& etag) { return !etag.empty() && etag.compare(0, 2, "W/") != 0; }

std::string dirname_of(const std::string& rel) {
    const size_t slash = rel.rfind('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}
std::string basename_of(const std::string& rel) {
    const size_t slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

int64_t file_size(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
}

// "bytes 100-199/200" -> start 100, total 200 (-1 if absent)
bool parse_content_range(const std::string& v, int64_t* start, int64_t* total) {
    if (v.compare(0, 6, "bytes ") != 0) return false;
    char* end = nullptr;
    const long long a = std::strtoll(v.c_str() + 6, &end, 10);
    if (!end || *end != '-') return false;
    std::strtoll(end + 1, &end, 10);
    *start = a;
    *total = -1;
    if (end && *end == '/' && *(end + 1) != '*') *total = std::strtoll(end + 1, nullptr, 10);
    return true;
}

}  // namespace

Preflight preflight_download(const StorageInfo& storage, const std::string& dir, std::optional<int64_t> size, int64_t warn_bytes) {
    Preflight p;
    p.size = size.value_or(0);
    auto refuse = [&](const char* key) {
        p.verdict = Preflight::Verdict::Refused;
        p.error_key = key;
        return p;
    };
    if (size && *size > storage.max_file_bytes(dir)) return refuse("too_large_for_storage");
    if (!storage.writable(dir)) return refuse("storage_readonly");
    if (size) p.needed = *size + std::max<int64_t>(8 * kMiB, *size / 100);
    const auto free = storage.free_bytes(dir);
    if (free) p.free = *free;
    if (size && free && *free < p.needed) return refuse("not_enough_space");
    if (size && *size >= warn_bytes) p.verdict = Preflight::Verdict::NeedsConfirmation;
    return p;
}

struct Downloader::Job {
    int64_t id = 0;
    std::string remote;
    std::string rel;                     // relative target path
    std::string target;                  // absolute
    std::string temp;                    // absolute
    std::string etag;                    // a strong ETag usable for If-Range, "" if none
    std::optional<int64_t> expected;     // the total size the server announced
    std::string checksum;
    std::optional<int64_t> remote_mtime;
    ConflictPolicy policy = ConflictPolicy::Ask;
    std::string root;                    // the folder `rel` is relative to: the download root, or the reader folder of a pair
    int64_t pair_id = 0;                 // > 0: recorded in sync_files
    std::string rel_server;              // pair: the path below the pair's server folder
    bool strict = false;                 // the version in planned_etag is required
    std::string planned_etag;
    std::string checksum_sha256;         // the SHA-256 of the finished file, when the verification computed one
    bool track = true;                   // recorded in local_files at the end
    std::string copy_of;                 // pair, KeepBoth: the original this copy was made for
    std::string recorded_etag;           // the ETag of the response that delivered the bytes, as sent
    std::optional<int64_t> recorded_size;
    // account
    std::string base_url, login, password, user_id;
};

Downloader::Downloader(Store& store, HttpTransport& http, StorageInfo& storage, const Clock& clock, std::string download_root,
                       std::function<bool()> wifi_up, DownloaderOptions options)
    : store_(&store), http_(&http), storage_(&storage), clock_(&clock), root_(std::move(download_root)), wifi_up_(std::move(wifi_up)), options_(std::move(options)) {}

void Downloader::sleep(int ms) {
    if (options_.sleep_ms) options_.sleep_ms(ms);
    else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

namespace {

void notify(const std::function<void(const JobProgress&)>& cb, JobState s, int64_t done, int64_t total, int attempt) {
    if (!cb) return;
    JobProgress p;
    p.state = s;
    p.done = done;
    p.total = total;
    p.attempt = attempt;
    cb(p);
}

// Removes the partial file and ends the journal row.
void abandon(Store& store, int64_t id, const std::string& temp) {
    ::unlink(temp.c_str());
    store.journal_end(id);
}

DownloadOutcome end(JobState s, const char* key, int64_t id = 0) {
    DownloadOutcome o;
    o.state = s;
    o.error_key = key ? key : "";
    o.job_id = id;
    return o;
}

}  // namespace

DownloadOutcome Downloader::run(const DownloadRequest& req, Cancel& cancel, const std::function<void(const JobProgress&)>& on_progress) {
    Job job;
    job.remote = normalize_remote_path(req.remote_path);
    job.checksum = req.checksum;
    job.remote_mtime = req.remote_mtime;
    job.policy = req.policy;
    job.etag = strong(req.etag) ? req.etag : "";
    job.expected = req.size;
    job.pair_id = req.pair_id;
    job.root = req.pair_id > 0 ? req.target_root : root_;
    job.strict = req.pair_id > 0 && req.strict_version && strong(req.etag);
    if (job.strict) job.planned_etag = req.etag;
    if (job.pair_id > 0) {
        struct stat rs;
        if (job.root.empty() || job.root[0] != '/' || ::lstat(job.root.c_str(), &rs) != 0 || !S_ISDIR(rs.st_mode)) return end(JobState::Failed, "path_unsafe");
        const std::string strip = normalize_remote_path(req.strip_prefix);
        const std::string prefix = strip == "/" ? std::string("/") : strip + "/";
        if (job.remote.size() <= prefix.size() || job.remote.compare(0, prefix.size(), prefix) != 0) return end(JobState::Failed, "protocol_error");
        job.rel_server = job.remote.substr(prefix.size());
    }

    const auto account = store_->account();
    AuthGuard guard(*store_);
    if (!account || guard.blocked()) return end(JobState::Failed, "auth_failed");
    auto creds = load_credentials(store_->directory());
    if (!creds.ok() || creds.value.password.empty()) return end(JobState::Failed, "auth_failed");
    job.base_url = account->base_url;
    job.login = account->login_name;
    job.user_id = account->user_id;
    job.password = creds.value.password;
    secure_wipe(creds.value.password);

    // where the file goes
    const bool pair = job.pair_id > 0;
    std::optional<SyncRecord> srec;
    if (pair) srec = store_->sync_record(job.pair_id, job.rel_server);
    const auto existing = pair ? std::optional<LocalFile>() : store_->local_file(job.remote);
    std::string rel;
    if (pair) {
        // the recorded name, else a safe name below the pair's folder that is free in the records and on the card
        if (srec) {
            rel = srec->local_rel;
        } else {
            std::vector<std::pair<std::string, std::string>> taken_pairs;
            for (const SyncRecord& r : store_->sync_records(job.pair_id)) taken_pairs.push_back({"/" + r.rel, r.local_rel});
            const auto chosen = choose_local_path(job.root, "/" + job.rel_server, build_taken(taken_pairs));
            if (!chosen.ok()) return end(JobState::Failed, chosen.error.key.c_str());
            rel = chosen.value;
        }
        if (req.policy == ConflictPolicy::KeepBoth) {   // the copy beside the original, under the exact name the plan chose
            const std::string name = req.conflict_name;
            if (name.empty() || name[0] == '.' || sanitize_component(name) != name) return end(JobState::Failed, "name_forbidden");
            const std::string dir = dirname_of(rel);
            const std::string folder = dir.empty() ? job.root : job.root + "/" + dir;
            if (DIR* d = ::opendir(folder.c_str())) {
                bool taken = false;
                while (dirent* e = ::readdir(d))
                    if (case_fold(e->d_name) == case_fold(name)) taken = true;
                ::closedir(d);
                if (taken) return end(JobState::Failed, "name_exists");
            }
            rel = dir.empty() ? name : dir + "/" + name;
            job.copy_of = req.copy_of;
            if (job.copy_of.empty()) return end(JobState::Failed, "protocol_error");
        }
    } else if (req.policy == ConflictPolicy::KeepBoth && existing) {
        const std::string dir = dirname_of(existing->local_path);
        std::string base = basename_of(existing->local_path);
        const size_t dot = base.rfind('.');
        const std::string stem = dot != std::string::npos && dot > 0 && base.size() - dot <= 16 ? base.substr(0, dot) : base;
        const std::string ext = stem.size() == base.size() ? std::string() : base.substr(stem.size());
        for (int n = 1; n < 1000; ++n) {
            std::string name = stem + " (server copy)" + (n > 1 ? " (" + std::to_string(n) + ")" : std::string()) + ext;
            const std::string candidate = dir.empty() ? name : dir + "/" + name;
            // free on the card, compared the way the storage compares: ignoring case
            bool taken = false;
            const std::string folder = dir.empty() ? root_ : root_ + "/" + dir;
            if (DIR* d = ::opendir(folder.c_str())) {
                while (dirent* e = ::readdir(d))
                    if (case_fold(e->d_name) == case_fold(name)) taken = true;
                ::closedir(d);
            }
            if (!taken) { rel = candidate; break; }
        }
        if (rel.empty()) return end(JobState::Failed, "path_too_long");
        job.track = false;
    } else if (existing) {
        rel = existing->local_path;
    } else {
        const auto chosen = choose_local_path(root_, job.remote, build_taken(store_->local_path_pairs()));
        if (!chosen.ok()) return end(JobState::Failed, chosen.error.key.c_str());
        rel = chosen.value;
    }
    job.rel = rel;
    job.target = job.root + "/" + rel;

    // a copy that the user changed is never overwritten without asking
    if (req.policy == ConflictPolicy::Ask && existing) {
        const FileStat now = stat_file(job.target);
        if (now.exists && local_copy_changed(*existing, now)) return end(JobState::Blocked, "local_changed");
    }
    if (req.policy == ConflictPolicy::Ask && pair) {   // a file of the pair folder that is not exactly the recorded copy is never overwritten
        const FileStat now = stat_file(job.target);
        if (now.exists && (!srec || !now.regular || now.size != srec->local_size.value_or(-1) || now.mtime != srec->local_mtime.value_or(-1)))
            return end(JobState::Blocked, "local_changed");
    }

    // the checks before the first byte
    const Preflight pf = preflight_download(*storage_, dirname_of(job.target), req.size, INT64_MAX);
    if (pf.verdict == Preflight::Verdict::Refused) return end(JobState::Failed, pf.error_key.c_str());
    const Error dirs = ensure_directories(job.root, dirname_of(rel));
    if (!dirs.ok()) return end(JobState::Failed, dirs.key.c_str());

    // the journal row, before the first request
    DownloadJournalRow row;
    row.remote_path = job.remote;
    row.target_path = rel;
    row.expected_etag = job.etag;
    row.expected_size = req.size;
    row.state = "running";
    if (pair) {
        row.pair_id = job.pair_id;
        row.target_root = job.root;
        row.checksum = req.checksum;
        row.strict = job.strict;
    }
    if (pair) {   // a cut-short earlier try of this very file (paused by a death or a lost connection) is replaced by this one: never two partial files for one record
        for (const DownloadJournalRow& old : store_->journal()) {
            if (old.pair_id != job.pair_id || old.remote_path != job.remote) continue;
            const std::string dir = dirname_of(old.target_path);
            const std::string base = old.target_root.empty() ? job.root : old.target_root;
            abandon(*store_, old.id, (dir.empty() ? base : base + "/" + dir) + "/.pbc-" + std::to_string(old.id) + ".part");
        }
    }
    const auto began = store_->journal_begin(row);
    if (!began.ok()) return end(JobState::Failed, "write_failed");
    job.id = began.value;
    job.temp = (dirname_of(job.target).empty() ? job.root : dirname_of(job.target)) + "/.pbc-" + std::to_string(job.id) + ".part";
    log::event("download.start", {log::count("job", static_cast<long>(job.id))});
    if (options_.hook) options_.hook("journal");
    return transfer(job, cancel, on_progress);
}

DownloadOutcome Downloader::resume(int64_t job_id, Cancel& cancel, const std::function<void(const JobProgress&)>& on_progress) {
    const auto rows = store_->journal();
    const DownloadJournalRow* row = nullptr;
    for (const auto& r : rows)
        if (r.id == job_id) row = &r;
    if (!row) return end(JobState::Failed, "protocol_error");
    Job job;
    job.id = row->id;
    job.remote = row->remote_path;
    job.rel = row->target_path;
    job.pair_id = row->pair_id;
    job.root = row->pair_id > 0 && !row->target_root.empty() ? row->target_root : root_;
    job.target = job.root + "/" + row->target_path;
    job.temp = (dirname_of(job.target).empty() ? job.root : dirname_of(job.target)) + "/.pbc-" + std::to_string(job.id) + ".part";
    job.etag = strong(row->expected_etag) ? row->expected_etag : "";
    job.expected = row->expected_size;
    job.policy = ConflictPolicy::Ask;
    if (job.pair_id > 0) {   // a pair's download: the pair is looked up again; without it the partial file is worthless
        const auto sp = store_->sync_pair(job.pair_id);
        const std::string prefix = sp ? (sp->remote_folder == "/" ? std::string("/") : sp->remote_folder + "/") : std::string();
        if (!sp || job.remote.size() <= prefix.size() || job.remote.compare(0, prefix.size(), prefix) != 0) {
            abandon(*store_, job.id, job.temp);
            return end(JobState::Failed, "protocol_error");
        }
        job.rel_server = job.remote.substr(prefix.size());
        job.checksum = row->checksum;
        job.strict = row->strict && !job.etag.empty();
        if (job.strict) job.planned_etag = job.etag;
    } else if (const auto entry = store_->entry(job.remote)) {
        job.checksum = entry->checksum;
        job.remote_mtime = entry->mtime;
    }
    const auto account = store_->account();
    AuthGuard guard(*store_);
    if (!account || guard.blocked()) return end(JobState::Failed, "auth_failed", job.id);
    auto creds = load_credentials(store_->directory());
    if (!creds.ok() || creds.value.password.empty()) return end(JobState::Failed, "auth_failed", job.id);
    job.base_url = account->base_url;
    job.login = account->login_name;
    job.user_id = account->user_id;
    job.password = creds.value.password;
    secure_wipe(creds.value.password);
    store_->journal_update(job.id, std::max<int64_t>(0, file_size(job.temp)), "running");
    log::event("download.resume", {log::count("job", static_cast<long>(job.id))});
    return transfer(job, cancel, on_progress);
}

DownloadOutcome Downloader::transfer(Job& job, Cancel& cancel, const std::function<void(const JobProgress&)>& on_progress) {
    AuthGuard guard(*store_);
    DavSession session(*http_, job.base_url, job.login, job.password);
    const std::string url = job.base_url + "/remote.php/dav/files" + encode_path("/" + job.user_id) + encode_path(job.remote);
    auto finish = [&](DownloadOutcome o) {
        secure_wipe(job.password);
        return o;
    };
    auto fail = [&](const std::string& key) {
        abandon(*store_, job.id, job.temp);
        log::event("download.fail", {log::text("cause", key)});
        notify(on_progress, JobState::Failed, 0, 0, 0);
        return finish(end(JobState::Failed, key.c_str()));
    };
    auto cancelled = [&]() {
        abandon(*store_, job.id, job.temp);
        log::event("download.cancel", {log::count("job", static_cast<long>(job.id))});
        notify(on_progress, JobState::Cancelled, 0, 0, 0);
        return finish(end(JobState::Cancelled, "cancelled"));
    };

    int attempt_index = 0;       // which wait comes next
    int waited_s = 0;
    int64_t size_at_failure = -1;
    bool restarted_after_416 = false;
    int64_t last_reported = -1;
    int64_t last_report_time = 0;
    bool body_hook_called = false;

    for (;;) {
        if (cancel.requested()) return cancelled();
        int64_t offset = std::max<int64_t>(0, file_size(job.temp));
        const bool ranged = offset > 0 && !job.etag.empty();
        if (offset > 0 && !ranged) {   // no strong validator: the partial bytes cannot be trusted to belong to one version
            ::unlink(job.temp.c_str());
            offset = 0;
        }
        bool complete = job.expected && offset > 0 && offset == *job.expected && ranged;   // a resume of a file that only needs its checks

        Response resp;
        if (!complete) {
            Request req;
            req.method = "GET";
            req.url = url;
            req.username = job.login;
            req.password = job.password;
            req.headers.push_back({"User-Agent", std::string("PBCloud/") + version()});
            if (ranged) {
                req.headers.push_back({"Range", "bytes=" + std::to_string(offset) + "-"});
                req.headers.push_back({"If-Range", job.etag});
            }
            if (job.strict) req.headers.push_back({"If-Match", job.planned_etag});   // only the planned version is wanted (research R-7)
            req.save_body_to = job.temp;
            req.save_append = true;
            req.resume_from = offset;
            req.buffer_size = 65536;
            req.identity_encoding = true;
            req.connect_timeout_s = 15;
            req.total_timeout_s = 0;   // a transfer has no overall time limit; a stalled one is stopped by the low-speed guard
            req.max_response_bytes = 1 << 20;
            req.progress = [&](long long done, long long total) {
                if (cancel.requested()) return false;
                const int64_t known_total = total > 0 ? total : job.expected.value_or(0);
                if (!body_hook_called && known_total > 0 && done * 2 >= known_total) {
                    body_hook_called = true;
                    if (options_.hook) options_.hook("body");
                }
                const int64_t now = clock_->now();
                const bool first = last_reported < 0;
                const bool step = known_total > 0 ? (done - last_reported) * 20 >= known_total : (now - last_report_time) >= 2;
                if (first || step || now - last_report_time >= 2) {
                    last_reported = done;
                    last_report_time = now;
                    notify(on_progress, JobState::Running, done, known_total, attempt_index);
                }
                return true;
            };
            resp = http_->perform(req);
        }

        if (!complete) {
            if (resp.cancelled || cancel.requested()) return cancelled();
            if (resp.storage_full) return fail("storage_full");
            if (resp.body_too_large) return fail("too_large_for_storage");

            const bool transport_failure = resp.error != ErrorClass::None || resp.status == 0;
            if (transport_failure) {
                const std::string key = resp.error_key.empty() ? "server_unreachable" : resp.error_key;
                const bool connection = resp.error == ErrorClass::Network || key == "connection_lost" || key == "timeout" || key == "server_unreachable" || key == "no_network";
                if (!connection) return fail(key);   // a certificate problem, a refused redirect, a write error: waiting does not help
                // the connection dropped: wait for it without sending anything, then continue
                const int64_t now_size = std::max<int64_t>(0, file_size(job.temp));
                if (now_size > size_at_failure && size_at_failure >= 0) { attempt_index = 0; waited_s = 0; }   // it got further this time: a new drop
                size_at_failure = now_size;
                // a partial answer's headers tell which version the bytes belong to
                if (resp.status == 200 || resp.status == 206) {
                    const std::string e = header_value(resp.headers, "ETag");
                    if (strong(e)) { job.etag = e; store_->journal_set_expected(job.id, job.etag, job.expected); }
                    else if (!e.empty()) job.etag.clear();
                }
                store_->journal_update(job.id, now_size, "waiting");
                for (;;) {
                    notify(on_progress, JobState::Waiting, now_size, job.expected.value_or(0), attempt_index);
                    if (attempt_index >= static_cast<int>(options_.delays_s.size()) || waited_s >= options_.max_wait_s) {
                        store_->journal_update(job.id, now_size, "paused");
                        notify(on_progress, JobState::Paused, now_size, job.expected.value_or(0), attempt_index);
                        return finish(end(JobState::Paused, key.c_str(), job.id));
                    }
                    const int delay = options_.delays_s[static_cast<size_t>(attempt_index++)];
                    for (int s = 0; s < delay; ++s) {
                        sleep(1000);
                        if (cancel.requested()) return cancelled();
                    }
                    waited_s += delay;
                    if (wifi_up_ && !wifi_up_()) continue;   // still no Wi-Fi: another wait, nothing is sent
                    break;
                }
                store_->journal_update(job.id, now_size, "running");
                continue;   // retry the request
            }

            // an answer of the server
            if (resp.status == 416) {
                if (offset > 0 && !restarted_after_416) {
                    restarted_after_416 = true;
                    ::unlink(job.temp.c_str());   // the range did not fit: start over once
                    continue;
                }
                return fail("protocol_error");
            }
            if (job.strict && resp.status == 412) return fail("changed_during_sync");
            if (resp.status != 200 && resp.status != 206) {
                bool handled = true;
                const Error e = session.status_error(resp, &handled);
                if (resp.status == 401) guard.record(make_error(ErrorClass::Auth, "auth_failed"));
                if (resp.status == 404) store_->set_server_missing(job.remote, true);
                ::unlink(job.temp.c_str());   // an error status leaves the partial file alone; it is useless after a final answer
                return fail(handled && !e.ok() ? e.key : std::string("protocol_error"));
            }
            guard.record(Error());

            // 200 or 206: the bytes are in the file
            const std::string etag_header = header_value(resp.headers, "ETag");
            if (job.strict && etag_header != job.planned_etag) return fail("changed_during_sync");   // a server that ignores If-Match is caught here
            job.recorded_etag = etag_header;
            int64_t total = -1;
            if (resp.status == 206) {
                int64_t start = -1;
                if (!parse_content_range(header_value(resp.headers, "Content-Range"), &start, &total) || start != offset) return fail("protocol_error");
            } else {
                const std::string cl = header_value(resp.headers, "Content-Length");
                if (!cl.empty()) total = std::atoll(cl.c_str());
            }
            if (strong(etag_header)) job.etag = etag_header;
            else job.etag.clear();
            if (total >= 0) job.expected = total;
            job.recorded_size = job.expected;
            store_->journal_set_expected(job.id, job.etag, job.expected);
        } else {
            job.recorded_etag = job.etag;
            job.recorded_size = job.expected;
        }

        // complete?
        const int64_t have = std::max<int64_t>(0, file_size(job.temp));
        if (job.expected && have != *job.expected) return fail("size_mismatch");
        break;
    }

    // the checks
    if (options_.hook) options_.hook("verify");
    if (const auto sum = strongest(parse_checksums(job.checksum))) {
        const auto digest = hash_file(job.temp, sum->algo, cancel);
        if (!digest.ok()) return digest.error.key == "cancelled" ? cancelled() : fail(digest.error.key);
        if (digest.value != sum->hex) return fail("checksum_mismatch");
        if (sum->algo == HashAlgo::Sha256) job.checksum_sha256 = digest.value;   // verified against the server: the record keeps it
    }
    // the file is on disk before it gets its name
    {
        const int fd = ::open(job.temp.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }
    // the copy must not have been changed meanwhile (also for a resume after a pause)
    if (job.policy == ConflictPolicy::Ask) {
        const FileStat now = stat_file(job.target);
        bool changed;
        if (job.pair_id > 0) {   // a pair's file: exactly the recorded copy, or nothing at all
            const auto srec = store_->sync_record(job.pair_id, job.rel_server);
            changed = now.exists && (!srec || !now.regular || now.size != srec->local_size.value_or(-1) || now.mtime != srec->local_mtime.value_or(-1));
        } else {
            const auto existing = store_->local_file(job.remote);
            changed = now.exists && (!existing || local_copy_changed(*existing, now));
        }
        if (changed) {
            abandon(*store_, job.id, job.temp);   // the server still has the bytes; the user's file is untouched
            return finish(end(JobState::Blocked, "local_changed"));
        }
    }
    if (::rename(job.temp.c_str(), job.target.c_str()) != 0) return fail(errno == ENOSPC || errno == EDQUOT ? "storage_full" : (errno == EROFS || errno == EACCES ? "storage_readonly" : "write_failed"));
    if (options_.hook) options_.hook("rename");

    const FileStat placed = stat_file(job.target);
    if (job.pair_id > 0 && job.track) {   // a pair's file: the record of the pair, advanced together with the end of the journal row
        SyncRecord rec;
        rec.pair_id = job.pair_id;
        rec.rel = job.rel_server;
        rec.local_rel = job.rel;
        rec.kind = Kind::File;
        rec.origin = "download";
        rec.server_etag = job.recorded_etag;
        rec.server_size = job.recorded_size;
        if (const auto sum = strongest(parse_checksums(job.checksum))) rec.server_checksum = std::string(sum->algo == HashAlgo::Sha256 ? "SHA256:" : sum->algo == HashAlgo::Sha1 ? "SHA1:" : "MD5:") + sum->hex;
        if (!job.copy_of.empty()) {   // the server's version added beside the reader's: it lives on the reader only and is never sent back
            const size_t slash = job.rel_server.rfind('/');   // the copy's name in the folder of the original, a name the server does not have
            rec.rel = (slash == std::string::npos ? std::string() : job.rel_server.substr(0, slash + 1)) + basename_of(job.rel);
            rec.origin = "conflict_copy";
            rec.one_sided = "reader";
            rec.copy_of = job.copy_of;
            rec.server_etag.clear();
            rec.server_size.reset();
            rec.server_checksum.clear();
        }
        rec.local_size = placed.size;
        rec.local_mtime = placed.mtime;
        rec.local_sha256 = job.checksum_sha256;
        rec.synced_at = clock_->now();
        const Error e = store_->sync_commit_download(job.pair_id, rec, job.id);
        if (!e.ok()) {
            log::event("download.fail", {log::text("cause", e.key)});
            return finish(end(JobState::Failed, e.key.c_str(), job.id));   // the file is in place; the journal row stays for the next start
        }
    } else if (job.track) {
        LocalFile lf;
        lf.remote_path = job.remote;
        lf.local_path = job.rel;
        lf.size = placed.size;
        lf.mtime = placed.mtime;
        lf.remote_etag = job.recorded_etag;
        lf.remote_size = job.recorded_size;
        lf.remote_mtime = job.remote_mtime;
        lf.downloaded_at = clock_->now();
        const Error e = store_->commit_download(lf, job.id);
        if (!e.ok()) {
            log::event("download.fail", {log::text("cause", e.key)});
            return finish(end(JobState::Failed, e.key.c_str(), job.id));   // the file is in place; the journal row stays for the next start
        }
    } else {
        store_->journal_end(job.id);
    }
    if (options_.hook) options_.hook("committed");
    log::event("download.done", {log::count("bytes", static_cast<long>(placed.size))});
    notify(on_progress, JobState::Done, placed.size, placed.size, 0);
    DownloadOutcome out;
    out.state = JobState::Done;
    out.local_path = job.rel;
    return finish(out);
}

void Downloader::recover() {
    std::vector<int64_t> known;
    for (const DownloadJournalRow& row : store_->journal()) {
        known.push_back(row.id);
        const std::string dir = dirname_of(row.target_path);
        const std::string base = row.pair_id > 0 && !row.target_root.empty() ? row.target_root : root_;
        const std::string folder = dir.empty() ? base : base + "/" + dir;
        const std::string temp = folder + "/.pbc-" + std::to_string(row.id) + ".part";
        const std::string target = base + "/" + row.target_path;
        if (stat_file(temp).exists) {
            if (row.state != "paused") store_->journal_update(row.id, std::max<int64_t>(0, file_size(temp)), "paused");
            continue;
        }
        if (row.pair_id > 0) {
            // The file was renamed into place and the process died before the record: the file is the finished download when it has the size that was
            // planned and is not the recorded copy it was to replace. Recording it here keeps the next comparison from seeing a stranger on the card.
            const FileStat placed = stat_file(target);
            const auto sp = store_->sync_pair(row.pair_id);
            const std::string prefix = sp ? (sp->remote_folder == "/" ? std::string("/") : sp->remote_folder + "/") : std::string();
            if (sp && placed.exists && placed.regular && row.expected_size && placed.size == *row.expected_size && strong(row.expected_etag) &&
                row.remote_path.size() > prefix.size() && row.remote_path.compare(0, prefix.size(), prefix) == 0) {
                const std::string rel_server = row.remote_path.substr(prefix.size());
                const auto old = store_->sync_record(row.pair_id, rel_server);
                const bool is_old_copy = old && old->local_size.value_or(-1) == placed.size && old->local_mtime.value_or(-1) == placed.mtime;
                if (!is_old_copy) {
                    SyncRecord rec;
                    rec.pair_id = row.pair_id;
                    rec.rel = rel_server;
                    rec.local_rel = row.target_path;
                    rec.kind = Kind::File;
                    rec.origin = "download";
                    rec.server_etag = row.expected_etag;
                    rec.server_size = row.expected_size;
                    if (const auto sum = strongest(parse_checksums(row.checksum)))
                        rec.server_checksum = std::string(sum->algo == HashAlgo::Sha256 ? "SHA256:" : sum->algo == HashAlgo::Sha1 ? "SHA1:" : "MD5:") + sum->hex;
                    rec.local_size = placed.size;
                    rec.local_mtime = placed.mtime;
                    rec.synced_at = clock_->now();
                    if (store_->sync_commit_download(row.pair_id, rec, row.id).ok()) continue;
                }
            }
            store_->journal_end(row.id);
            continue;
        }
        // no partial file: the job never wrote one, or the rename happened and the commit did not
        const FileStat placed = stat_file(target);
        const auto recorded = store_->local_file(row.remote_path);
        const bool is_new_file = placed.exists && placed.regular && row.expected_size && placed.size == *row.expected_size &&
                                 !(recorded && !local_copy_changed(*recorded, placed));
        if (is_new_file) {
            LocalFile lf;
            lf.remote_path = row.remote_path;
            lf.local_path = row.target_path;
            lf.size = placed.size;
            lf.mtime = placed.mtime;
            lf.remote_etag = row.expected_etag;
            lf.remote_size = row.expected_size;
            lf.downloaded_at = clock_->now();
            if (store_->commit_download(lf, row.id).ok()) continue;
        }
        store_->journal_end(row.id);
    }
    // partial files that no journal row knows
    struct Walk {
        static void run(const std::string& base, const std::string& rel, const std::vector<int64_t>& known, int depth) {
            if (depth > 64) return;
            DIR* d = ::opendir(rel.empty() ? base.c_str() : (base + "/" + rel).c_str());
            if (!d) return;
            while (dirent* e = ::readdir(d)) {
                const std::string n = e->d_name;
                if (n == "." || n == "..") continue;
                const std::string r = rel.empty() ? n : rel + "/" + n;
                struct stat st;
                if (::lstat((base + "/" + r).c_str(), &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) {
                    run(base, r, known, depth + 1);
                } else if (S_ISREG(st.st_mode) && n.compare(0, 5, ".pbc-") == 0 && n.size() > 10 && n.compare(n.size() - 5, 5, ".part") == 0) {
                    const std::string digits = n.substr(5, n.size() - 10);
                    bool numeric = !digits.empty();
                    for (char c : digits) numeric = numeric && c >= '0' && c <= '9';
                    const int64_t id = numeric ? std::atoll(digits.c_str()) : -1;
                    if (id < 0 || std::find(known.begin(), known.end(), id) == known.end()) ::unlink((base + "/" + r).c_str());
                }
            }
            ::closedir(d);
        }
    };
    Walk::run(root_, "", known, 0);
    // the folders of the pairs hold temporary files too (a pair's download that was cut short)
    std::vector<std::string> roots;
    for (const SyncPair& p : store_->sync_pairs()) roots.push_back(p.local_folder);
    for (const DownloadJournalRow& row : store_->journal())
        if (row.pair_id > 0 && !row.target_root.empty()) roots.push_back(row.target_root);
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    for (const std::string& r : roots) Walk::run(r, "", known, 0);
}

}  // namespace pbc
