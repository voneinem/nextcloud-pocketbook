#pragma once
// A fake Nextcloud written to a file and read back (kill tests of specs/003-folder-sync): the process that is killed saves what its server held, the
// process that checks the restart starts from exactly that, ETags and file ids included. A test helper only.

#include <cstdint>
#include <fstream>
#include <map>
#include <string>

#include "fake_nextcloud_server.h"

namespace pbtest {

inline void dump_put(std::ofstream& f, const std::string& s) {
    const uint64_t n = s.size();
    f.write(reinterpret_cast<const char*>(&n), sizeof n);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}
inline void dump_put(std::ofstream& f, int64_t v) { f.write(reinterpret_cast<const char*>(&v), sizeof v); }
inline std::string dump_get_string(std::ifstream& f) {
    uint64_t n = 0;
    f.read(reinterpret_cast<char*>(&n), sizeof n);
    std::string s(static_cast<size_t>(n), '\0');
    f.read(&s[0], static_cast<std::streamsize>(n));
    return s;
}
inline int64_t dump_get_int(std::ifstream& f) {
    int64_t v = 0;
    f.read(reinterpret_cast<char*>(&v), sizeof v);
    return v;
}

inline void save_server(const FakeNextcloud& server, const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    dump_put(f, static_cast<int64_t>(server.etag_counter()));
    dump_put(f, server.file_id_counter());
    dump_put(f, static_cast<int64_t>(server.nodes().size()));
    for (const auto& kv : server.nodes()) {
        const FakeNextcloud::Node& n = kv.second;
        dump_put(f, kv.first);
        dump_put(f, static_cast<int64_t>(n.folder));
        dump_put(f, n.content);
        dump_put(f, n.synthetic_size);
        dump_put(f, static_cast<int64_t>(n.synthetic_seed));
        dump_put(f, n.mtime);
        dump_put(f, n.etag);
        dump_put(f, n.fileid);
        dump_put(f, n.mime);
        dump_put(f, n.permissions);
        dump_put(f, static_cast<int64_t>(n.favorite));
        dump_put(f, n.checksum);
        dump_put(f, static_cast<int64_t>(n.encrypted));
        dump_put(f, n.listing_quirk);
        dump_put(f, static_cast<int64_t>(n.weak_etag));
        dump_put(f, static_cast<int64_t>(n.locked));
        dump_put(f, n.sha256);
    }
}

// false: there is no dump
inline bool load_server(FakeNextcloud& server, const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const uint64_t etags = static_cast<uint64_t>(dump_get_int(f));
    const int64_t ids = dump_get_int(f);
    const int64_t count = dump_get_int(f);
    std::map<std::string, FakeNextcloud::Node> nodes;
    for (int64_t i = 0; i < count; ++i) {
        const std::string key = dump_get_string(f);
        FakeNextcloud::Node n;
        n.folder = dump_get_int(f) != 0;
        n.content = dump_get_string(f);
        n.synthetic_size = dump_get_int(f);
        n.synthetic_seed = static_cast<uint8_t>(dump_get_int(f));
        n.mtime = dump_get_int(f);
        n.etag = dump_get_string(f);
        n.fileid = dump_get_int(f);
        n.mime = dump_get_string(f);
        n.permissions = dump_get_string(f);
        n.favorite = dump_get_int(f) != 0;
        n.checksum = dump_get_string(f);
        n.encrypted = dump_get_int(f) != 0;
        n.listing_quirk = dump_get_string(f);
        n.weak_etag = dump_get_int(f) != 0;
        n.locked = dump_get_int(f) != 0;
        n.sha256 = dump_get_string(f);
        nodes[key] = n;
    }
    server.restore(nodes, etags, ids);
    return true;
}

}  // namespace pbtest
