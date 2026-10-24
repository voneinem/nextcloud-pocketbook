#pragma once
// The tolerant parsers of the trash list and of a file's version list (specs/006-trash-and-versions, contracts/core-interfaces.md, research R-3).
// Everything in an answer is untrusted: a name that cannot be one path segment makes the item unreadable (listed, never addressable), a location
// that is unsafe or too long makes the original place unknown, a member that failed is skipped, an answer above the limit is refused. The sort
// is made here, not trusted from the server. No input makes a parser fail in any way but with an error key. Server behaviour is UNVERIFIED
// (docs/live-server-checklist.md LV-T2, LV-T7). Qt-free.

#include <cstdint>
#include <string>
#include <string_view>

#include "pbc/http_transport.h"
#include "pbc/trash_types.h"
#include "pbc/types.h"

namespace pbc {

// The property list of the trash request (T2).
std::string trash_request_body();
// A 207 answer of T2 -> TrashList. `user_id` fixes the expected address prefix, `now` bounds the deletion times. Errors: listing_too_large (the
// body cap was hit), malformed_xml, protocol_error (not a 207).
Result<TrashList> parse_trash(const Response& r, std::string_view user_id, int64_t now);

// The property list of the version request (T6).
std::string versions_request_body();
// A 207 answer of T6 -> VersionList (without the file stamp, which the caller adds). The collection itself is not a row. Same errors.
Result<VersionList> parse_versions(const Response& r, int64_t now);

// One path segment of valid text: 1..255 bytes, valid UTF-8, not "." or "..", no "/" or "\", no control character (NUL included).
bool is_safe_segment(std::string_view s);
// The display form of a name from the server: control characters removed, at most 255 bytes, cut on a character boundary.
std::string display_name(std::string_view s);

// "Books/story.epub" -> {dir "/Books", name "story.epub", known}; a location with an unsafe or empty segment, a "." or "..", invalid text, or more
// than kMaxLocationBytes is unknown (dir and name stay empty).
struct OriginalPlace {
    std::string dir, name;
    bool known = false;
};
OriginalPlace parse_original_location(std::string_view location);

}  // namespace pbc
