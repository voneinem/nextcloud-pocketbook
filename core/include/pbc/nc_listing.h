#pragma once
// A folder listing of the user's Nextcloud (specs/001-nextcloud-files-client, contracts/server-requests.md section 5): the PROPFIND
// request (Depth 1 only, never infinity) and the parser of its answer. Everything in the answer is untrusted: names that cannot be one
// path segment are skipped, entries the user cannot use are marked, and no input makes the parser fail in any way but with an error key.
// Server behaviour is UNVERIFIED (docs/live-server-checklist.md LV-N5). Qt-free.

#include <string>
#include <string_view>
#include <vector>

#include "pbc/dav_common.h"
#include "pbc/remote_path.h"
#include "pbc/store_files.h"
#include "pbc/types.h"

namespace pbc {

// The property list of section 5.
std::string listing_request_body();

// Turns the answer into a Listing. `requested_path` is the folder that was asked for: its own response is not an entry. Errors:
// listing_too_large (the body cap was hit, or more than kMaxEntriesPerFolder entries), malformed_xml, protocol_error (not a 207).
Result<Listing> parse_listing(const Response& r, std::string_view user_id, std::string_view requested_path);

// One request: PROPFIND Depth 1 on the folder. Errors are those of DavSession::status_error plus the parser's; a 404 is not_found.
Result<Listing> fetch_listing(DavSession& session, std::string_view user_id, std::string_view remote_path);

// ---- favorites and server search (contracts/server-requests.md sections 7 and 8) ----

inline constexpr int kMaxFavorites = 2000;      // more are cut; the page says so
inline constexpr int kMaxSearchResults = 100;

// The REPORT body that asks for the entries flagged as favorites, with the properties of section 5.
std::string favorites_request_body();
// The SEARCH body for a name search over the whole account. The term is trimmed, cut to 100 bytes, stripped of control characters and of
// `%`, `_` and `\` (the characters of a LIKE pattern) and XML-escaped. Empty if nothing usable is left: then no request is made.
std::string search_request_body(std::string_view user_id, std::string_view term);
// The term as the request sends it (before the XML escape), or "" if nothing usable is left.
std::string clean_search_term(std::string_view term);

// An answer that holds entries from many folders. Addresses outside the account and unusable names are left out; the order is kept; at most
// `max_entries` are returned. Errors: listing_too_large, malformed_xml, protocol_error (not a 207).
Result<std::vector<RemoteEntry>> parse_flat_result(const Response& r, std::string_view user_id, size_t max_entries);

// One request each. They return up to kMaxFavorites + 1 and kMaxSearchResults + 1 entries: the extra one tells the caller that more exist.
Result<std::vector<RemoteEntry>> fetch_favorites(DavSession& session, std::string_view user_id);
Result<std::vector<RemoteEntry>> search_server(DavSession& session, std::string_view user_id, std::string_view term);

}  // namespace pbc
