#pragma once
// Opening and refreshing folders (specs/001-nextcloud-files-client US2): one request when a folder is opened for the first time or when
// the user refreshes it, never on its own. What was fetched stays in the store and is shown without a connection; a failed refresh
// changes nothing. Used by the bridge on the worker thread. Qt-free.

#include <string>
#include <vector>

#include "pbc/http_transport.h"
#include "pbc/store.h"
#include "pbc/types.h"

namespace pbc {

enum class Fetch {
    Never,       // only look at the cache
    IfMissing,   // fetch when the folder was never fetched (opening a folder)
    Always,      // fetch now (the explicit Refresh)
};

struct FolderResult {
    Error error;               // a fetch that was asked for and failed; the cached rows, if any, are untouched
    bool fetched = false;      // a request was sent and its answer was stored
    bool cached = false;       // there are rows to show (from this fetch or an earlier one)
    bool partial = false;      // some entries of the answer could not be used
    int skipped = 0;           // addresses and names that were left out
};

// The answer of the favorites refresh and of the server search.
struct FlatResult {
    Error error;                        // the cached favorites (if any) are untouched on an error
    std::vector<RemoteEntry> entries;   // at most kMaxFavorites / kMaxSearchResults
    bool more = false;                  // the server had more than that; the rest was cut
};

class FolderService {
public:
    FolderService(Store& store, HttpTransport& http, const Clock& clock) : store_(&store), http_(&http), clock_(&clock) {}

    // Opens (and renews in the eviction order) the folder; fetches according to `mode`. Needs the saved account and its app password;
    // without them, or after two refused logins (AuthGuard), nothing is sent and the error is auth_failed.
    FolderResult load(const std::string& path, Fetch mode);

    // One REPORT for the favorites; on success the stored list is replaced as a whole, on any error it is kept.
    FlatResult refresh_favorites();
    // One SEARCH for the whole account, only when the user taps for it. The results are held in memory (Store::set_search_results) and are
    // never written to the folder cache. An unusable term (empty after cleaning) sends nothing.
    FlatResult search_server(const std::string& term);

private:
    Store* store_;
    HttpTransport* http_;
    const Clock* clock_;
};

}  // namespace pbc
