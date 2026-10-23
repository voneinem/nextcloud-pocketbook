#pragma once
// The name of a conflict copy (specs/003-folder-sync R-9): when a file changed on both sides, each side gets the other's version under a name that says where
// it came from and when, and keeps its own version under the original name. Pure. Qt-free.

#include <functional>
#include <string>
#include <string_view>

namespace pbc {

// "<stem> (server copy YYYY-MM-DD)<ext>" (server_copy) or "<stem> (reader copy YYYY-MM-DD)<ext>", with " (2)", " (3)", ... after the marker while `taken` says the
// name is. The extension is the part from the last dot (none for a dotfile, a name without a dot or one that ends with a dot: the marker goes at the end).
// Within 255 bytes the stem is shortened on a character boundary, never the extension. The date is the reader's date, YYYY-MM-DD, for the eye only.
// "" when the name is empty, holds a "/", a backslash or a control character, is not valid UTF-8, when `date` is not YYYY-MM-DD, when the extension leaves no
// room for the marker, or when every number up to 10,000 is taken.
std::string conflict_name(std::string_view name, bool server_copy, std::string_view date, const std::function<bool(std::string_view)>& taken);

}  // namespace pbc
