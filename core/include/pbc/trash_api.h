#pragma once
// The requests to the server's trash and versions (specs/006-trash-and-versions/contracts/trash-requests.md, research R-1 to R-10). Each method sends
// exactly the requests its row lists, never retries a write and never logs more than a code, a status and a count. This is the only module that
// names MOVE or DELETE for the trashbin and the versions (rule N2) and one of two that name their endpoints (rule N11). UNVERIFIED against a real
// server (docs/live-server-checklist.md LV-T*). Qt-free.

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
#include <string>
#include <string_view>

#include "pbc/dav_common.h"
#include "pbc/store_files.h"
#include "pbc/trash_rules.h"
#include "pbc/trash_types.h"
#include "pbc/types.h"

namespace pbc {

// Timeouts of every request of the trash and versions screens (connect 10 s, total 30 s).
inline constexpr long kTrashConnectTimeoutS = 10;
inline constexpr long kTrashTotalTimeoutS = 30;

// The state of an Empty trash run after an item: how many were handled, removed and refused so far.
struct EmptyProgress {
    int done = 0, total = 0, removed = 0, not_removed = 0;
};

class TrashApi {
public:
    // `user_id`: the id of the signed-in user (the address of the trashbin, the versions and the files root).
    TrashApi(DavSession& session, std::string_view user_id, const Clock& clock);

    // T1: is the trash app on, is the versions app on. A failure gives both states Unknown, the code "T1-<status>" and the error key of the failure
    // (auth_failed, no_network, server_error, ...): the bridge shows the message of an Auth, Network or Tls key, and the generic message with the code for
    // a Server or Protocol one. An answer that is fine but says nothing (no files object) is Unknown with the code and no error.
    Result<TrashAvailability> fetch_availability();

    // T2: the trash, newest deletion first, at most kMaxTrashRows items (more are cut and `truncated` says so). Errors: those of DavSession::status_error,
    // listing_too_large (the answer was above the limit), malformed_xml, protocol_error (also a 404: the capability said On, so an absent endpoint is an
    // unclear answer, never "off"). On every failure `code` is "T2-<status>". The caller stores the list.
    Result<TrashList> list_trash();

    // T3: what is at a place of the files and in its folder. The entry is asked for first; its folder only when the entry is absent and the folder is
    // not the root. NameTaken (the entry exists, whatever it is), ParentMissing (the folder does not exist or is not a folder), else OriginalFree (= the
    // place is free). Never writes. `detail` receives the probe for plan_restore.
    Result<RestorePlan> probe_place(std::string_view folder, std::string_view name, RestoreProbe* detail = nullptr);
    // The plan for an item: Unavailable and ChoosePlace without a request, otherwise probe_place of its original place.
    Result<RestorePlan> probe_restore(const TrashItem& item, RestoreProbe* detail = nullptr);

    // T4: restore to the original place. Sends the MOVE only for an item that is actionable and has a known original place (else trash_not_actionable, nothing
    // sent). After a confirmed answer the place is probed again: the item there (same kind, same size for a file) is "done"; anything else is
    // `landed_elsewhere` (a late collision: the server put it under another name; the app renames nothing). A lost answer is outcome_unknown, never repeated.
    OperationResult restore_original(const TrashItem& item);
    // T5: restore to a chosen place with Overwrite: F. The name must pass the rename rules, the folder must be a valid path. A refusal that says the server
    // cannot do this (403, 405, 501, 502) becomes restore_place_unsupported only when a probe shows nothing was restored.
    OperationResult restore_to(const TrashItem& item, const RestoreTarget& target);

    // T3 + T6: the older versions of a file, newest first, at most kMaxVersionRows; the current content is not a row. `file` is an entry of a stored listing
    // (its id builds the address). The file is probed first (its ETag and id become the stamp of the stale check). A folder is trash_not_actionable, an entry
    // without an id refresh_first (nothing is sent), a file that is gone file_gone. A 404 of the versions is an empty list. Other failures: their key and
    // the code "T6-<status>".
    Result<VersionList> list_versions(const RemoteEntry& file);
    // The stale check (T3: the file's ETag must still be the stamp of `shown`; else version_changed_meanwhile and nothing is sent), then T7 (a MOVE with
    // Overwrite: F), then the list again. `after` receives that list when it could be read. A lost answer is outcome_unknown, never repeated;
    // `previous_not_kept` says that the content that was current is not among the versions afterwards (the server did not keep it).
    OperationResult restore_version(const RemoteEntry& file, const VersionList& shown, const VersionRow& row, VersionList* after = nullptr);

    // T8: ONE trash item, for good. Called only from TrashBridge after the last confirmation (rule N12). A 2xx or a 404 (already gone) is removed; any other answer
    // is not removed with its key and the code "T8-<status>"; a lost answer is outcome_unknown (neither counted), never repeated. The request names the item by its
    // unique trash name and carries no condition header (research R-7); never the collection.
    OperationResult delete_item(const TrashItem& item);
    // Empty trash: delete_item for exactly the items of `named`, in order, one request at a time (rule N12 applies here too). After each item `progress` is called
    // (it may be empty); a false answer stops the run between two requests (error cancelled). The loop goes on after a refusal and stops as loop_step_after says; the
    // items after the stop are `not_tried`. `removed_trash_names` lists what is gone.
    OperationResult empty_trash(const std::vector<TrashItem>& named, const std::function<bool(const EmptyProgress&)>& progress);

    std::optional<int64_t> server_time() const { return session_.last_server_time(); }

private:
    OperationResult move_item(const TrashItem& item, int op, const std::string& destination, const std::string& verify_folder, const std::string& verify_name);
    std::string trash_base() const;
    std::string versions_base() const;   // the address of the account's versions area   // the address of the account's trash area
    int64_t now() const;              // the server's time when known, else the reader's
    DavSession& session_;
    std::string user_id_;
    const Clock& clock_;
};

// Pure: what a response means for a request of operation `op` (4, 5, 7 or 8): the error of an answer that is not a confirmation, an empty Error for one that is.
// A lost answer is outcome_unknown (the request may have been executed), a request that never started is no_connection; the sabre exception refines a refusal.
Error interpret_trash_answer(const Response& r, int op);

}  // namespace pbc
