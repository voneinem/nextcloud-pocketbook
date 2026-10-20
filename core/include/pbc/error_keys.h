#pragma once
// The stable cause keys of the files client (specs/001-nextcloud-files-client/contracts/server-requests.md, "Error keys"). The UI has
// one message and one "what to do" line per key; the keys never carry an address, a user name or a name from the server.
// Qt-free.

#include <string>
#include <vector>

#include "pbc/types.h"

namespace pbc {

// Every key, once. Includes the parser-level keys foreign_href (a d:href of another account) and qr_too_long.
const std::vector<std::string>& all_error_keys();
bool is_known_error_key(const std::string& key);
// The error class of a key; an unknown key is Protocol.
ErrorClass class_of(const std::string& key);
// An Error with the class of the key and an empty detail; an unknown key becomes protocol_error.
Error error_from_key(const std::string& key);

}  // namespace pbc
