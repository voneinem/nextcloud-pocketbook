#include "pbc/queue_sender.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>

#include "pbc/credentials_file.h"
#include "pbc/dav_write.h"
#include "pbc/error_keys.h"
#include "pbc/log.h"
#include "pbc/new_name.h"
#include "pbc/nc_listing.h"
#include "pbc/nc_server.h"
#include "pbc/remote_path.h"
#include "pbc/server_caps.h"
#include "pbc/upload_session.h"
#include "pbc/upload_source.h"

namespace pbc {

namespace {

constexpr int64_t kChunkSize = 10ll << 20;      // research R-4: a file up to this size is one PUT, a larger one goes in chunks of this size
constexpr int64_t kMinChunkSize = 5ll << 20;    // the protocol's minimum

// The account a run speaks to: the session carries the credentials to the transport only.
struct Link {
    AccountRow account;
    ServerCaps caps;
    std::unique_ptr<DavSession> session;
};

// "" = the link is open; else the key that stops the run (the account needs a new sign-in, or has no password).
std::string open_link(Store& store, HttpTransport& http, const Clock& clock, Link& link) {
    const auto account = store.account();
    if (!account || account->state == "needs_signin") return "auth_failed";
    const auto creds = load_credentials(store.directory());
    if (!creds.ok() || creds.value.password.empty()) return "auth_failed";
    link.account = *account;
    auto caps = store.server_caps();
    if (!caps) {
        // The capabilities were never learned (the account check at start runs only with Wi-Fi already on): the write side needs them (a trash for
        // Delete and Replace, chunking for resume), so one request asks now, on the user's own action. A failure leaves them unknown.
        const auto check = quick_check(http, account->base_url, account->login_name, creds.value.password, clock);
        if (check.ok()) {
            store.save_server_caps(check.value.caps);
            caps = check.value.caps;
        }
    }
    link.caps = caps.value_or(ServerCaps());
    link.session = std::make_unique<DavSession>(http, account->base_url, account->login_name, creds.value.password);
    return std::string();
}

std::string hex_id(Random& random) {
    uint8_t raw[16];
    random.bytes(raw, sizeof(raw));
    static const char* digits = "0123456789abcdef";
    std::string out = "pbc-";
    for (uint8_t b : raw) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The server reports the algorithms it computed ("SHA256:... MD5:..."); ours differs only when it names the same algorithm with another value.
bool checksum_differs(const std::string& reported, const std::string& sha256_hex) {
    if (sha256_hex.empty()) return false;
    const std::string r = lower(reported);
    const size_t at = r.find("sha256:");
    if (at == std::string::npos) return false;
    const size_t end = r.find(' ', at);
    return r.substr(at + 7, end == std::string::npos ? std::string::npos : end - at - 7) != lower(sha256_hex);
}

bool connection_error(const Error& e) { return e.cls == ErrorClass::Network || e.cls == ErrorClass::Tls; }

class Runner {
public:
    Runner(Store& store, HttpTransport& http, const Clock& clock, Random& random, const SourceRules& rules, const std::string& download_root,
           const SenderOptions& options, Cancel& cancel)
        : store_(store), http_(http), clock_(clock), random_(random), rules_(rules), download_root_(download_root), options_(options), cancel_(cancel) {}

    SendReport run(int64_t only) {
        const std::string link_key = open_link(store_, http_, clock_, link_);
        if (!link_key.empty()) {
            report_.stopped_by_key = link_key;
            return report_;
        }
        std::string stop;
        for (;;) {
            if (cancel_.requested()) {
                stop = "cancelled";
                break;
            }
            if (options_.wifi_up && !options_.wifi_up()) {
                stop = "no_connection";
                break;
            }
            std::optional<QueueItem> next;
            if (only != 0) {
                next = store_.queue_item(only);
                if (!next || next->state != ItemState::Queued || next->server != link_.account.base_url || next->user_id != link_.account.user_id) break;
                if (next->depends_on) {
                    const auto dep = store_.queue_item(*next->depends_on);
                    if (dep && dep->state != ItemState::Done) break;
                }
            } else {
                next = store_.queue_next(link_.account.base_url, link_.account.user_id);
            }
            if (!next) break;
            ++handled_;
            stop = send_item(*next);
            if (write_failed_) stop = "write_failed";
            if (!stop.empty() || only != 0) break;
        }
        report_.stopped_by_key = stop;
        report_.left = store_.queue_count_sendable(link_.account.base_url, link_.account.user_id);
        if (handled_ > 0 || !stop.empty()) remember();
        log::event("queue.send", {log::count("done", report_.done), log::count("failed", report_.failed), log::count("conflict", report_.conflict),
                                  log::count("unknown", report_.unknown), log::count("left", report_.left)});
        return report_;
    }

private:
    Store& store_;
    HttpTransport& http_;
    const Clock& clock_;
    Random& random_;
    const SourceRules& rules_;
    const std::string& download_root_;
    const SenderOptions& options_;
    Cancel& cancel_;
    Link link_;
    SendReport report_;
    int handled_ = 0;
    bool write_failed_ = false;
    bool quota_asked_ = false;
    std::optional<int64_t> free_bytes_;   // what the server said it has room for, less what this run has sent since (nullopt = unknown or unlimited)

    // The free space is asked once per run, when the first upload is about to be sent; a failure of that request is not fatal.
    void know_free_space() {
        if (quota_asked_) return;
        quota_asked_ = true;
        const auto free_space = fetch_free_bytes(session(), user());
        if (free_space.ok()) free_bytes_ = free_space.value;
    }

    void remember() {
        store_.set_setting("last_send_at", std::to_string(clock_.now()));
        store_.set_setting("last_send_counts", "{\"done\":" + std::to_string(report_.done) + ",\"failed\":" + std::to_string(report_.failed) +
                                                   ",\"conflict\":" + std::to_string(report_.conflict) + ",\"unknown\":" + std::to_string(report_.unknown) +
                                                   ",\"left\":" + std::to_string(report_.left) + "}");
    }

    void hook(const char* step) {
        if (options_.hook) options_.hook(step);
    }
    void progress(const QueueItem& it, const char* step, int64_t done, int64_t total) {
        if (!options_.on_progress) return;
        SendProgress p;
        p.item_id = it.id;
        p.state = it.state;
        p.step = step;
        p.done = done;
        p.total = total;
        options_.on_progress(p);
    }
    bool save(const QueueItem& it) {
        if (store_.queue_update(it).ok()) return true;
        write_failed_ = true;
        return false;
    }
    std::string user() const { return link_.account.user_id; }
    DavSession& session() { return *link_.session; }

    // ---- how an item ends ----
    void finish(QueueItem& it, ItemState state, const std::string& reason) {
        it.state = state;
        it.step.clear();
        it.reason = reason;
        save(it);
        switch (state) {
            case ItemState::Failed:
                ++report_.failed;
                log::event("upload.fail", {log::count("item", static_cast<long>(it.id))});
                break;
            case ItemState::Conflict: ++report_.conflict; break;
            case ItemState::Unknown:
                ++report_.unknown;
                log::event("upload.unknown", {log::count("item", static_cast<long>(it.id))});
                break;
            default: break;
        }
    }
    // Back to the queue untouched: nothing was changed on the server (or only what the next send finds again).
    void requeue(QueueItem& it, const std::string& reason) {
        it.state = ItemState::Queued;
        it.step.clear();
        it.reason = reason;
        save(it);
    }
    // What a failed request means for the item and for the run (research R-7, R-15). Returns the key that stops the run, or "".
    std::string end_with_error(QueueItem& it, const Error& error, bool outcome_unknown = false) {
        const std::string& key = error.key;
        if (outcome_unknown) {
            finish(it, ItemState::Unknown, "no_answer");
            return std::string();
        }
        if (key == "cancelled") {
            requeue(it, std::string());
            return key;
        }
        if (connection_error(error) || key == "no_connection" || key == "connection_lost") {
            requeue(it, key);
            return key;
        }
        if (key == "auth_failed") {
            requeue(it, std::string());
            return key;
        }
        if (key == "rate_limited" || key == "maintenance") {
            finish(it, ItemState::Failed, key);
            return key;
        }
        if (key == "name_exists" || key == "changed_on_server") {
            finish(it, ItemState::Conflict, key);
            return std::string();
        }
        finish(it, ItemState::Failed, key);
        return std::string();
    }
    std::string end_with_answer(QueueItem& it, const WriteAnswer& a) { return end_with_error(it, a.error, a.outcome_unknown); }

    // The server confirmed. The version and the file id come from the answer, else from one PROPFIND; the row and the cache change in one transaction.
    std::string confirm(QueueItem& it, std::string etag, std::optional<int64_t> fileid) {
        if (it.kind != ItemKind::Delete && (etag.empty() || !fileid)) {
            const auto seen = fetch_entry(session(), user(), join_remote(it.dst_parent, it.dst_name));
            if (seen.ok() && seen.value) {
                if (etag.empty()) etag = seen.value->etag;
                if (!fileid) fileid = seen.value->fileid;
            }
        }
        hook("confirm");
        ConfirmContext context;
        context.download_root = download_root_;
        if (it.kind == ItemKind::Upload) {   // the file as it is now: the link of a replace to the downloaded copy needs it unchanged since the start
            const auto now = inspect_source(it.src_local, rules_);
            if (now.ok()) {
                context.source_size = now.value.size;
                context.source_mtime = now.value.mtime;
            }
        }
        if (!store_.queue_confirm(it.id, etag, fileid, context).ok()) {
            write_failed_ = true;
            return "write_failed";
        }
        if (it.kind == ItemKind::Upload && free_bytes_) *free_bytes_ = std::max<int64_t>(0, *free_bytes_ - it.src_size.value_or(0));
        ++report_.done;
        log::event("upload.done", {log::count("item", static_cast<long>(it.id))});
        return std::string();
    }

    // ---- one item ----
    std::string send_item(QueueItem it) {
        it.state = ItemState::Sending;
        it.step = "prepare";
        it.reason.clear();
        if (!save(it)) return "write_failed";
        log::event("upload.start", {log::count("item", static_cast<long>(it.id))});
        hook("sending");
        progress(it, "prepare", 0, 0);
        switch (it.kind) {
            case ItemKind::Upload: return send_upload(it);
            case ItemKind::Mkdir: return send_mkdir(it);
            case ItemKind::Rename:
            case ItemKind::Move:
            case ItemKind::Copy: return send_entry_change(it);
            case ItemKind::Delete: return send_delete(it);
            default: finish(it, ItemState::Failed, "protocol_error"); return std::string();
        }
    }

    // Rename, move and copy: one request with the entry's version and Overwrite: F. A 412 is told apart by looking at both entries (research R-3).
    std::string send_entry_change(QueueItem& it) {
        if (it.replace) {   // replacing a file at the destination: its old version goes to the trash on its own version first (never Overwrite: T)
            const auto target = fetch_entry(session(), user(), join_remote(it.dst_parent, it.dst_name));
            if (!target.ok()) return end_with_error(it, target.error);
            std::string reason;
            const Target t = look_at_target(it, target.value, &reason);
            if (t == Target::Conflict || t == Target::Refused) {
                finish(it, t == Target::Conflict ? ItemState::Conflict : ItemState::Failed, reason);
                return std::string();
            }
            if (t == Target::ReplaceOld) {
                it.step = "request";
                if (!save(it)) return "write_failed";
                hook("delete_old");
                const WriteAnswer d = delete_entry(session(), user(), join_remote(it.dst_parent, it.dst_name), it.dst_etag, false);
                if (!d.error.ok()) return end_with_answer(it, d);
                it.dst_etag.clear();
            }
        }
        it.step = "request";
        if (!save(it)) return "write_failed";
        progress(it, "request", 0, 0);
        hook("request");
        const WriteAnswer a = it.kind == ItemKind::Copy ? copy_entry(session(), user(), it) : move_entry(session(), user(), it);
        if (a.ambiguous_412) {
            const auto target = fetch_entry(session(), user(), join_remote(it.dst_parent, it.dst_name));
            const auto source = fetch_entry(session(), user(), it.src_remote);
            if (!target.ok() || !source.ok()) return end_with_error(it, target.ok() ? source.error : target.error);
            switch (disambiguate_412(target.value, source.value, it.src_etag)) {
                case Cause412::NameExists: finish(it, ItemState::Conflict, "name_exists"); return std::string();
                case Cause412::SourceGone: finish(it, ItemState::Failed, "source_gone"); return std::string();
                case Cause412::ChangedOnServer: finish(it, ItemState::Conflict, "changed_on_server"); return std::string();
            }
        }
        if (!a.error.ok()) return end_with_answer(it, a);
        return confirm(it, a.etag, a.fileid);
    }

    // Delete: only where the server has a trash (an item queued earlier is held when the account check since then says otherwise), on the version the reader saw.
    std::string send_delete(QueueItem& it) {
        if (!delete_offered(link_.caps)) {
            finish(it, ItemState::Failed, "trash_unavailable");
            return std::string();
        }
        it.step = "request";
        if (!save(it)) return "write_failed";
        progress(it, "request", 0, 0);
        hook("request");
        const WriteAnswer a = delete_entry(session(), user(), it.src_remote, it.src_etag, it.entry_kind == Kind::Folder);
        if (!a.error.ok()) return end_with_answer(it, a);
        return confirm(it, std::string(), std::nullopt);
    }

    std::string send_mkdir(QueueItem& it) {
        it.step = "request";
        if (!save(it)) return "write_failed";
        progress(it, "request", 0, 0);
        hook("request");
        const WriteAnswer a = mkcol(session(), user(), join_remote(it.dst_parent, it.dst_name));
        if (!a.error.ok()) return end_with_answer(it, a);
        return confirm(it, a.etag, a.fileid);
    }

    // An upload: prepare (identity and checksum), then one PUT or the chunk sequence. A source that changes under an upload that is under way
    // starts that upload over once (research R-4.6); a second change fails the item.
    std::string send_upload(QueueItem& it) {
        for (int round = 0; round < 2; ++round) {
            bool restart = false;
            std::string stop;
            if (upload_round(it, round == 0, &restart, &stop) && restart) continue;
            return stop;
        }
        finish(it, ItemState::Failed, "source_changed");
        return std::string();
    }

    // Returns true when the work of this round is over; *restart tells the caller to run another round.
    bool upload_round(QueueItem& it, bool may_restart, bool* restart, std::string* stop) {
        const auto identity = inspect_source(it.src_local, rules_);
        if (!identity.ok()) {
            finish(it, ItemState::Failed, identity.error.key);
            return true;
        }
        if (!it.checksum.empty() && (it.src_size != identity.value.size || it.src_mtime != identity.value.mtime)) {
            // the file changed since it was hashed: whatever was sent belongs to other content
            discard_upload_folder(it);
            if (!may_restart) {
                finish(it, ItemState::Failed, "source_changed");
                return true;
            }
            reset_upload(it);
            it.reason = "source_changed";
        }
        it.src_size = identity.value.size;
        it.src_mtime = identity.value.mtime;
        if (it.checksum.empty()) {
            const auto digest = hash_source(it.src_local, cancel_, [&](long long done, long long total) { progress(it, "prepare", done, total); });
            if (!digest.ok()) {
                if (digest.error.key == "cancelled") {
                    requeue(it, std::string());
                    *stop = "cancelled";
                } else {
                    finish(it, ItemState::Failed, digest.error.key);
                }
                return true;
            }
            const auto again = inspect_source(it.src_local, rules_);
            if (!again.ok() || !same_identity(again.value, identity.value)) {
                finish(it, ItemState::Failed, "source_changed");
                return true;
            }
            it.checksum = digest.value;
        }
        if (!save(it)) {
            *stop = "write_failed";
            return true;
        }
        know_free_space();
        if (free_bytes_ && it.src_size.value_or(0) > *free_bytes_) {   // it cannot fit: said before any byte is sent (FR-038); the items that fit still go
            finish(it, ItemState::Failed, "server_storage_full");
            return true;
        }
        const bool chunked = link_.caps.chunking.value_or(false) && it.src_size.value_or(0) > kChunkSize;
        if (!chunked) {
            *stop = send_single(it);
            return true;
        }
        return send_chunked(it, identity.value, may_restart, restart, stop);
    }

    std::string send_single(QueueItem& it) {
        it.step = "single";
        if (!save(it)) return "write_failed";
        hook("request");
        const int64_t total = it.src_size.value_or(0);
        const WriteAnswer a = put_small(session(), user(), it, it.checksum, it.src_mtime.value_or(0), [&](long long done, long long) {
            progress(it, "single", done, total);
            return !cancel_.requested();
        });
        if (!a.error.ok()) return end_with_answer(it, a);
        return confirm(it, a.etag, a.fileid);
    }

    void reset_upload(QueueItem& it) {
        it.upload_id.clear();
        it.chunk_size.reset();
        it.checksum.clear();
    }

    // Best effort: the upload folder is the app's own temporary session; its 24-hour expiry is the backstop.
    void discard_upload_folder(const QueueItem& it) {
        if (it.upload_id.empty()) return;
        UploadSession gone(session(), user(), it.upload_id, join_remote(it.dst_parent, it.dst_name), ChunkPlan(), std::string(), 0);
        gone.abort();
    }

    // What the target at the final step means for an upload (research R-3, R-8): free, replaceable (the old version is still the one the user
    // confirmed), or a conflict. A replace whose old version is gone is a plain upload now: there is nothing left to overwrite.
    enum class Target { Free, ReplaceOld, Conflict, Refused };
    Target look_at_target(QueueItem& it, const std::optional<RemoteEntry>& entry, std::string* reason) {
        if (!it.replace) {
            if (entry) {
                *reason = "name_exists";
                return Target::Conflict;
            }
            return Target::Free;
        }
        if (!entry) {   // replaced meanwhile by a delete (ours or somebody's): what is asked for needs no overwrite
            it.dst_etag.clear();
            return Target::Free;
        }
        if (it.dst_etag.empty() || entry->kind == Kind::Folder) {   // the old version was removed by this item, and the name is taken again
            *reason = "name_exists";
            return Target::Conflict;
        }
        if (entry->etag != it.dst_etag) {
            *reason = "changed_on_server";
            return Target::Conflict;
        }
        if (link_.caps.trash != true) {   // the old version can only go the way of a delete, and a delete here would be permanent
            *reason = "trash_unavailable";
            return Target::Refused;
        }
        return Target::ReplaceOld;
    }

    bool send_chunked(QueueItem& it, const SourceIdentity& identity, bool may_restart, bool* restart, std::string* stop) {
        int64_t chunk_size = kChunkSize;
        if (link_.caps.max_chunk && *link_.caps.max_chunk < chunk_size) chunk_size = std::max(*link_.caps.max_chunk, kMinChunkSize);
        const bool fresh = it.upload_id.empty();
        if (!fresh && it.chunk_size) chunk_size = *it.chunk_size;   // a resume keeps the plan of the first send
        const auto plan = plan_chunks(identity.size, chunk_size);
        if (!plan) {
            finish(it, ItemState::Failed, "protocol_error");
            return true;
        }
        const std::string destination = join_remote(it.dst_parent, it.dst_name);
        if (fresh) {   // a name that is taken, or a version that changed, is found out before the first byte is sent, not after the last
            const auto taken = fetch_entry(session(), user(), destination);
            if (!taken.ok()) {
                *stop = end_with_error(it, taken.error);
                return true;
            }
            std::string reason;
            const Target t = look_at_target(it, taken.value, &reason);
            if (t == Target::Conflict || t == Target::Refused) {
                finish(it, t == Target::Conflict ? ItemState::Conflict : ItemState::Failed, reason);
                return true;
            }
            it.upload_id = hex_id(random_);
        }
        it.chunk_size = chunk_size;
        it.step = "chunks";
        if (!save(it)) {
            *stop = "write_failed";
            return true;
        }
        hook("upload_id");
        UploadSession up(session(), user(), it.upload_id, destination, *plan, it.checksum, it.src_mtime.value_or(0));

        std::set<int> good;
        bool need_open = fresh;
        if (!fresh) {
            const auto listed = up.list();
            if (!listed.ok()) {
                *stop = end_with_error(it, listed.error);
                return true;
            }
            if (listed.value.gone) need_open = true;
            else good = listed.value.good;
        }
        if (need_open) {
            const WriteAnswer a = up.open();
            if (!a.error.ok()) {
                *stop = end_with_answer(it, a);
                return true;
            }
        }

        int64_t sent = 0;
        for (int n = 1; n <= plan->count; ++n) {
            if (good.count(n)) {
                sent += plan->size_of(n);
                continue;
            }
            if (cancel_.requested()) {
                requeue(it, std::string());
                *stop = "cancelled";
                return true;
            }
            if (options_.wifi_up && !options_.wifi_up()) {
                requeue(it, "no_connection");
                *stop = "no_connection";
                return true;
            }
            if (source_moved(it, identity)) return restart_or_fail(it, may_restart, restart);
            hook("chunk");
            const int64_t base = sent;
            const WriteAnswer a = up.send_chunk(n, it.src_local, [&](long long done, long long) {
                progress(it, "chunks", base + done, plan->total);
                return !cancel_.requested();
            });
            if (!a.error.ok()) {
                *stop = end_with_answer(it, a);
                return true;
            }
            sent += plan->size_of(n);
            progress(it, "chunks", sent, plan->total);
        }

        if (source_moved(it, identity)) return restart_or_fail(it, may_restart, restart);
        // the pre-check of research R-3: the final MOVE is not known to honor a condition (LV-W6), so the target is looked at first, and a replace
        // never relies on the MOVE to overwrite: the old version is deleted on its own version (to the trash) and the MOVE is Overwrite: F
        const auto target = fetch_entry(session(), user(), destination);
        if (!target.ok()) {
            *stop = end_with_error(it, target.error);
            return true;
        }
        std::string reason;
        const Target t = look_at_target(it, target.value, &reason);
        if (t == Target::Conflict || t == Target::Refused) {
            finish(it, t == Target::Conflict ? ItemState::Conflict : ItemState::Failed, reason);
            return true;
        }
        it.step = "assemble";
        if (!save(it)) {
            *stop = "write_failed";
            return true;
        }
        progress(it, "assemble", plan->total, plan->total);
        if (t == Target::ReplaceOld) {
            hook("delete_old");
            const WriteAnswer d = delete_entry(session(), user(), destination, it.dst_etag, false);
            if (!d.error.ok()) {
                *stop = end_with_answer(it, d);
                return true;
            }
            it.dst_etag.clear();   // the old version is in the trash: from here on the name is free, and a resume finds it so
            if (!save(it)) {
                *stop = "write_failed";
                return true;
            }
        }
        hook("assemble");
        const WriteAnswer a = up.assemble(false, std::string());
        if (!a.error.ok()) {
            *stop = end_with_answer(it, a);
            return true;
        }
        std::string etag = a.etag;
        std::optional<int64_t> fileid = a.fileid;
        const auto made = fetch_entry(session(), user(), destination);
        if (made.ok() && made.value) {
            if (!made.value->size || *made.value->size != plan->total) {
                finish(it, ItemState::Failed, "size_mismatch");
                return true;
            }
            if (checksum_differs(made.value->checksum, it.checksum)) {
                finish(it, ItemState::Failed, "checksum_mismatch");
                return true;
            }
            if (!made.value->etag.empty()) etag = made.value->etag;
            if (made.value->fileid) fileid = made.value->fileid;
        }
        *stop = confirm(it, etag, fileid);
        return true;
    }

    bool source_moved(const QueueItem& it, const SourceIdentity& identity) {
        const auto now = inspect_source(it.src_local, rules_);
        return !now.ok() || !same_identity(now.value, identity);
    }
    // The source changed while its upload was under way.
    bool restart_or_fail(QueueItem& it, bool may_restart, bool* restart) {
        discard_upload_folder(it);
        if (!may_restart) {
            finish(it, ItemState::Failed, "source_changed");
            return true;
        }
        reset_upload(it);
        it.reason = "source_changed";
        it.step = "prepare";
        save(it);
        *restart = true;
        return true;
    }

public:
    // The choices for an item whose outcome is unknown share the link.
    Link& link() { return link_; }
    bool open() { return open_link(store_, http_, clock_, link_).empty(); }
};

}  // namespace

QueueSender::QueueSender(Store& store, HttpTransport& http, const Clock& clock, Random& random, SourceRules rules, std::string download_root,
                         SenderOptions options)
    : store_(store), http_(http), clock_(clock), random_(random), rules_(std::move(rules)), download_root_(std::move(download_root)), options_(std::move(options)) {}

SendReport QueueSender::run(Cancel& cancel, int64_t only) {
    Runner runner(store_, http_, clock_, random_, rules_, download_root_, options_, cancel);
    return runner.run(only);
}

Found QueueSender::check_unknown(int64_t id) {
    const auto item = store_.queue_item(id);
    if (!item || item->state != ItemState::Unknown) return Found::Unclear;
    Link link;
    if (!open_link(store_, http_, clock_, link).empty()) return Found::Unclear;
    return check_outcome(*link.session, link.account.user_id, *item);
}

Error QueueSender::mark_unknown_done(int64_t id) {
    const auto item = store_.queue_item(id);
    if (!item || item->state != ItemState::Unknown) return make_error(ErrorClass::Protocol, "protocol_error", "the item is not waiting for a decision");
    std::string etag;
    std::optional<int64_t> fileid;
    Link link;
    if (open_link(store_, http_, clock_, link).empty() && (item->kind == ItemKind::Upload || item->kind == ItemKind::Mkdir)) {
        // the version of what arrived, if it can be read now; the decision does not wait for it
        const auto seen = fetch_entry(*link.session, link.account.user_id, join_remote(item->dst_parent, item->dst_name));
        if (seen.ok() && seen.value) {
            etag = seen.value->etag;
            fileid = seen.value->fileid;
        }
    }
    ConfirmContext context;
    context.download_root = download_root_;
    context.source_size = item->src_size;
    context.source_mtime = item->src_mtime;
    return store_.queue_confirm(id, etag, fileid, context);
}

Error QueueSender::send_again(int64_t id) {
    auto item = store_.queue_item(id);
    if (!item || item->state != ItemState::Unknown) return make_error(ErrorClass::Protocol, "protocol_error", "the item is not waiting for a decision");
    item->state = ItemState::Queued;
    item->step.clear();
    item->reason.clear();
    return store_.queue_update(*item);
}

Error QueueSender::remove_item(int64_t id) {
    const auto item = store_.queue_item(id);
    if (!item) return Error();
    if (item->state == ItemState::Sending) return make_error(ErrorClass::Protocol, "protocol_error", "an item that is being sent cannot be removed");
    if (!item->upload_id.empty() && item->state != ItemState::Done) {
        Link link;
        if (open_link(store_, http_, clock_, link).empty()) {
            UploadSession gone(*link.session, link.account.user_id, item->upload_id, join_remote(item->dst_parent, item->dst_name), ChunkPlan(), std::string(), 0);
            gone.abort();   // best effort: the 24-hour expiry of the server is the backstop
        }
    }
    return store_.queue_remove(id);
}

Error QueueSender::resolve_keep_both(int64_t id, const std::string& new_name) {
    auto item = store_.queue_item(id);
    if (!item || item->state != ItemState::Conflict || item->kind == ItemKind::Delete)
        return make_error(ErrorClass::Protocol, "protocol_error", "the item is not a change that waits for a choice of name");
    const ServerCaps caps = store_.server_caps().value_or(ServerCaps());
    // the names the server has in the folder now, when it can be asked: the listing the reader has may be old
    std::set<std::string> on_server;
    {
        Link link;
        if (open_link(store_, http_, clock_, link).empty()) {
            const auto listing = fetch_listing(*link.session, link.account.user_id, item->dst_parent);
            if (listing.ok())
                for (const RemoteEntry& e : listing.value.entries) on_server.insert(e.name);
        }
    }
    const auto taken = [&](const std::string& name) { return on_server.count(name) == 1 || store_.name_use(item->dst_parent, name).any(); };
    std::string name = new_name;
    if (name.empty()) {
        name = keep_both_name(item->dst_name, [&](std::string_view n) { return taken(std::string(n)); }, caps);
        if (name.empty()) return error_from_key("name_forbidden");
    } else {
        if (!check_new_name(name, caps).ok) return error_from_key("name_forbidden");
        if (taken(name)) return error_from_key("name_exists");
    }
    item->dst_name = name;
    item->replace = false;
    item->dst_etag.clear();
    item->state = ItemState::Queued;
    item->step.clear();
    item->reason.clear();
    return store_.queue_update(*item);
}

Result<std::optional<RemoteEntry>> QueueSender::refresh_target(int64_t id) {
    Result<std::optional<RemoteEntry>> out;
    const auto item = store_.queue_item(id);
    if (!item) {
        out.error = make_error(ErrorClass::Protocol, "protocol_error", "the item does not exist");
        return out;
    }
    Link link;
    if (!open_link(store_, http_, clock_, link).empty()) {
        out.error = error_from_key("auth_failed");
        return out;
    }
    return fetch_entry(*link.session, link.account.user_id, join_remote(item->dst_parent, item->dst_name));
}

Error QueueSender::resolve_replace(int64_t id, const std::string& etag) {
    auto item = store_.queue_item(id);
    if (!item || item->state != ItemState::Conflict || item->kind == ItemKind::Delete || item->kind == ItemKind::Mkdir || item->entry_kind != Kind::File)
        return make_error(ErrorClass::Protocol, "protocol_error", "the item is not a file that waits for a choice about a taken name");
    if (etag.empty()) {   // nothing is there any more: a plain change
        item->replace = false;
        item->dst_etag.clear();
    } else {
        if (etag.compare(0, 2, "W/") == 0) return error_from_key("refresh_first");
        item->replace = true;
        item->dst_etag = etag;
    }
    item->state = ItemState::Queued;
    item->step.clear();
    item->reason.clear();
    return store_.queue_update(*item);
}

Result<std::optional<RemoteEntry>> QueueSender::refresh_source(int64_t id) {
    Result<std::optional<RemoteEntry>> out;
    const auto item = store_.queue_item(id);
    if (!item || item->src_remote.empty()) {
        out.error = make_error(ErrorClass::Protocol, "protocol_error", "the item has no entry on the server");
        return out;
    }
    Link link;
    if (!open_link(store_, http_, clock_, link).empty()) {
        out.error = error_from_key("auth_failed");
        return out;
    }
    return fetch_entry(*link.session, link.account.user_id, item->src_remote);
}

Error QueueSender::resolve_apply_anyway(int64_t id, const std::string& etag) {
    auto item = store_.queue_item(id);
    if (!item || item->state != ItemState::Conflict || item->src_remote.empty())
        return make_error(ErrorClass::Protocol, "protocol_error", "the item is not a change of an entry that waits for a choice");
    if (etag.empty() || etag.compare(0, 2, "W/") == 0) return error_from_key("refresh_first");
    item->src_etag = etag;
    item->state = ItemState::Queued;
    item->step.clear();
    item->reason.clear();
    return store_.queue_update(*item);
}

Error QueueSender::resolve_retarget(int64_t id, const std::string& new_parent) { return store_.queue_retarget(id, new_parent); }

Error QueueSender::resolve_discard(int64_t id) { return remove_item(id); }

Error QueueSender::resolve_edit(int64_t id, const std::string& new_name) {
    auto item = store_.queue_item(id);
    if (!item || (item->state != ItemState::Failed && item->state != ItemState::Conflict) || item->kind == ItemKind::Delete)
        return make_error(ErrorClass::Protocol, "protocol_error", "the item cannot be given another name");
    const ServerCaps caps = store_.server_caps().value_or(ServerCaps());
    if (!check_new_name(new_name, caps).ok) return error_from_key("name_forbidden");
    if (new_name != item->dst_name && store_.name_use(item->dst_parent, new_name).any()) return error_from_key("name_exists");
    item->dst_name = new_name;
    item->replace = false;   // another name: nothing is replaced
    item->dst_etag.clear();
    item->state = ItemState::Queued;
    item->step.clear();
    item->reason.clear();
    return store_.queue_update(*item);
}

}  // namespace pbc
