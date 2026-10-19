#include "store_util.h"

#include "pbc/log.h"

namespace pbc {
namespace store_util {

Error storage_error(sqlite3* db, const char* fallback_key) {
    const int rc = sqlite3_errcode(db);
    const bool full = rc == SQLITE_FULL || sqlite3_extended_errcode(db) == SQLITE_IOERR_WRITE;
    log::event("store.write_failed", {log::count("sqlite", rc)});
    return make_error(ErrorClass::Storage, full ? "storage_full" : fallback_key, "the database could not be written");
}

}  // namespace store_util
}  // namespace pbc
