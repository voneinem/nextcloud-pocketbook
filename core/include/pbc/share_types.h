#pragma once
// The entities of the share links feature (specs/004-share-links/data-model.md). In memory only: nothing here is stored. A ShareRow cannot
// hold a link (research R-5): only ShareDetail, produced for a single share the user asked for, carries one. Qt-free.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "pbc/secret_text.h"

namespace pbc {

// Bounds (data-model "ShareRow", "ShareDetail", "LinkRequest"; research R-14).
inline constexpr size_t kMaxShareRows = 500;
inline constexpr size_t kMaxShareLabelBytes = 64;
inline constexpr size_t kMaxShareLinkBytes = 512;
inline constexpr size_t kMaxSharePasswordBytes = 256;
inline constexpr size_t kMaxSharePathBytes = 4096;
inline constexpr long long kMaxShareResponseBytes = 2ll << 20;

// OCS share permission bits.
inline constexpr int kShareRead = 1;
inline constexpr int kShareUpdate = 2;
inline constexpr int kShareCreate = 4;
inline constexpr int kShareDelete = 8;
inline constexpr int kShareReshare = 16;
inline constexpr int kLinkReadOnlyBits = kShareRead;                                                           // 1
inline constexpr int kLinkReadUploadBits = kShareRead | kShareUpdate | kShareCreate | kShareDelete;             // 15

enum class ShareKind { PublicLink, User, Group, Other };
enum class LinkPermission { ReadOnly, ReadUpload, UploadOnly, Other };
enum class LinkState { Ok, DifferentAddress, NotSecure, Unreadable };

// The permission of a share from its bits and the kind of the item (data-model table; the reshare bit 16 is ignored).
LinkPermission link_permission_from_bits(int bits, bool is_folder);
// The bits sent for a permission offered for new links (ReadOnly -> 1, ReadUpload -> 15); 0 for anything else.
int link_bits_for(LinkPermission permission);

struct ShareRow {                       // cannot hold a link
    int64_t id = 0;
    ShareKind kind = ShareKind::Other;
    LinkPermission permission = LinkPermission::Other;
    int permission_bits = 0;
    std::optional<std::string> expires;   // "YYYY-MM-DD"
    bool expires_unreadable = false;
    bool has_password = false;            // links only
    std::string label;                    // links; sanitized, <= 64 bytes
    std::string with_name;                // users and groups; sanitized, <= 64 bytes
    bool owned_by_me = false;
    bool inherited = false;
    bool limited = false;                 // the answer lacks the fields needed to show the share
};

struct ShareDetail {
    ShareRow row;
    std::string link;                     // empty when link_state is Unreadable
    LinkState link_state = LinkState::Unreadable;
};

struct ShareTarget {
    std::string path;                     // a server path of feature 001 ("/Docs/a.pdf"), <= 4,096 bytes
    bool is_folder = false;
};

struct LinkRequest {                      // Create
    LinkPermission permission = LinkPermission::ReadOnly;   // ReadOnly | ReadUpload
    SecretText password;                  // optional (empty = none)
    std::optional<std::string> expiry;    // "YYYY-MM-DD"
};

struct LinkChange {                       // Change: exactly one action
    enum class Action { SetPassword, ClearPassword, SetExpiry, ClearExpiry, SetPermission };
    Action action = Action::SetPassword;
    SecretText password;                  // SetPassword
    std::string expiry;                   // SetExpiry, "YYYY-MM-DD"
    LinkPermission permission = LinkPermission::ReadOnly;   // SetPermission
};

// What the screen showed when the user decided; compared with a fresh read before a change (research R-7).
struct Snapshot {
    ShareKind kind = ShareKind::Other;
    int permission_bits = 0;
    std::optional<std::string> expires;
    bool has_password = false;
    bool owned_by_me = false;
    friend bool operator==(const Snapshot& a, const Snapshot& b) {
        return a.kind == b.kind && a.permission_bits == b.permission_bits && a.expires == b.expires && a.has_password == b.has_password && a.owned_by_me == b.owned_by_me;
    }
    friend bool operator!=(const Snapshot& a, const Snapshot& b) { return !(a == b); }
};
Snapshot snapshot_of(const ShareRow& row);

}  // namespace pbc
