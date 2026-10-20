#include "pbc/entry_state.h"

namespace pbc {

namespace {

// Has the server version changed since the copy was downloaded (or since the user chose to keep their copy against it)?
bool server_changed(const RemoteEntry& e, const LocalFile& l) {
    if (!e.etag.empty()) return e.etag != l.remote_etag && e.etag != l.seen_etag;
    // no ETag: size and time, where both sides know them
    if (e.size && l.remote_size && *e.size != *l.remote_size) return true;
    if (e.mtime && l.remote_mtime && *e.mtime != *l.remote_mtime) return true;
    return false;
}

}  // namespace

EntryState compute_state(const RemoteEntry* cached, const LocalFile* local, const FileStat& stat) {
    if (!local) return EntryState::NotDownloaded;
    if (!stat.exists || !stat.regular) return EntryState::MissingLocally;
    if (local->server_missing) return EntryState::RemovedOnServer;
    const bool local_changed = local_copy_changed(*local, stat);
    const bool newer = cached && server_changed(*cached, *local);
    if (local_changed && newer) return EntryState::Conflict;
    if (local_changed) return EntryState::ChangedOnReader;
    if (newer) return EntryState::Outdated;
    return EntryState::Downloaded;
}

const char* state_key(EntryState s) {
    switch (s) {
        case EntryState::NotDownloaded: return "not_downloaded";
        case EntryState::Downloaded: return "downloaded";
        case EntryState::Outdated: return "outdated";
        case EntryState::ChangedOnReader: return "changed_on_reader";
        case EntryState::Conflict: return "conflict";
        case EntryState::RemovedOnServer: return "removed_on_server";
        default: return "missing_locally";
    }
}

}  // namespace pbc
