#pragma once
// What the user's actions put into the queue (specs/002-manage-server-files): the logic behind Upload, folder picks, New folder, Rename,
// Move, Copy and Delete lives here, in the core, so that it is proven on the host (constitution Principle II); the bridge only wraps it.
// A call writes the queue and nothing else: no request is ever sent from here (FR-011). Qt-free.

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pbc/source_walk.h"
#include "pbc/store.h"
#include "pbc/types.h"

namespace pbc {

struct PickRefusal {
    std::string path;   // the picked path (or the entry of a picked folder) that was left out
    std::string key;    // source_not_allowed, source_unreadable, too_large_for_storage, name_forbidden
};

// A name that is taken (FR-021): the question the user answers before anything is queued for it. It holds everything its answer needs, so the
// caller keeps it as a plain value.
enum class AskKind { Upload, Move, Copy, Rename };

struct NameAsk {
    AskKind kind = AskKind::Upload;
    std::string source;                // an upload: the file on the reader; a move or copy: the remote path of the entry
    std::string target_folder;         // the server folder
    std::string name;                  // the name that is taken
    bool existing_is_folder = false;
    bool in_queue = false;             // the name is the target of an open item (it may or may not exist on the server as well)
    bool replace_possible = false;     // an existing file whose version the reader knows
    bool refresh_needed = false;       // an existing file, but its version is weak or unknown: refresh the folder first
    std::string existing_etag;
    std::optional<int64_t> existing_size, existing_mtime;
    std::string suggested;             // the keep-both name, free in the listing and the queue
};

enum class AskChoice { KeepBoth, Replace, Skip };
// One answer for every collision of a folder pick (the "apply to all" of the question).
enum class PickPolicy { Ask, KeepBoth, Replace, Skip };

struct AddResult {
    Error error;                       // a failure of the whole call: no account, a bad target, queue_full
    std::vector<int64_t> queued;       // ids of the items that were queued
    std::vector<PickRefusal> refused;  // picks that were left out, with the reason
    std::vector<NameAsk> asks;         // names that are taken: nothing is queued for them until they are answered
};

// A folder pick that has been counted but not yet queued (FR-003: the user confirms the numbers first).
struct PickedFolder {
    std::string canonical;             // the canonical path of the picked folder on the reader
    SourceWalk walk;
};
struct FolderPick {
    Error error;
    std::string target;                // the server folder it goes into
    std::vector<PickedFolder> folders;
    int folders_count = 0, files = 0;  // everything that would be created, picked folders included
    int64_t bytes = 0;
    int skipped = 0;                   // hidden entries, links, special and unreadable files the walk left out
    bool over_limit = false;           // a walk hit the item or depth limit
    std::vector<PickRefusal> refused;  // picks that were not usable
    int items() const { return folders_count + files; }
};

struct DeletePreview {
    bool offered = false;
    bool trash_known = false;          // the reader has learned from the server whether it has a trash (false: the account was never checked)
    int entries = 0, folders = 0, items_inside = 0;
    std::vector<std::string> names;    // the names of the first entries, for the dialog
};

class QueueActions {
public:
    QueueActions(Store& store, SourceRules rules) : store_(&store), rules_(std::move(rules)) {}

    // How many more items the queue takes.
    int headroom() const;

    // Files picked for a server folder: every file that passes the checks becomes one upload item; the others are refused one by one. A call
    // that does not fit the queue queues nothing (queue_full).
    AddResult add_uploads(const std::string& target_folder, const std::vector<std::string>& picked_files);

    // Folders picked for a server folder: the walk and the numbers, nothing queued.
    FolderPick count_folder_pick(const std::string& target_folder, const std::vector<std::string>& picked_folders) const;
    // The confirmed pick as one group: a new folder per folder (except those the stored listing shows on the server already) and an upload
    // per file, every folder before what it contains. Whole or not at all; over the walk limit or the queue headroom it is queue_full.
    // `policy` answers every file of the pick whose name is taken in an existing folder at once; Ask returns them as questions instead.
    AddResult confirm_folder_pick(const FolderPick& pick, PickPolicy policy = PickPolicy::Ask);

    // The user's answer to a question. Keep both: `new_name` ("" = the proposal) is checked like any new name and, when it is taken as well, comes back
    // as a new question; Replace: only a file over a file whose version is known (not_allowed for a folder, refresh_first for a weak or unknown one);
    // Skip queues nothing. The source is inspected again: it may have changed or vanished while the question was open.
    AddResult answer_ask(const NameAsk& ask, AskChoice choice, const std::string& new_name = std::string());

    // Move or copy entries of cached listings into `target_folder` (a folder of the server, possibly one that is still queued). Entries that cannot be
    // queued are left out with their reason in `refused` (refresh_first: no strong version; change_pending: a change waits for the entry; not_allowed:
    // a folder into itself or inside itself; name_exists: moving into the folder it is in); taken names come back as questions.
    AddResult move_or_copy(ItemKind kind, const std::vector<std::string>& remote_paths, const std::string& target_folder);

    // What the confirmation of a delete says: the entries, the folders among them, the names (at most the first few), and how many open items are about
    // anything inside a folder to be deleted (they fail with the folder). `offered` is false without a trash (research R-9).
    DeletePreview delete_preview(const std::vector<std::string>& remote_paths) const;
    // Delete entries of cached listings: only where the server has a trash (trash_unavailable otherwise, nothing queued); entries without a strong version
    // (refresh_first) or with a change waiting (change_pending) are left out with their reason. Nothing is sent.
    AddResult delete_entries(const std::vector<std::string>& remote_paths);

    // New folder: the name is checked like every new name, an existing or queued name queues nothing (name_exists: the page shows the existing
    // folder), a folder that is itself still queued is the parent the new one waits for.
    AddResult new_folder(const std::string& parent, const std::string& name);
    // Rename an entry of a cached listing: a strong version of the entry is needed (refresh_first) and the new name valid (name_forbidden). A taken name
    // comes back as a question (keep both, replace a file by a file, skip); the same name as the entry's own is name_exists. A case-only change is a different name. An entry that has an open item is not changed twice (not_allowed).
    AddResult rename_entry(const std::string& remote_path, const std::string& new_name);

    Error remove_item(int64_t id);

private:
    // The question for a taken name, or nullopt when the name is free (the cached listing and the open items are looked at, not the server).
    // `also_taken` holds names an earlier pick of the same call already claims.
    std::optional<NameAsk> ask_for(const std::string& source, const std::string& folder, const std::string& name, const ServerCaps& caps,
                                   const std::set<std::string>& also_taken) const;
    Store* store_;
    SourceRules rules_;
};

}  // namespace pbc
