#pragma once
// A minimal, strict JSON reader (RFC 8259) for the OCS and status.php answers of the files client. Total: any input gives a tree or an
// error, never a crash or a hang. Limits bound the size, the nesting and the node count. Numbers keep their literal text; an integer
// is read on demand with as_int(). Strings are decoded to UTF-8 (\uXXXX and surrogate pairs); a lone surrogate or a raw control
// character is an error. Bytes >= 0x80 pass through unchanged. No Qt.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pbc/types.h"

namespace pbc {

struct JsonLimits {
    size_t max_bytes = 1u << 20;
    int max_depth = 32;
    size_t max_nodes = 100000;
};

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    std::string text;                                        // the decoded string, or the literal of a number
    std::vector<JsonValue> items;                            // Array
    std::vector<std::pair<std::string, JsonValue>> members;  // Object, in document order (duplicates are kept)

    // The first member with this key; nullptr for a missing key or when this is not an object.
    const JsonValue* get(std::string_view key) const;
    // The number as a 64-bit integer; nullopt for a non-number, a fraction, an exponent or a value out of range.
    std::optional<int64_t> as_int() const;
};

// Error: ErrorClass::Protocol, key "protocol_error" (the detail names the kind of problem, never the content).
Result<JsonValue> parse_json(std::string_view json, const JsonLimits& limits = JsonLimits());

}  // namespace pbc
