#include "pbc/server_caps.h"

namespace pbc {

namespace {

const JsonValue* path(const JsonValue& v, std::initializer_list<const char*> keys) {
    const JsonValue* cur = &v;
    for (const char* k : keys) {
        cur = cur->get(k);
        if (!cur) return nullptr;
    }
    return cur;
}

bool has_control(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
}

// at most `max_entries` non-empty strings of at most `max_bytes` bytes without control characters; anything else in the array is skipped
std::vector<std::string> string_list(const JsonValue* arr, size_t max_entries, size_t max_bytes) {
    std::vector<std::string> out;
    if (!arr || arr->type != JsonValue::Type::Array) return out;
    for (const JsonValue& item : arr->items) {
        if (out.size() >= max_entries) break;
        if (item.type != JsonValue::Type::String || item.text.empty() || item.text.size() > max_bytes || has_control(item.text)) continue;
        out.push_back(item.text);
    }
    return out;
}

}  // namespace

ServerCaps server_caps_from_capabilities(const JsonValue& capabilities) {
    ServerCaps c;
    if (capabilities.type != JsonValue::Type::Object) return c;
    if (const JsonValue* v = path(capabilities, {"files", "undelete"}))
        if (v->type == JsonValue::Type::Bool) c.trash = v->boolean;
    if (const JsonValue* v = path(capabilities, {"dav", "chunking"}))
        if (v->type == JsonValue::Type::String && !v->text.empty()) c.chunking = true;
    if (const JsonValue* v = path(capabilities, {"files", "chunked_upload", "max_size"})) {
        const auto n = v->as_int();
        if (n && *n > 0 && *n <= (1ll << 40)) c.max_chunk = *n;
    }
    // the characters: each entry is one character (at most 4 UTF-8 bytes), at most 64 together
    for (const std::string& ch : string_list(path(capabilities, {"files", "forbidden_filename_characters"}), kMaxCapabilityEntries, 4)) c.forbidden_chars += ch;
    c.forbidden_names = string_list(path(capabilities, {"files", "forbidden_filenames"}), kMaxCapabilityEntries, kMaxCapabilityEntryBytes);
    c.forbidden_basenames = string_list(path(capabilities, {"files", "forbidden_filename_basenames"}), kMaxCapabilityEntries, kMaxCapabilityEntryBytes);
    c.forbidden_extensions = string_list(path(capabilities, {"files", "forbidden_filename_extensions"}), kMaxCapabilityEntries, kMaxCapabilityEntryBytes);
    return c;
}

bool delete_offered(const ServerCaps& caps) { return caps.trash.has_value() && *caps.trash; }

bool chunked_available(const ServerCaps& caps) { return caps.chunking.has_value() && *caps.chunking; }

int64_t chunk_size_for(const ServerCaps& caps) {
    if (!caps.max_chunk || *caps.max_chunk >= kDefaultChunkBytes) return kDefaultChunkBytes;
    if (*caps.max_chunk < kMinChunkBytes) return kMinChunkBytes;
    return *caps.max_chunk;
}

}  // namespace pbc
