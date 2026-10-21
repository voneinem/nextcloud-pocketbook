#pragma once
// Remote paths of the files client (specs/001-nextcloud-files-client, data-model.md "Paths"). An application-relative remote path
// starts with "/", has no trailing "/" except for the root "/", and holds the bytes the server sent: nothing is normalized in the
// Unicode sense, and comparison is byte-exact. Everything here is pure string work, tested on the host. No Qt.

#include <string>
#include <string_view>

#include "pbc/types.h"

namespace pbc {

// "" and "/" -> "/"; a missing leading "/" is added, repeated and trailing "/" are dropped. "." and ".." are not interpreted.
std::string normalize_remote_path(std::string_view raw);
// "/a/b" -> "/a", "/a" -> "/", "/" -> "/".
std::string parent_of(std::string_view remote_path);
std::string join_remote(std::string_view folder, std::string_view name);
// Percent-encodes each segment as UTF-8 (everything except A-Z a-z 0-9 - . _ ~), keeps "/" between segments, never uses "+".
std::string encode_path(std::string_view remote_path);

// The remote path for a d:href of a multistatus answer. The href is a path or an absolute address; it must lie below
// /remote.php/dav/files/<user_id>/ (the user id is compared after decoding, case-sensitive). Every segment is decoded on its own, so
// an encoded "/" cannot turn one name into two. Errors: "foreign_href" (another prefix or user: the entry is not ours, the caller
// counts and drops it) and "protocol_error" (an invalid percent escape, a NUL or control character, ".", "..", an empty segment, a
// query or a fragment).
Result<std::string> decode_href(std::string_view href, std::string_view user_id);

// "https://Host:8443/x" -> scheme "https", host "host" (lower-cased), port 8443 (443 for https and 80 for http when absent). User
// information is ignored. false when the text has no scheme and authority.
bool parse_url_origin(std::string_view url, std::string* scheme, std::string* host, int* port);
// true when both addresses are https and name the same host and port.
bool same_https_origin(std::string_view a, std::string_view b);

// A name can be one path segment: 1..1024 bytes, not "." or "..", no "/", no control characters (U+0000..U+001F, U+007F).
bool is_valid_name(std::string_view name);
// A remote path as the queue and the actions use it: absolute, already normalized, no "." or ".." segment, no control character.
bool is_valid_remote_path(std::string_view path);

}  // namespace pbc
