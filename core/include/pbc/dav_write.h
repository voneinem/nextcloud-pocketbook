#pragma once
// The write side of the Nextcloud client (specs/002-manage-server-files): how the answer to a write request becomes a stable key
// (contracts/server-requests.md W10, research R-7 and R-12), the 412 disambiguation (research R-3) and the single write requests. This file
// and queue_sender.cpp/upload_session.cpp are the only places that name a write method (rule N2 of scripts/check-constitution.sh); every
// write carries its condition (contracts/server-requests.md, checked by tests/integration/test_write_conditions.cpp). Qt-free.
// Server behaviour is UNVERIFIED until a live run (docs/live-server-checklist.md LV-W*).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "pbc/dav_common.h"
#include "pbc/http_transport.h"
#include "pbc/queue_types.h"
#include "pbc/store_files.h"
#include "pbc/types.h"

namespace pbc {

// The kind of request an answer belongs to. Chunk = a request of the app's own upload session under /dav/uploads/ (idempotent: the next send
// lists the upload folder, so a lost answer is never "outcome unknown").
enum class WriteKind { Put, Mkcol, Move, Copy, Delete, Chunk };

struct WriteAnswer {
    Error error;                       // ok() = the server confirmed
    long status = 0;
    Delivery delivery = Delivery::NotSent;
    std::string etag;                  // OC-Etag, else ETag ("" if the answer has neither)
    std::optional<int64_t> fileid;     // OC-FileId, only when it is a plain number (the opaque form is read with a PROPFIND instead)
    bool outcome_unknown = false;      // the request may have been executed and no answer arrived (research R-7)
    bool ambiguous_412 = false;        // a 412 on MOVE/COPY: the target exists or the source changed, see disambiguate_412
    std::string exception_class;       // the whitelisted sabre class ("InvalidPath", ...) or ""
};

// Turns the response of one write request into a WriteAnswer. Pure. `replacing` tells a PUT that carried If-Match from one that carried
// If-None-Match. Nothing of the body, the headers or the URL ends up in the error detail.
WriteAnswer interpret_write_answer(const Response& r, WriteKind kind, bool replacing);
Error classify_write_answer(const Response& r, WriteKind kind, bool replacing);

// The exception class in a sabre error body: the last name segment of <s:exception> when it is one of a fixed list (InvalidPath,
// ForbiddenFileName, FileLocked (also Locked), NotFound, EntityTooLarge, UnsupportedMediaType, InsufficientStorage, Forbidden), else "".
// The body is read only up to 64 KiB with the minimal XML reader; a trailing "Exception" of the class name is ignored.
std::string sabre_exception_class(const std::string& body);

enum class Cause412 { NameExists, ChangedOnServer, SourceGone };
// A 412 on MOVE or COPY: `target` and `source` are the entries a Depth 0 PROPFIND found (nullopt = 404). Pure.
Cause412 disambiguate_412(const std::optional<RemoteEntry>& target, const std::optional<RemoteEntry>& source, const std::string& expected_source_etag);

// ---- the single write requests (contracts/server-requests.md W3, W5-W8). Each sends exactly one request with its condition and returns what
// interpret_write_answer made of it. `progress(done, total)` returns false to cancel a transfer in the middle of its body.
using ProgressFn = std::function<bool(long long done, long long total)>;

// The URL of a path of the account: <base>/remote.php/dav/files/<uid><encoded path>, with a trailing "/" for a folder.
std::string files_url(const DavSession& session, std::string_view user_id, std::string_view remote_path, bool folder = false);

// W3: PUT of the whole file of the item; new (If-None-Match: *) or replace (If-Match: dst_etag). `checksum` is the SHA-256 hex of the source ("" = none),
// `mtime` the source's modification time in seconds (0 = none).
WriteAnswer put_small(DavSession& session, std::string_view user_id, const QueueItem& item, const std::string& checksum, int64_t mtime, ProgressFn progress);

// W8: DELETE of an entry, conditional on its version (If-Match, a strong ETag). On a server with a trash it lands there; this function never decides
// whether that is the case: the caller does (research R-9). 404 is a success (what was asked for has happened), 207 is partial_failure.
WriteAnswer delete_entry(DavSession& session, std::string_view user_id, std::string_view remote_path, const std::string& etag, bool folder);

// W6/W7: MOVE (a rename in its own folder, or a move) and COPY of an entry to dst_parent/dst_name. `Destination` is the full address of the new place,
// `Overwrite: F` (never replace silently) and `If-Match` with the version of the entry the reader last saw. A folder's addresses end with "/". A 412 is
// ambiguous (the target exists, or the source changed): the caller decides with disambiguate_412.
WriteAnswer move_entry(DavSession& session, std::string_view user_id, const QueueItem& item);
WriteAnswer copy_entry(DavSession& session, std::string_view user_id, const QueueItem& item);

// W5: MKCOL of a new folder (conditional by nature: an existing name is refused with 405).
WriteAnswer mkcol(DavSession& session, std::string_view user_id, std::string_view remote_path);

}  // namespace pbc
