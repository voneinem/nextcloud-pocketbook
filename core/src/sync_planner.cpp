#include "pbc/sync_planner.h"

#include <algorithm>
#include <map>
#include <set>

#include "pbc/conflict_name.h"
#include "pbc/local_name.h"
#include "pbc/new_name.h"
#include "pbc/remote_path.h"
#include "pbc/storage_info.h"
#include "pbc/sync_rules.h"
#include "pbc/utf8.h"

namespace pbc {

namespace {

bool strong_etag(const std::string& etag) { return !etag.empty() && etag.compare(0, 2, "W/") != 0; }

std::string parent_rel(const std::string& rel) {
    const size_t slash = rel.rfind('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

std::string base_name(const std::string& rel) {
    const size_t slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

// a component that starts with a dot: the scan of the reader skips such names, so a file with one would look as if it had vanished from the card
bool hidden_component(const std::string& rel) {
    size_t pos = 0;
    while (pos <= rel.size()) {
        if (pos < rel.size() && rel[pos] == '.') return true;
        const size_t slash = rel.find('/', pos);
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return false;
}

// every name of a relative path passes the server's rules for new names
bool server_names_ok(const std::string& rel, const ServerCaps& caps) {
    size_t pos = 0;
    while (pos <= rel.size()) {
        const size_t slash = rel.find('/', pos);
        const std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!check_new_name(part, caps).ok) return false;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

class Planner {
public:
    explicit Planner(const PlannerInput& in) : in_(in), to_reader_(direction_allows_to_reader(in.pair.direction)), to_server_(direction_allows_to_server(in.pair.direction)) {}

    SyncPlan run() {
        index();
        std::set<std::string> names;
        for (const auto& kv : sfile_) names.insert(kv.first);
        for (const auto& kv : rec_) names.insert(kv.first);
        for (const auto& kv : sfolder_)
            if (!kv.second->skip_reason.empty()) names.insert(kv.first);   // a folder the user cannot use is reported like a file
        for (const auto& kv : rfile_)
            if (!record_locals_.count(kv.first)) names.insert(kv.first);
        folder_proposals();
        for (const std::string& n : names) decide(n);

        std::stable_sort(plan_.entries.begin(), plan_.entries.end(), [](const PlanEntry& a, const PlanEntry& b) { return a.rel < b.rel; });   // folders were added first
        plan_.pair_id = in_.pair.id;
        plan_.incomplete = in_.server.incomplete || in_.reader.incomplete;
        plan_.incomplete_folders = in_.server.folders_not_listed + in_.reader.folders_not_listed;
        space();
        limits();
        totals();
        return std::move(plan_);
    }

private:
    void index() {
        for (const SyncRecord& r : in_.records) {
            rec_[r.rel] = &r;
            record_locals_.insert(r.local_rel);
        }
        for (const ScanEntry& e : in_.server.entries) (e.kind == Kind::Folder ? sfolder_ : sfile_)[e.rel] = &e;
        for (const ScanEntry& e : in_.reader.entries) (e.kind == Kind::Folder ? rfolder_ : rfile_)[e.rel] = &e;
        for (const SyncRecord& r : in_.records) pairs_.push_back({join_remote("/", r.rel), r.local_rel});
        taken_ = build_taken(pairs_);
        // the names a conflict copy must not take: on the reader the files and folders, the records' reader names; on the server the listing, the records
        // and the names with an open queue item (compared without regard to case: either side may sit on a case-insensitive storage)
        for (const ScanEntry& e : in_.reader.entries) reader_names_.insert(case_fold(e.rel));
        for (const ScanEntry& e : in_.server.entries) server_names_.insert(case_fold(e.rel));
        for (const SyncRecord& r : in_.records) {
            reader_names_.insert(case_fold(r.local_rel));
            server_names_.insert(case_fold(r.rel));
            if (!r.copy_of.empty()) copies_[r.copy_of].insert(r.one_sided);
        }
        for (const std::string& n : in_.open_rels) server_names_.insert(case_fold(n));
    }

    PlanEntry& add(PlanAction action, const std::string& rel, const std::string& local_rel, const std::string& reason, std::optional<int64_t> size) {
        PlanEntry e;
        e.rel = rel;
        e.local_rel = local_rel;
        e.action = action;
        e.reason = reason;
        e.size = size;
        plan_.entries.push_back(std::move(e));
        return plan_.entries.back();
    }
    void skip(const std::string& rel, const std::string& local_rel, const std::string& reason, std::optional<int64_t> size = std::nullopt) {
        add(PlanAction::Skip, rel, local_rel, reason, size);
    }

    void decide(const std::string& n) {
        if (below_proposed_folder_.count(n)) return;   // the folder is one proposal: its files are not listed on their own
        const SyncRecord* R = rec_.count(n) ? rec_[n] : nullptr;
        const ScanEntry* S = sfile_.count(n) ? sfile_[n] : nullptr;
        const std::string ln = R ? R->local_rel : n;
        const ScanEntry* L = rfile_.count(ln) ? rfile_[ln] : nullptr;
        if (!S && !R && !L) {
            if (sfolder_.count(n) && !sfolder_[n]->skip_reason.empty()) skip(n, n, sfolder_[n]->skip_reason);   // row 2 for a folder
            return;
        }
        const std::optional<int64_t> shown = S ? std::optional<int64_t>(S->size) : (L ? std::optional<int64_t>(L->size) : std::nullopt);

        // row 1: a name with an open queue item is left alone, whatever else is true
        if (in_.open_rels.count(n)) {
            skip(n, ln, "change_pending", shown);
            return;
        }
        // row 2: the server does not let the user use the entry
        if (S && !S->skip_reason.empty()) {
            skip(n, ln, S->skip_reason, S->size);
            return;
        }
        // row 3: a reader entry that is not a regular, readable file (or collides with another by case); a stray one is only reported when it could be sent
        if (L && !L->skip_reason.empty()) {
            if (S || R || to_server_) skip(n, ln, L->skip_reason, L->size);
            return;
        }
        // row 21: a conflict copy that lives on one side only is never sent to the other one and never proposed for deletion
        if (R && !R->one_sided.empty()) {
            const bool holder = R->one_sided == "reader" ? L != nullptr : S != nullptr;
            const bool other = R->one_sided == "reader" ? S != nullptr : L != nullptr;
            if (holder && !other) {
                skip(n, ln, "kept_both", shown);
                return;
            }
            if (holder && other) R = nullptr;   // the user put it on the other side as well: an ordinary name seen for the first time
        }
        // a name that is a folder on the other side cannot become a file
        if (S && rfolder_.count(ln)) {
            skip(n, ln, "name_exists", S->size);
            return;
        }
        if (L && !S && sfolder_.count(n)) {
            if (to_server_) skip(n, ln, "name_exists", L->size);
            return;
        }

        if (S && L && !R) {   // rows 8 and 9: the same name on both sides, never seen by a sync before
            if (S->size == L->size && !S->checksum.empty() && in_.identical && in_.identical(*S, *L)) {
                PlanEntry& e = add(PlanAction::Skip, n, ln, "identical", S->size);   // the record is written when the plan is approved
                e.plan_etag = S->etag;
                e.plan_lsize = L->size;
                e.plan_lmtime = L->mtime;
            } else {
                keep_both(n, ln, *S, *L, nullptr);   // row 9: a clash at the first sync: each side gets the other's version under a new name
            }
            return;
        }
        if (S && !L && !R) {   // row 6
            if (to_reader_) download(n, *S, nullptr, nullptr);
            return;
        }
        if (!S && L && !R) {   // row 7
            if (to_server_) upload(n, *L, nullptr);
            return;
        }
        if (S && L && R && R->diverged) {   // row 10: the two versions were kept apart; only a copy that is still missing is made
            keep_both(n, ln, *S, *L, R);
            return;
        }
        if (S && L && R) {   // rows 11 to 14
            const bool sc = server_changed(*S, *R);
            const std::string abs = join_path_checked(in_.pair.local_folder, ln);
            const bool lc = abs.empty() || (in_.reader_changed ? in_.reader_changed(abs, *R) : true);
            if (!sc && !lc) {   // row 11
                ++plan_.totals.n_in_sync;
            } else if (sc && !lc) {   // row 12
                if (to_reader_) download(n, *S, R, L);
                else skip(n, ln, "direction", S->size);
            } else if (!sc && lc) {   // row 13
                if (to_server_) upload(n, *L, R);
                else skip(n, ln, "direction", L->size);
            } else {   // row 14: changed on both sides: each side gets the other's version under a new name
                keep_both(n, ln, *S, *L, R);
            }
            return;
        }
        if (R && !S && L && !in_.server.incomplete) {   // rows 15 and 16: gone from the server; the reader still has it
            const std::string abs = join_path_checked(in_.pair.local_folder, ln);
            const bool lc = abs.empty() || (in_.reader_changed ? in_.reader_changed(abs, *R) : true);
            if (R->quiet && !lc) {   // row 20: the user chose to leave it; proposed again only when the present file changes
                ++plan_.totals.n_in_sync;
                return;
            }
            PlanEntry& e = add(PlanAction::ProposeDeleteReader, n, ln, std::string(), L->size);
            e.plan_lsize = L->size;
            e.plan_lmtime = L->mtime;
            e.allowed.insert(Choice::Leave);
            if (to_server_) e.allowed.insert(Choice::TransferAgain);        // send it to the server again
            if (to_reader_ && !lc) e.allowed.insert(Choice::Delete);        // a changed reader file is never offered for deletion
            return;
        }
        if (R && S && !L && !in_.reader.incomplete) {   // rows 17 and 18: gone from the reader; the server still has it
            const bool sc = server_changed(*S, *R);
            if (R->quiet && !sc) {   // row 20
                ++plan_.totals.n_in_sync;
                return;
            }
            PlanEntry& e = add(PlanAction::ProposeDeleteServer, n, ln, std::string(), S->size);
            e.plan_etag = S->etag;
            e.plan_checksum = S->checksum;
            e.allowed.insert(Choice::Leave);
            if (to_reader_) e.allowed.insert(Choice::TransferAgain);        // copy it back to the reader
            if (to_server_ && !sc && delete_offered(in_.caps) && strong_etag(S->etag)) e.allowed.insert(Choice::Delete);   // the trash, conditional on the version the record knows
            return;
        }
        if (R && !S && !L) {   // row 19: gone on both sides; the record goes when the plan is approved - only when both scans saw everything
            if (!in_.server.incomplete && !in_.reader.incomplete) skip(n, ln, "gone_both");
            return;
        }
        // a name that is on one side only while the other side's scan is incomplete: unknown is not missing, nothing is planned (FR-029)
    }

    // A folder that is gone from one side as a whole, with synced files below it, is ONE proposal naming how many synced files are in it (R-10 rule 3); its
    // files are not listed on their own. The choices: leave, transfer again (the synced files below it, as the record knows them), or delete (the files that are unchanged, at the confirmation).
    void folder_proposals() {
        std::map<std::string, std::vector<const SyncRecord*>> below;   // a folder -> the records anywhere below it
        for (const SyncRecord& r : in_.records) {
            if (r.rel != r.local_rel || !r.one_sided.empty()) continue;   // only ordinary names that are the same on both sides
            for (std::string dir = parent_rel(r.rel); !dir.empty(); dir = parent_rel(dir)) below[dir].push_back(&r);
        }
        std::vector<std::string> folders;
        for (const auto& kv : below) folders.push_back(kv.first);
        std::sort(folders.begin(), folders.end(), [](const std::string& a, const std::string& b) {
            const auto depth = [](const std::string& x) { return std::count(x.begin(), x.end(), '/'); };
            return depth(a) != depth(b) ? depth(a) < depth(b) : a < b;
        });
        for (const std::string& dir : folders) {
            bool inside = false;
            for (std::string up = parent_rel(dir); !up.empty(); up = parent_rel(up)) inside = inside || proposed_folders_.count(up) == 1;
            if (inside) continue;   // a folder above it is proposed already
            const auto& recs = below[dir];
            bool all_quiet = true;
            for (const SyncRecord* r : recs) all_quiet = all_quiet && r->quiet;
            if (all_quiet) continue;   // the user chose to leave it: its files are judged one by one, and are quiet
            const bool on_server = sfolder_.count(dir) == 1, on_reader = rfolder_.count(dir) == 1;
            bool server_has_any = false, reader_has_any = false;
            for (const SyncRecord* r : recs) {
                server_has_any = server_has_any || sfile_.count(r->rel) == 1;
                reader_has_any = reader_has_any || rfile_.count(r->local_rel) == 1;
            }
            if (!on_server && on_reader && !server_has_any && reader_has_any && !in_.server.incomplete) {
                add_folder_proposal(PlanAction::ProposeDeleteReader, dir, recs);
            } else if (!on_reader && on_server && !reader_has_any && server_has_any && !in_.reader.incomplete) {
                add_folder_proposal(PlanAction::ProposeDeleteServer, dir, recs);
            }
        }
    }
    void add_folder_proposal(PlanAction action, const std::string& dir, const std::vector<const SyncRecord*>& recs) {
        PlanEntry& e = add(action, dir, dir, std::string(), std::nullopt);
        e.kind = Kind::Folder;
        e.records_below = static_cast<int>(recs.size());
        e.allowed.insert(Choice::Leave);
        if (action == PlanAction::ProposeDeleteReader) {
            if (to_reader_) e.allowed.insert(Choice::Delete);
            if (to_server_) e.allowed.insert(Choice::TransferAgain);   // send the synced files below it to the server again
        } else {
            e.plan_etag = sfolder_[dir]->etag;
            if (to_server_ && delete_offered(in_.caps) && strong_etag(e.plan_etag)) e.allowed.insert(Choice::Delete);
            if (to_reader_) e.allowed.insert(Choice::TransferAgain);   // copy the synced files below it back to the reader
        }
        proposed_folders_.insert(dir);
        for (const SyncRecord* r : recs) below_proposed_folder_.insert(r->rel);
        below_proposed_folder_.insert(dir);
    }

    void download(const std::string& n, const ScanEntry& s, const SyncRecord* R, const ScanEntry* L) {
        if (s.size > kFatMaxFileBytes) {   // row 5
            skip(n, R ? R->local_rel : n, "too_large_for_storage", s.size);
            return;
        }
        std::string ln;
        if (R) {
            ln = R->local_rel;
        } else if (hidden_component(n)) {   // row 4: the reader would never see it again
            skip(n, n, "hidden_name", s.size);
            return;
        } else {   // row 4: a name the card can take, free in the records and in this plan
            const Result<std::string> chosen = choose_local_path(in_.pair.local_folder, join_remote("/", n), taken_);
            if (!chosen.ok()) {
                skip(n, n, chosen.error.key, s.size);
                return;
            }
            ln = chosen.value;
            const TakenMap more = build_taken({{join_remote("/", n), ln}});
            taken_.insert(more.begin(), more.end());
        }
        PlanEntry& e = add(PlanAction::Download, n, ln, std::string(), s.size);
        e.plan_etag = s.etag;
        e.plan_checksum = s.checksum;
        if (L) {
            e.plan_lsize = L->size;
            e.plan_lmtime = L->mtime;
        }
    }

    void upload(const std::string& n, const ScanEntry& l, const SyncRecord* R) {
        const std::string ln = R ? R->local_rel : n;
        if (l.size > kFatMaxFileBytes) {   // row 5
            skip(n, ln, "too_large_for_storage", l.size);
            return;
        }
        if (l.size == 0) {
            skip(n, ln, "source_unreadable", 0);
            return;
        }
        if (!server_names_ok(n, in_.caps)) {   // row 4
            skip(n, ln, "name_forbidden", l.size);
            return;
        }
        if (R && !strong_etag(R->server_etag)) {   // row 13: a replace of the server file needs a strong version to be conditional on
            skip(n, ln, "server_version_unknown", l.size);
            return;
        }
        if (in_.quota_free && l.size > *in_.quota_free) {
            skip(n, ln, "quota_exceeded", l.size);
            return;
        }
        PlanEntry& e = add(PlanAction::Upload, n, ln, std::string(), l.size);
        e.plan_etag = R ? R->server_etag : std::string();
        e.plan_lsize = l.size;
        e.plan_lmtime = l.mtime;
    }

    // Rows 9, 10 and 14 (research R-9): the original names keep their own side's version; each side the direction allows gets the other's version under
    // a name that says where it came from. The names are chosen here and stored, so that the same plan names the same copies; copies that exist from an
    // earlier, interrupted run (records with copy_of) are not made again.
    bool has_copy(const std::string& n, const char* side) const {
        const auto it = copies_.find(n);
        return it != copies_.end() && it->second.count(side) == 1;
    }
    std::string pick_name(const std::string& base_rel, bool server_copy, const std::set<std::string>& names, std::set<std::string>* claimed) {
        const std::string dir = parent_rel(base_rel);
        const std::string name = conflict_name(base_name(base_rel), server_copy, in_.reader_date, [&](std::string_view candidate) {
            const std::string folded = case_fold(rel_join(dir, std::string(candidate)));
            return names.count(folded) == 1 || claimed->count(folded) == 1;
        });
        if (name.empty()) return std::string();
        const std::string full = rel_join(dir, name);
        claimed->insert(case_fold(full));
        return full;
    }
    void keep_both(const std::string& n, const std::string& ln, const ScanEntry& S, const ScanEntry& L, const SyncRecord* R) {
        const bool need_reader = to_reader_ && !has_copy(n, "reader");
        const bool need_server = to_server_ && !has_copy(n, "server");
        if (!need_reader && !need_server) {   // both copies exist (or nothing may be copied): what is left is to remember that the two versions are apart
            PlanEntry& e = add(PlanAction::Skip, n, ln, "kept_both", S.size);
            if (!(R && R->diverged)) {
                e.plan_etag = S.etag;
                e.plan_checksum = S.checksum;
                e.plan_lsize = L.size;
                e.plan_lmtime = L.mtime;
            }
            return;
        }
        if (need_reader && S.size > kFatMaxFileBytes) {
            skip(n, ln, "too_large_for_storage", S.size);
            return;
        }
        if (need_server) {
            if (L.size > kFatMaxFileBytes) {
                skip(n, ln, "too_large_for_storage", L.size);
                return;
            }
            if (L.size == 0) {
                skip(n, ln, "source_unreadable", 0);
                return;
            }
            if (in_.quota_free && L.size > *in_.quota_free) {
                skip(n, ln, "quota_exceeded", L.size);
                return;
            }
        }
        std::string reader_copy, server_copy;
        if (need_reader) {
            reader_copy = pick_name(ln, true, reader_names_, &claimed_reader_);
            if (reader_copy.empty() || join_path_checked(in_.pair.local_folder, reader_copy).empty()) {
                skip(n, ln, "path_too_long", S.size);
                return;
            }
        }
        if (need_server) {
            server_copy = pick_name(n, false, server_names_, &claimed_server_);
            if (server_copy.empty() || !server_names_ok(server_copy, in_.caps)) {
                skip(n, ln, "name_forbidden", L.size);
                return;
            }
        }
        PlanEntry& e = add(PlanAction::KeepBoth, n, ln, std::string(), S.size);
        e.plan_etag = S.etag;
        e.plan_checksum = S.checksum;
        e.plan_lsize = L.size;
        e.plan_lmtime = L.mtime;
        e.conflict_name = reader_copy;
        e.conflict_name_server = server_copy;
    }

    // The free space as far as the plan goes: a download that would take it below the reserve cannot be approved as it is.
    void space() {
        plan_.free_bytes = in_.free_bytes;
        plan_.quota_free = in_.quota_free;
        plan_.reserve_bytes = reserve_for(in_.volume_bytes);
        if (!in_.free_bytes) return;
        const int64_t usable = *in_.free_bytes - plan_.reserve_bytes;
        int64_t added = 0;
        for (PlanEntry& e : plan_.entries) {
            if (e.action != PlanAction::Download && !(e.action == PlanAction::KeepBoth && !e.conflict_name.empty())) continue;
            const int64_t size = e.size.value_or(0);
            if (added + size > usable) e.unapprovable = "no_space";
            else added += size;
        }
    }

    // What a plan adds to the reader (downloads and the reader-side conflict copies) against the pair's limits (R-11). Only a plan that adds something can be over
    // them: a folder that holds more than a lowered limit is never an error by itself. The system never picks entries to make a plan fit.
    void limits() {
        int64_t files = 0, bytes = 0;
        int adds = 0;
        for (const PlanEntry& e : plan_.entries) {
            if (e.action != PlanAction::Download && !(e.action == PlanAction::KeepBoth && !e.conflict_name.empty())) continue;
            ++adds;
            if (e.action == PlanAction::Download && e.plan_lsize) {   // a newer version of a file the reader holds already: no new file, only the difference in size
                bytes += e.size.value_or(0) - *e.plan_lsize;
                continue;
            }
            ++files;
            bytes += e.size.value_or(0);
        }
        if (adds == 0) return;
        const bool over_files = in_.held_files + files > in_.pair.max_files;
        const bool over_bytes = in_.held_bytes + bytes > in_.pair.max_bytes;
        plan_.over_limit = over_files && over_bytes ? "both" : over_files ? "files" : over_bytes ? "bytes" : "";
    }

    void totals() {
        SyncPlan::Totals& t = plan_.totals;
        std::set<std::string> new_folders;
        for (size_t i = 0; i < plan_.entries.size(); ++i) {
            PlanEntry& e = plan_.entries[i];
            e.seq = static_cast<int>(i) + 1;
            switch (e.action) {
                case PlanAction::Download: ++t.n_download; t.b_download += e.size.value_or(0); break;
                case PlanAction::Upload: ++t.n_upload; t.b_upload += e.size.value_or(0); break;
                case PlanAction::Skip:
                    if (e.reason != "gone_both") ++t.n_skip;
                    break;
                case PlanAction::KeepBoth: ++t.n_keep_both; t.b_keep_both += e.size.value_or(0); break;
                case PlanAction::ProposeDeleteServer: ++t.n_prop_server; break;
                case PlanAction::ProposeDeleteReader: ++t.n_prop_reader; break;
            }
            if (!e.unapprovable.empty()) ++plan_.unapprovable;
            // the folders a transfer would have to create on the receiving side
            if (e.action == PlanAction::Download) {
                for (std::string dir = parent_rel(e.local_rel); !dir.empty(); dir = parent_rel(dir))
                    if (!rfolder_.count(dir)) new_folders.insert("r:" + dir);
            } else if (e.action == PlanAction::Upload) {
                for (std::string dir = parent_rel(e.rel); !dir.empty(); dir = parent_rel(dir))
                    if (!sfolder_.count(dir)) new_folders.insert("s:" + dir);
            }
        }
        t.folders_new = static_cast<int>(new_folders.size());
        if (plan_.entries.size() > static_cast<size_t>(kSyncMaxPlanEntries)) {   // too much to review at once: the totals stay, the entries go
            plan_.truncated = true;
            plan_.entries.clear();
            plan_.unapprovable = 0;
        }
    }

    const PlannerInput& in_;
    const bool to_reader_, to_server_;
    SyncPlan plan_;
    std::map<std::string, const SyncRecord*> rec_;
    std::map<std::string, const ScanEntry*> sfile_, sfolder_, rfile_, rfolder_;
    std::set<std::string> record_locals_;
    std::vector<std::pair<std::string, std::string>> pairs_;
    TakenMap taken_;
    std::set<std::string> reader_names_, server_names_, claimed_reader_, claimed_server_;   // folded names a conflict copy must not take
    std::set<std::string> proposed_folders_, below_proposed_folder_;   // folders that are one proposal each; the names below them
    std::map<std::string, std::set<std::string>> copies_;                                  // original rel -> the sides that hold a copy of it
};

}  // namespace

SyncPlan plan(const PlannerInput& input) { return Planner(input).run(); }

Approval can_approve(const SyncPlan& plan, int64_t now) {
    Approval a;
    auto refuse = [&](const char* key) {
        a.ok = false;
        a.error_key = key;
        return a;
    };
    if (plan.state != "ready") return refuse("plan_not_approvable");
    if (now - plan.created_at > kSyncPlanMaxAgeS) return refuse("plan_stale");
    if (plan.truncated) return refuse("plan_too_large");
    if (!plan.over_limit.empty()) return refuse("over_limit");
    if (plan.unapprovable > 0) return refuse("plan_not_approvable");
    if (plan.incomplete && plan.totals.n_prop_server + plan.totals.n_prop_reader > 0) return refuse("plan_not_approvable");   // a proposal needs a complete comparison
    a.ok = true;
    return a;
}

}  // namespace pbc
