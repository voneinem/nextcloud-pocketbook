#include "pbc/share_types.h"

namespace pbc {

LinkPermission link_permission_from_bits(int bits, bool is_folder) {
    const int b = bits & ~kShareReshare;
    if (b == kShareRead) return LinkPermission::ReadOnly;
    if (!is_folder) return LinkPermission::Other;
    if ((b & kShareRead) && (b & kShareCreate)) return LinkPermission::ReadUpload;
    if (b == kShareCreate) return LinkPermission::UploadOnly;
    return LinkPermission::Other;
}

int link_bits_for(LinkPermission permission) {
    switch (permission) {
        case LinkPermission::ReadOnly: return kLinkReadOnlyBits;
        case LinkPermission::ReadUpload: return kLinkReadUploadBits;
        default: return 0;
    }
}

Snapshot snapshot_of(const ShareRow& row) {
    Snapshot s;
    s.kind = row.kind;
    s.permission_bits = row.permission_bits;
    s.expires = row.expires;
    s.has_password = row.has_password;
    s.owned_by_me = row.owned_by_me;
    return s;
}

}  // namespace pbc
