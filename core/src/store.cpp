#include "pbc/store.h"
#include "pbc/utf8.h"

#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "pbc/log.h"
#include "store_schema.h"
#include "store_sql.h"

namespace pbc {

using sql::exec;
using sql::query_text;

namespace {

bool file_size(const std::string& path, long long* size) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    *size = static_cast<long long>(st.st_size);
    return true;
}

bool copy_file(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) return false;
    out << in.rdbuf();
    out.flush();
    if (!out) return false;
    ::chmod(to.c_str(), 0600);   // the backup holds the same data as the database
    return true;
}

// Creates the directory with mode 0700 (and narrows an existing one).
void make_private_dir(const std::string& path) {
    ::mkdir(path.c_str(), 0700);
    ::chmod(path.c_str(), 0700);
}

Result<std::unique_ptr<Store>> fail(const char* key, const char* detail) {
    Result<std::unique_ptr<Store>> r;
    r.error = make_error(ErrorClass::Storage, key, detail);
    return r;
}

// user_version of an existing database file. The file is opened for reading AND writing so that SQLite can roll back the journal of a
// transaction that was cut short (the app was killed, the battery pulled); a read-only handle cannot do that and would report a perfectly
// good database as unreadable. Nothing is written otherwise.
bool read_user_version(const std::string& path, int* version) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            return false;
        }
    }
    std::string v, err;
    const bool ok = query_text(db, "PRAGMA user_version", &v, &err);
    sqlite3_close(db);
    if (ok) *version = std::atoi(v.c_str());
    return ok;
}

}  // namespace

Store::~Store() {
    if (db_) sqlite3_close(db_);
}

Result<std::unique_ptr<Store>> Store::open(const std::string& dir, Clock& clock, const StoreOptions& options) {
    const int target = options.target_version > 0 ? options.target_version : kSchemaVersion;

    make_private_dir(dir);
    const std::string path = dir + "/pbcloud.db";
    const std::string backup = path + ".bak";

    long long size = 0;
    const bool existed_nonempty = file_size(path, &size) && size > 0;
    if (existed_nonempty) {
        int stored = 0;
        if (!read_user_version(path, &stored)) return fail("open_failed", "the database cannot be read");
        if (stored > target) return fail("schema_too_new", "the database was written by a newer version of the app");
    }

    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return fail("open_failed", "the database cannot be opened");
    }
    ::chmod(path.c_str(), 0600);
    // the ordering of names (utf8.h: sort_key)
    sqlite3_create_function(db, "pbc_fold", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
                            [](sqlite3_context* ctx, int, sqlite3_value** argv) {
                                const unsigned char* t = sqlite3_value_text(argv[0]);
                                const std::string key = pbc::sort_key(t ? reinterpret_cast<const char*>(t) : "");
                                sqlite3_result_text(ctx, key.c_str(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
                            },
                            nullptr, nullptr);

    std::string value, err;
    if (!query_text(db, "PRAGMA journal_mode=TRUNCATE", &value, &err) ||
        !exec(db, "PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; PRAGMA secure_delete=ON;", &err) ||
        !query_text(db, "PRAGMA user_version", &value, &err)) {
        sqlite3_close(db);
        return fail("open_failed", "the database is unusable");
    }
    int version = std::atoi(value.c_str());
    if (version == 0) {   // a fresh database: create schema 1 in one transaction
        if (!exec(db, "BEGIN IMMEDIATE", &err) || !exec(db, kSchemaSql, &err) || !exec(db, "PRAGMA user_version=1", &err)) {
            exec(db, "ROLLBACK", nullptr);
            sqlite3_close(db);
            log::event("store.create_failed", {});
            return fail("open_failed", "the schema could not be created");
        }
        if (!exec(db, "COMMIT", &err)) {
            sqlite3_close(db);
            return fail("open_failed", "the schema could not be created");
        }
        version = 1;
    }

    bool upgraded = false;
    if (version < target) {   // one-way upgrade steps: the built-in ones, then those a test adds
        upgraded = true;
        if (existed_nonempty && !copy_file(path, backup)) {
            sqlite3_close(db);
            return fail("migration_failed", "the backup before the upgrade could not be written");
        }
        bool ok = exec(db, "BEGIN IMMEDIATE", &err);
        while (ok && version < target) {
            const Migration* step = nullptr;
            for (const Migration& m : builtin_migrations())
                if (m.from == version) step = &m;
            if (!step)
                for (const Migration& m : options.extra_migrations)
                    if (m.from == version) step = &m;
            if (!step) { ok = false; break; }
            ok = exec(db, step->sql.c_str(), &err) && exec(db, ("PRAGMA user_version=" + std::to_string(step->to)).c_str(), &err);
            version = step->to;
        }
        // the copy made above stays until the next successful start has opened the upgraded database (research R-07)
        if (ok && existed_nonempty) ok = exec(db, "INSERT OR REPLACE INTO settings(key, value) VALUES('schema_backup_pending', '1')", &err);
        if (ok) ok = exec(db, "COMMIT", &err);
        if (!ok) {
            exec(db, "ROLLBACK", nullptr);
            sqlite3_close(db);
            log::event("store.migration_failed", {});
            return fail("migration_failed", "the upgrade failed; your data was not changed");
        }
        if (!query_text(db, "PRAGMA integrity_check", &value, &err) || value != "ok") {
            sqlite3_close(db);
            return fail("integrity_failed", "the database failed its check after the upgrade");
        }
    }
    if (!upgraded) {   // the previous start upgraded the file and this one opened it: the copy of the old file is not needed any more
        std::string pending;
        if (query_text(db, "SELECT value FROM settings WHERE key = 'schema_backup_pending'", &pending, &err) && pending == "1") {
            std::remove(backup.c_str());
            exec(db, "DELETE FROM settings WHERE key = 'schema_backup_pending'", &err);
        }
    }

    Result<std::unique_ptr<Store>> r;
    r.value.reset(new Store(db, dir, clock, options.max_cached_entries, options.max_cached_folders));
    return r;
}

int Store::schema_version() const { return std::atoi(debug_query("PRAGMA user_version").c_str()); }

Error Store::check_integrity() const {
    const std::string v = debug_query("PRAGMA integrity_check");
    return v == "ok" ? Error() : make_error(ErrorClass::Storage, "integrity_failed", "the database failed its check");
}

std::string Store::debug_exec(const std::string& sql) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string err;
    return exec(db_, sql.c_str(), &err) ? std::string() : (err.empty() ? std::string("error") : err);
}

std::string Store::debug_query(const std::string& sql) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::string out, err;
    query_text(db_, sql, &out, &err);
    return out;
}

}  // namespace pbc
