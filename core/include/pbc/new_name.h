#pragma once
// Names for the server (specs/002-manage-server-files, research R-8 and R-11): the check every new name passes before it is queued (a
// folder, a rename, a file picked on the reader, a keep-both name) and the name "keep both" proposes. Names are compared byte-exactly and
// case-sensitively, never normalized. Pure. Qt-free.

#include <functional>
#include <string>
#include <string_view>

#include "pbc/server_caps.h"

namespace pbc {

struct NameCheck {
    bool ok = false;
    std::string error_key;   // "name_forbidden" when !ok
};

inline constexpr size_t kMaxNameBytes = 255;

// Refuses: an empty name, "." and "..", a "/" or "\", control characters, a trailing space or dot (so names made only of dots or spaces),
// invalid UTF-8, more than 255 bytes, and what the server's lists forbid (characters, whole names, base names, extensions, all compared
// without regard to case). A list the server did not state forbids nothing here; the server's own refusal then has the last word.
NameCheck check_new_name(std::string_view name, const ServerCaps& caps);

// "name (2).ext", "name (3).ext", ...: the first candidate for which `taken` is false and check_new_name is ok. The number goes before the last
// extension (at the end of a name without one, of a dotfile, or of a name that ends with a dot). Within 255 bytes the stem is shortened on a
// character boundary, never the extension. "" when `name` itself is not valid or every candidate up to 10,000 is taken.
std::string keep_both_name(std::string_view name, const std::function<bool(std::string_view)>& taken, const ServerCaps& caps);

}  // namespace pbc
