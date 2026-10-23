#include "pbc/share_rules.h"

#include <string_view>

namespace pbc {

namespace {

constexpr size_t kMaxMembersRead = 64;

// The first member with this key among the first kMaxMembersRead members of an object; nullptr otherwise.
const JsonValue* member(const JsonValue* object, std::string_view key) {
    if (!object || object->type != JsonValue::Type::Object) return nullptr;
    size_t n = 0;
    for (const auto& kv : object->members) {
        if (++n > kMaxMembersRead) break;
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

std::optional<bool> as_bool(const JsonValue* v) {
    if (!v || v->type != JsonValue::Type::Bool) return std::nullopt;
    return v->boolean;
}

}  // namespace

ShareRules share_rules_from_capabilities(const JsonValue& capabilities) {
    ShareRules r;
    const JsonValue* fs = member(&capabilities, "files_sharing");
    if (!fs) return r;
    r.api_enabled = as_bool(member(fs, "api_enabled"));
    r.resharing = as_bool(member(fs, "resharing"));
    const JsonValue* pub = member(fs, "public");
    r.public_enabled = as_bool(member(pub, "enabled"));
    r.upload_allowed = as_bool(member(pub, "upload"));
    const JsonValue* pw = member(pub, "password");
    r.password_enforced = as_bool(member(pw, "enforced"));
    const JsonValue* pf = member(pw, "enforced_for");
    r.password_enforced_read_only = as_bool(member(pf, "read_only"));
    r.password_enforced_read_write = as_bool(member(pf, "read_write"));
    r.password_enforced_upload_only = as_bool(member(pf, "upload_only"));
    const JsonValue* ex = member(pub, "expire_date");
    r.expiry_enabled = as_bool(member(ex, "enabled"));
    r.expiry_enforced = as_bool(member(ex, "enforced"));
    if (const JsonValue* d = member(ex, "days")) {
        const std::optional<int64_t> n = d->as_int();
        if (n && *n >= 1 && *n <= 3650) r.expiry_days = static_cast<int>(*n);
    }
    return r;
}

bool sharing_offered(const ShareRules& rules) { return rules.api_enabled != std::optional<bool>(false); }

bool link_creation_offered(const ShareRules& rules) { return sharing_offered(rules) && rules.public_enabled != std::optional<bool>(false); }

bool password_required(const ShareRules& rules, LinkPermission p) {
    std::optional<bool> specific;
    switch (p) {
        case LinkPermission::ReadOnly: specific = rules.password_enforced_read_only; break;
        case LinkPermission::ReadUpload: specific = rules.password_enforced_read_write; break;
        case LinkPermission::UploadOnly: specific = rules.password_enforced_upload_only; break;
        case LinkPermission::Other: break;
    }
    if (specific) return *specific;
    return rules.password_enforced == std::optional<bool>(true);
}

bool expiry_required(const ShareRules& rules) { return rules.expiry_enforced == std::optional<bool>(true); }

std::optional<int> expiry_max_days(const ShareRules& rules) {
    if (!expiry_required(rules)) return std::nullopt;
    return rules.expiry_days;
}

bool upload_offered(const ShareRules& rules, bool is_folder) { return is_folder && rules.upload_allowed != std::optional<bool>(false); }

}  // namespace pbc
