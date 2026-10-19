// The built-in schema upgrades. Every step is a script that Store::open runs inside ONE transaction after a copy of the
// database file, followed by an integrity check. Steps only go forward; an older build refuses a database of a newer
// version (schema_too_new). `PRAGMA user_version` is the only version source.
//
// The baseline has no step: a fresh database is schema 1. Tests add their own steps through StoreOptions::extra_migrations.

#include "pbc/store.h"

namespace pbc {

const std::vector<Migration>& builtin_migrations() {
    static const std::vector<Migration> list = {};
    return list;
}

}  // namespace pbc
