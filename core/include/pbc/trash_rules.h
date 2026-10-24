#pragma once
// The pure rules of the trash and versions screens (specs/006-trash-and-versions, contracts/core-interfaces.md "trash_rules.h"): what the server's
// capabilities say about the two apps, the plan of a restore, the confirmation each irreversible step shows, the set Empty trash names. Nothing here
// touches the network, the store or the clock. Server behaviour behind the capability keys is UNVERIFIED (docs/live-server-checklist.md LV-T1). Qt-free.

#include <optional>
#include <string>
#include <vector>

#include "pbc/json_min.h"
#include "pbc/server_caps.h"
#include "pbc/store_files.h"
#include "pbc/trash_types.h"

namespace pbc {

// The "capabilities" object of the OCS answer (ocs.data.capabilities) -> the two states of research R-2. `files.undelete` (the trash app) and
// `files.versioning` (the versions app) are judged separately: true is On; a missing or false key inside a "files" object is Off (the apps announce
// themselves only while they are enabled); a value that is not an object, no "files" object, or a key that is not a boolean is Unknown for what it
// concerns. Unknown is never Off. `code` is left empty here.
TrashAvailability availability_from_capabilities(const JsonValue& capabilities);

// An item the user can act on: readable, and not marked unavailable (encrypted, no permission, an unknown type).
bool actionable(const TrashItem& item);

// ---- the plan of a restore (research R-5) ----
struct RestoreProbe {
    bool original_known = false;
    std::optional<RemoteEntry> at_original;     // what the probe found at the original place (nullopt: nothing there)
    std::optional<bool> parent_exists;          // whether the original folder exists; nullopt = not asked (the server decides)
};
enum class RestorePlan { OriginalFree, NameTaken, ParentMissing, ChoosePlace, Unavailable };
// Decided before any write, from the probes: an item that cannot be acted on is Unavailable; an unknown original place is ChoosePlace; a name that exists
// is NameTaken (the entry is the stronger fact); a folder that does not exist is ParentMissing; otherwise OriginalFree.
RestorePlan plan_restore(const TrashItem& item, const RestoreProbe& probe);

// ---- a chosen target (research R-6) ----
struct TargetCheck {
    bool ok = false;
    std::string error_key;   // name_forbidden | restore_parent_missing | restore_into_self
};
// The folder must be a valid remote path, the name passes the rename rules of feature 002 (and what the server forbids), and a folder is not put inside
// its own former place (unless the target is the original place itself).
TargetCheck check_restore_target(const TrashItem& item, const RestoreTarget& target, const ServerCaps& caps);

// ---- what a confirmation says (research R-15) ----
enum class ConfirmKind { RestoreProposal, DeleteFile, DeleteFolder, EmptyTrash, RestoreVersion };
struct ConfirmContent {
    ConfirmKind kind = ConfirmKind::RestoreProposal;
    std::vector<std::string> shown_names;       // at most 5, each at most 40 bytes, cut on a character boundary
    int items = 0, folders = 0, stage = 1;
    bool truncated_notice = false, destructive = false, default_cancel = true, original_place = false;
    std::string place;                          // the folder shown (a restore)
    std::optional<int64_t> version_mtime, version_size;   // the version a restore of a version is about (nullopt = unknown)
};
// The number of confirmations a kind needs: 1 for a restore, a file and a version, 2 for a folder and for emptying the trash.
int confirm_stages(ConfirmKind kind);
ConfirmContent confirm_for_restore(const TrashItem& item, const RestoreTarget& target);
// A version restore: the file's name, the version's date and size; it says the current content is kept as one more version. One confirmation, Cancel default.
ConfirmContent confirm_for_version(const std::string& file_name, const VersionRow& row);

// ---- the irreversible steps (research R-8, R-15) ----
// A file has one confirmation, a folder two; every one names the item, says it cannot be undone and defaults to Cancel. `stage` is clamped to the stages of the kind.
ConfirmContent confirm_for_delete(const TrashItem& item, int stage);
// Emptying the trash: the number of items and of folders (with their content) that the named set holds, up to five names, the notice that the list was cut.
ConfirmContent confirm_for_empty(const std::vector<TrashItem>& named, bool list_truncated, int stage);
// The set Empty trash names: exactly the actionable items of the list as held, in order; the others are only counted. A later list change never alters it.
struct EmptySet {
    std::vector<TrashItem> named;
    int not_actionable = 0;
};
EmptySet empty_set_of(const TrashList& list);
// What the loop of Empty trash does after one delete: go on after a refusal (locked, forbidden, a server error, an item that is gone); stop at a refused login, at a
// pause the server asks for, at a lost answer and at a connection that is not there.
enum class LoopStep { Continue, StopAuth, StopRateLimited, StopUnknown, StopConnection };
LoopStep loop_step_after(const OperationResult& r);

// ---- the key and the short code of a refused request (research R-11) ----
struct Refusal {
    std::string key, code;   // code: "T<op>-<status>", no name, no server text
};
// `op`: 1 capabilities, 2 trash list, 4 restore to the original place, 5 restore to a chosen place, 6 version list, 7 version restore, 8 delete.
Refusal refusal_for(int op, long status);

}  // namespace pbc
