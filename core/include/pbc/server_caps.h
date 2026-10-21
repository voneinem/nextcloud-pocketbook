#pragma once
// What the account check learns about the server for the write side (specs/002-manage-server-files, contracts/server-requests.md W1,
// research R-9 and R-11). The capabilities of a server are untrusted and bounded: a missing or unusable value is "unknown", never a
// guess, and an unknown trash counts as no trash for Delete. JSON paths are UNVERIFIED (docs/live-server-checklist.md LV-W1, LV-W10). Qt-free.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pbc/json_min.h"

namespace pbc {

inline constexpr size_t kMaxCapabilityEntries = 64;    // per list
inline constexpr size_t kMaxCapabilityEntryBytes = 32;
inline constexpr int64_t kDefaultChunkBytes = 10ll << 20;
inline constexpr int64_t kMinChunkBytes = 5ll << 20;   // the minimum of the chunked upload protocol

struct ServerCaps {
    std::optional<bool> trash;                 // files.undelete; nullopt = unknown (counts as false for Delete)
    std::optional<bool> chunking;              // dav.chunking
    std::optional<int64_t> max_chunk;          // files.chunked_upload.max_size in bytes
    std::string forbidden_chars;               // up to 64 characters, each a single character of at most 4 bytes
    std::vector<std::string> forbidden_names, forbidden_basenames, forbidden_extensions;   // up to 64 entries of 32 bytes each
    std::optional<int64_t> checked_at;
};

// Reads the "capabilities" object of the OCS answer. Total: anything else gives unknown fields.
ServerCaps server_caps_from_capabilities(const JsonValue& capabilities);

bool delete_offered(const ServerCaps& caps);       // trash == true
bool chunked_available(const ServerCaps& caps);    // chunking == true
// 10 MiB; the server's maximum when it lies in [5 MiB, 10 MiB); never below 5 MiB.
int64_t chunk_size_for(const ServerCaps& caps);

}  // namespace pbc
