#pragma once
// Private to the store implementation: helpers shared by the store_*.cpp files.

#include <sqlite3.h>

#include <algorithm>
#include <optional>
#include <string>

#include "pbc/types.h"
#include "store_sql.h"

namespace pbc {
namespace store_util {

// At most `max` bytes, never cutting inside a UTF-8 character.
inline std::string cap(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

Error storage_error(sqlite3* db, const char* fallback_key);   // store_util.cpp

struct Transaction {
    sqlite3* db;
    bool open = false;
    explicit Transaction(sqlite3* d) : db(d) { open = sql::exec(db, "BEGIN IMMEDIATE", nullptr); }
    bool commit() {
        if (!open) return false;
        open = false;
        if (sql::exec(db, "COMMIT", nullptr)) return true;
        sql::exec(db, "ROLLBACK", nullptr);
        return false;
    }
    ~Transaction() {
        if (open) sql::exec(db, "ROLLBACK", nullptr);
    }
};

inline void bind_text_or_null(sql::Stmt& st, int i, const std::string& v) {
    if (v.empty()) st.bind_null(i);
    else st.bind_text(i, v);
}

inline void bind_opt(sql::Stmt& st, int i, const std::optional<int64_t>& v) {
    if (v) st.bind_int64(i, *v);
    else st.bind_null(i);
}

inline std::optional<int64_t> opt_col(const sql::Stmt& st, int col) {
    if (st.is_null(col)) return std::nullopt;
    return st.int64(col);
}

}  // namespace store_util
}  // namespace pbc
