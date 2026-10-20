#pragma once
// The state of a file as the list shows it (specs/001-nextcloud-files-client FR-026, data-model.md "Entry state"): computed from what the
// listing says, what was downloaded and what the card holds now - never stored. Pure. No Qt.

#include "pbc/local_files.h"
#include "pbc/store_files.h"

namespace pbc {

enum class EntryState {
    NotDownloaded,     // no copy on the reader
    Downloaded,        // an unchanged copy of the current server version
    Outdated,          // the server has another version; the copy is unchanged
    ChangedOnReader,   // the user changed the copy; the server version is the one it came from
    Conflict,          // both changed
    RemovedOnServer,   // the last listing of the parent did not contain it; the copy stays
    MissingLocally,    // a recorded copy that is no longer on the card
};

// `cached`: the entry of the newest listing (nullptr if none is known); `local`: the recorded copy (nullptr if none); `stat`: the card now.
EntryState compute_state(const RemoteEntry* cached, const LocalFile* local, const FileStat& stat);

// "not_downloaded", "downloaded", "outdated", "changed_on_reader", "conflict", "removed_on_server", "missing_locally"
const char* state_key(EntryState state);

}  // namespace pbc
