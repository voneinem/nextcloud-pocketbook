#include "pbc/age_text.h"

namespace pbc {

Age listing_age(int64_t now, std::optional<int64_t> fetched_at, bool gone) {
    Age a;
    if (gone) {
        a.kind = AgeKind::Gone;
        return a;
    }
    if (!fetched_at) {
        a.kind = AgeKind::NotSynced;
        return a;
    }
    const int64_t d = now - *fetched_at;
    if (d < 0) {
        a.kind = AgeKind::JustNow;
        a.clock_suspect = true;
        return a;
    }
    if (d < 60) a.kind = AgeKind::JustNow;
    else if (d < 3600) { a.kind = AgeKind::Minutes; a.n = static_cast<int>(d / 60); }
    else if (d < 86400) { a.kind = AgeKind::Hours; a.n = static_cast<int>(d / 3600); }
    else { a.kind = AgeKind::Days; a.n = static_cast<int>(d / 86400); }
    return a;
}

}  // namespace pbc
