#pragma once
// How old a shown listing is (spec FR-012, FR-035): the kind and the number, so that the page can put the words around them in the
// reader's language. Pure arithmetic. No Qt.

#include <cstdint>
#include <optional>

namespace pbc {

enum class AgeKind { NotSynced, Gone, JustNow, Minutes, Hours, Days };

struct Age {
    AgeKind kind = AgeKind::NotSynced;
    int n = 0;                     // minutes, hours or days
    bool clock_suspect = false;    // the listing claims to be from the future: the reader's clock is wrong
};

// fetched_at: when the server last confirmed the listing (nullopt: never). A folder that is gone says so whatever its age.
Age listing_age(int64_t now, std::optional<int64_t> fetched_at, bool gone);

}  // namespace pbc
