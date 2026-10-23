#pragma once
// The queue of changes the user made on the reader (specs/002-manage-server-files/data-model.md "queue_items"): the item model, its states
// and the rules that belong to the model itself. Rows are durable user data (constitution Principle III): written before any network
// request, never discarded or overwritten until the server has confirmed them or the user has removed them. Qt-free.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pbc/store_files.h"

namespace pbc {

enum class ItemKind { Upload, Mkdir, Rename, Move, Copy, Delete };

// Queued = "not synced". Sending exists for one item at a time. Failed, Conflict and Unknown are held: a run never takes them, only the
// user's tap (Retry, a choice, Check the server) moves them on.
enum class ItemState { Queued, Sending, Done, Failed, Conflict, Unknown };

const char* item_kind_text(ItemKind kind);
std::optional<ItemKind> parse_item_kind(std::string_view text);
const char* item_state_text(ItemState state);
std::optional<ItemState> parse_item_state(std::string_view text);

// The sender's transitions (data-model.md state machine). The store itself does not enforce them: it keeps what it is told, validated.
bool transition_allowed(ItemState from, ItemState to);
bool is_held(ItemState state);

inline constexpr int kMaxQueueItems = 2000;

struct QueueItem {
    int64_t id = 0;
    std::optional<int64_t> group_id;   // rows of one pick or one multi-entry action
    ItemKind kind = ItemKind::Upload;
    Kind entry_kind = Kind::File;      // what is uploaded, created, renamed, moved, copied or deleted
    ItemState state = ItemState::Queued;
    std::string step;                  // while Sending: prepare | chunks | assemble | single | request
    std::string server;                // the account the item was queued for; never sent to another one
    std::string user_id;
    std::string src_local;             // upload: absolute path of the source on the reader
    std::optional<int64_t> src_size;   // upload: recorded when the item starts (the pick-time size before that)
    std::optional<int64_t> src_mtime;  // seconds
    std::string src_remote;            // rename, move, copy, delete: the entry
    std::string src_etag;              // the version of that entry the reader last saw ("" or weak = none: such an item cannot be queued)
    std::optional<int64_t> src_fileid;
    std::string dst_parent;            // upload, mkdir, move, copy: the target folder; rename: the entry's own folder
    std::string dst_name;              // the name on the server after the change
    bool replace = false;              // the user confirmed replacing an existing file
    std::string dst_etag;              // replace: the version of the file to be replaced, as confirmed
    std::optional<int64_t> depends_on; // an item that must be Done first (a mkdir)
    std::string upload_id;             // chunked upload: the id of the upload folder
    std::optional<int64_t> chunk_size;
    std::string checksum;              // upload: SHA-256 hex of the source when the item started
    std::string result_etag;           // the version after the change, once known
    std::optional<int64_t> result_fileid;
    std::string reason;                // an error key or a hold reason (name_exists, changed_on_server, source_gone, target_gone, no_answer)
    int64_t created_at = 0;
    int64_t updated_at = 0;
    // specs/003-folder-sync: an item that a pair's approved plan created. The confirmation advances the pair's record (sync_rel) in its own transaction.
    std::optional<int64_t> pair_id;
    std::string sync_rel;              // the record the confirmation advances
    std::string sync_role;             // "", "file", "conflict_copy", "delete"
};

struct QueuePage {
    std::vector<QueueItem> rows;
    int total = 0;
};

// What recovery at start did: Sending rows whose step the server can verify went back to the queue, the others became unknown.
struct RecoveryReport {
    int requeued = 0;
    int unknown = 0;
};

// Where a name is taken (Store::name_use).
struct NameUse {
    bool cached = false;   // an entry of the cached listing of the folder
    bool queued = false;   // the target of an open queue item
    bool any() const { return cached || queued; }
};

// The facts a confirmation needs besides the answer of the server (research R-10): where the downloaded copies live, and the identity the
// source file has now (for the rule that links an uploaded replacement to the entry's own downloaded copy).
struct ConfirmContext {
    std::string download_root;
    std::optional<int64_t> source_size;
    std::optional<int64_t> source_mtime;
};

// A row of a folder listing with the open queue items merged in (data-model.md "Listing overlay", spec FR-008). `entry.name` is what is shown
// (the new name of a pending rename); `entry.path` is the identity a tap uses (the entry's own path, or the future path of a pending row).
struct OverlayRow {
    RemoteEntry entry;
    std::string pending;     // "" | new | renamed | moving | copying | deleting | replacing
    std::string sync_state;  // "" | queued | sending | failed | conflict | unknown
    int64_t item_id = 0;     // the queue item, 0 for a plain cached row
    bool pending_row = false;  // true: the row exists only in the queue (a new file or folder, the target of a move or copy), not on the server yet
};

struct OverlayPage {
    std::vector<OverlayRow> rows;
    int total = 0;
};

}  // namespace pbc
