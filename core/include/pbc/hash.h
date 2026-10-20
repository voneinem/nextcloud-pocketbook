#pragma once
// The checksums a Nextcloud server lists for a file (oc:checksums, "SHA1:… MD5:… ADLER32:…") and the check of a finished download
// (specs/001-nextcloud-files-client, research R-8 point 5). SHA-256, SHA-1 and MD5 through OpenSSL; unknown algorithms are ignored.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pbc/types.h"

namespace pbc {

enum class HashAlgo { Sha256, Sha1, Md5 };

struct Checksum {
    HashAlgo algo = HashAlgo::Sha256;
    std::string hex;   // lower case
};

// Entries are separated by blanks or commas; "name:hex" with a known name and the right number of hex digits (64, 40, 32).
std::vector<Checksum> parse_checksums(std::string_view oc_checksums);
// The strongest of the listed checksums (SHA-256, then SHA-1, then MD5).
std::optional<Checksum> strongest(const std::vector<Checksum>& list);

// One streaming pass over the file in 64 KiB pieces; Cancel is asked before every piece ("cancelled").
Result<std::string> hash_file(const std::string& path, HashAlgo algo, Cancel& cancel);

}  // namespace pbc
