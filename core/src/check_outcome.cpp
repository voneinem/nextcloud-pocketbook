#include "pbc/check_outcome.h"

#include <algorithm>
#include <cctype>

#include "pbc/nc_listing.h"
#include "pbc/remote_path.h"

namespace pbc {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The server reports checksums as "SHA1:... MD5:..." (the algorithms it computed). Ours is compared only when it states the same algorithm.
bool checksum_differs(const std::string& reported, const std::string& sha256_hex) {
    if (sha256_hex.empty()) return false;
    const std::string r = lower(reported);
    const size_t at = r.find("sha256:");
    if (at == std::string::npos) return false;
    size_t end = r.find(' ', at);
    const std::string value = r.substr(at + 7, end == std::string::npos ? std::string::npos : end - at - 7);
    return value != lower(sha256_hex);
}

}  // namespace

Found check_outcome(DavSession& session, std::string_view user_id, const QueueItem& item) {
    if (item.kind == ItemKind::Delete) {   // nothing is made: the entry is gone or it is not
        const auto gone = fetch_entry(session, user_id, item.src_remote);
        if (!gone.ok()) return Found::Unclear;
        return gone.value ? Found::NotThere : Found::Arrived;
    }
    const std::string target = join_remote(item.dst_parent, item.dst_name);
    const auto there = fetch_entry(session, user_id, target);
    if (!there.ok()) return Found::Unclear;
    switch (item.kind) {
        case ItemKind::Upload: {
            if (!there.value) return Found::NotThere;
            const RemoteEntry& e = *there.value;
            if (e.kind != Kind::File || !e.size || !item.src_size || *e.size != *item.src_size) return Found::SomethingElse;
            if (checksum_differs(e.checksum, item.checksum)) return Found::SomethingElse;
            return Found::Arrived;
        }
        case ItemKind::Mkdir:
            if (!there.value) return Found::NotThere;
            return there.value->kind == Kind::Folder ? Found::Arrived : Found::SomethingElse;
        case ItemKind::Rename:
        case ItemKind::Move: {
            if (!there.value) return Found::NotThere;
            // a move keeps the file id: the entry at the target is ours when it carries the id the reader saw at the source
            if (!item.src_fileid || !there.value->fileid || *there.value->fileid != *item.src_fileid) return Found::SomethingElse;
            const auto source = fetch_entry(session, user_id, item.src_remote);
            if (!source.ok()) return Found::Unclear;
            return source.value ? Found::SomethingElse : Found::Arrived;
        }
        case ItemKind::Copy:
            // a copy has an id of its own: only that something of the same kind is there can be seen
            if (!there.value) return Found::NotThere;
            return there.value->kind == item.entry_kind ? Found::Arrived : Found::SomethingElse;
        case ItemKind::Delete: break;   // handled above
    }
    return Found::Unclear;
}

}  // namespace pbc
