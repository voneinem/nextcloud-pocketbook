#pragma once
// The run that sends the queue (specs/002-manage-server-files, research R-15, constitution Principles III and VI): one item at a time in the order
// the user made them, each written as `sending` before its first request and ended in the same transaction as its effects. It never sleeps, never
// loops on a failure and never retries by itself: it stops at the first sign that the connection is gone or the server asks for a pause, and an item
// whose outcome is unknown is kept and skipped until the user decides. This file and dav_write.cpp/upload_session.cpp are the only ones that name write
// methods (rule N2). The files on the reader are only ever read (rule N6). Qt-free.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "pbc/check_outcome.h"
#include "pbc/dav_common.h"
#include "pbc/http_transport.h"
#include "pbc/queue_types.h"
#include "pbc/source_walk.h"
#include "pbc/store.h"
#include "pbc/types.h"

namespace pbc {

struct SendReport {
    int done = 0, failed = 0, conflict = 0, unknown = 0;   // items that ended this way in this run
    int left = 0;                                          // items that are still queued and could be sent
    std::string stopped_by_key;                            // "" = the run went through; else why it stopped (no_connection, rate_limited, cancelled, ...)
};

struct SendProgress {
    int64_t item_id = 0;
    ItemState state = ItemState::Sending;
    std::string step;          // prepare | chunks | assemble | single | request
    int64_t done = 0, total = 0;
};

struct SenderOptions {
    std::function<bool()> wifi_up;                          // asked between items and chunks; empty = up
    std::function<void(const SendProgress&)> on_progress;   // at state changes, after every chunk and every 64 KiB piece of the hash pass; the caller throttles
    std::function<void(const char* step)> hook;             // test seam: sending, upload_id, chunk, assemble, request, confirm
};

class QueueSender {
public:
    QueueSender(Store& store, HttpTransport& http, const Clock& clock, Random& random, SourceRules rules, std::string download_root,
                SenderOptions options = {});

    // One run: the whole queue, or only the item `only` (a Retry), or only the items of the group `only_group` (a sync sends its own uploads and never
    // the user's other items; specs/003-folder-sync). Blocks the caller (the worker thread).
    SendReport run(Cancel& cancel, int64_t only = 0, int64_t only_group = 0);

    // The choices for an item whose outcome is unknown (research R-7). The check and the best-effort read of the version in mark_unknown_done use the
    // network; the rest are short transactions.
    Found check_unknown(int64_t id);       // read-only; never changes the item
    Error mark_unknown_done(int64_t id);   // the user decided that it arrived: the effects of a confirmation, the row done
    Error send_again(int64_t id);          // Unknown -> Queued with its conditions unchanged
    Error remove_item(int64_t id);         // best-effort DELETE of the upload folder first when the item has one; a running item is refused

    // The choices for a held item (research R-8.4). Each is one short transaction; the two refreshes use the network and change nothing.
    // Keep both: Conflict -> Queued under `new_name` ("" = a free name next to the old one), as a new file (the upload folder and its chunks are kept).
    Error resolve_keep_both(int64_t id, const std::string& new_name);
    // PROPFIND of the item's target: what the server has now (nullopt = nothing there). The second confirmation of "Replace anyway" shows it.
    Result<std::optional<RemoteEntry>> refresh_target(int64_t id);
    // Replace anyway: Conflict -> Queued as a replace conditional on `etag` (the version the user was shown); "" = the name is free now: a plain upload.
    Error resolve_replace(int64_t id, const std::string& etag);
    // The same two steps for a rename, move, copy or delete held because its entry changed: the entry as it is now, then "apply anyway".
    Result<std::optional<RemoteEntry>> refresh_source(int64_t id);
    Error resolve_apply_anyway(int64_t id, const std::string& etag);
    // Edit the name of a failed or held item (the server refused the name, or it is taken): checked like every new name, the item is queued again as a
    // new file or folder under it. Upload folder and chunks are kept.
    Error resolve_edit(int64_t id, const std::string& new_name);
    // The same for the target folder: a failed or held upload, new folder, move or copy whose target is gone is queued again for another folder.
    Error resolve_retarget(int64_t id, const std::string& new_parent);
    Error resolve_discard(int64_t id);   // remove the item (the same as remove_item)

private:
    Store& store_;
    HttpTransport& http_;
    const Clock& clock_;
    Random& random_;
    SourceRules rules_;
    std::string download_root_;
    SenderOptions options_;
};

}  // namespace pbc
