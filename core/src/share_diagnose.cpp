#include "pbc/share_diagnose.h"

#include <vector>

#include "pbc/share_date.h"

namespace pbc {

namespace {

struct Violation {
    const char* key;
    ShareMessage::Field field;
    unsigned bit;
};

// What a request would leave behind: used to ask the rules whether the result is allowed.
struct Outcome {
    LinkPermission permission = LinkPermission::ReadOnly;
    bool has_password = false;
    std::optional<std::string> expiry;
    bool check_password = true, check_expiry = true, check_upload = true;
};

ShareMessage make(const char* key, ShareMessage::Field f = ShareMessage::Field::None, unsigned mask = 0) {
    ShareMessage m;
    m.key = key;
    m.field = f;
    m.field_mask = mask;
    return m;
}

// The violated rules in the fixed order of research R-4: sharing off, public links off, password, expiry, upload.
std::vector<Violation> violations(const ShareRules& rules, const ShareTarget& target, const Outcome& o, int64_t today, bool include_public) {
    std::vector<Violation> v;
    if (!sharing_offered(rules)) {
        v.push_back({"share_not_offered", ShareMessage::Field::None, 0});
        return v;
    }
    if (include_public && !link_creation_offered(rules)) {
        v.push_back({"share_public_disabled", ShareMessage::Field::None, 0});
        return v;
    }
    if (o.check_password && password_required(rules, o.permission) && !o.has_password)
        v.push_back({"share_password_required", ShareMessage::Field::Password, ShareMessage::kPassword});
    if (o.check_expiry) {
        if (expiry_required(rules) && !o.expiry)
            v.push_back({"share_expiry_required", ShareMessage::Field::Expiry, ShareMessage::kExpiry});
        else if (o.expiry && check_expiry(*o.expiry, today, expiry_max_days(rules)).key == "share_expiry_too_far")
            v.push_back({"share_expiry_too_far", ShareMessage::Field::Expiry, ShareMessage::kExpiry});
    }
    if (o.check_upload && o.permission == LinkPermission::ReadUpload && !upload_offered(rules, target.is_folder))
        v.push_back({"share_upload_not_allowed", ShareMessage::Field::Permission, ShareMessage::kPermission});
    return v;
}

ShareMessage from_violations(const std::vector<Violation>& v, const ShareRules& rules) {
    ShareMessage m = make(v[0].key, v[0].field);
    bool expiry = false;
    for (const Violation& x : v) {
        m.field_mask |= x.bit;
        if (x.bit == ShareMessage::kExpiry) expiry = true;
    }
    if (expiry) m.limit_days = expiry_max_days(rules);
    return m;
}

ShareMessage check_password_text(const SecretText& pw, bool required_content) {
    if (pw.empty()) return required_content ? make("password_invalid", ShareMessage::Field::Password, ShareMessage::kPassword) : ShareMessage();
    if (pw.size() > kMaxSharePasswordBytes || pw.has_control_character() || pw.all_blank())
        return make("password_invalid", ShareMessage::Field::Password, ShareMessage::kPassword);
    return ShareMessage();
}

ShareMessage check_expiry_text(const std::string& text, int64_t today) {
    const Error e = check_expiry(text, today, std::nullopt);   // form and "not in the past"; the maximum is a rule
    if (!e.ok()) return make(e.key.c_str(), ShareMessage::Field::Expiry, ShareMessage::kExpiry);
    return ShareMessage();
}

bool permission_ok_for_link(LinkPermission p, bool is_folder) {
    return p == LinkPermission::ReadOnly || (p == LinkPermission::ReadUpload && is_folder);
}

}  // namespace

ShareMessage validate_create(const ShareRules& rules, const ShareTarget& target, const LinkRequest& request, int64_t today) {
    if (!permission_ok_for_link(request.permission, target.is_folder))
        return make("permission_invalid", ShareMessage::Field::Permission, ShareMessage::kPermission);
    if (ShareMessage m = check_password_text(request.password, false); !m.empty()) return m;
    if (request.expiry)
        if (ShareMessage m = check_expiry_text(*request.expiry, today); !m.empty()) return m;
    Outcome o;
    o.permission = request.permission;
    o.has_password = !request.password.empty();
    o.expiry = request.expiry;
    const auto v = violations(rules, target, o, today, true);
    return v.empty() ? ShareMessage() : from_violations(v, rules);
}

ShareMessage validate_change(const ShareRules& rules, const ShareTarget& target, const ShareRow& current, const LinkChange& change, int64_t today) {
    if (current.kind != ShareKind::PublicLink || !current.owned_by_me) return make("share_not_changeable");
    Outcome o;
    o.permission = current.permission;
    o.has_password = current.has_password;
    o.expiry = current.expires;
    o.check_password = o.check_expiry = o.check_upload = false;
    switch (change.action) {
        case LinkChange::Action::SetPassword:
            if (ShareMessage m = check_password_text(change.password, true); !m.empty()) return m;
            o.has_password = true;
            o.check_password = true;
            break;
        case LinkChange::Action::ClearPassword:
            o.has_password = false;
            o.check_password = true;
            break;
        case LinkChange::Action::SetExpiry:
            if (ShareMessage m = check_expiry_text(change.expiry, today); !m.empty()) return m;
            o.expiry = change.expiry;
            o.check_expiry = true;
            break;
        case LinkChange::Action::ClearExpiry:
            o.expiry.reset();
            o.check_expiry = true;
            break;
        case LinkChange::Action::SetPermission:
            if (!permission_ok_for_link(change.permission, target.is_folder))
                return make("permission_invalid", ShareMessage::Field::Permission, ShareMessage::kPermission);
            o.permission = change.permission;
            o.check_password = o.check_upload = true;
            break;
    }
    const auto v = violations(rules, target, o, today, true);
    return v.empty() ? ShareMessage() : from_violations(v, rules);
}

ShareMessage diagnose_refusal(const RefusalContext& c) {
    long status = c.http_status;
    if (status >= 200 && status < 300 && c.ocs_status >= 400) status = c.ocs_status;
    if (status == 401) return make("auth_failed");
    if (status == 429) return make("rate_limited");
    if (status == 503) return make("maintenance");

    if (c.op == ShareOp::Create && c.create) {
        Outcome o;
        o.permission = c.create->permission;
        o.has_password = !c.create->password.empty();
        o.expiry = c.create->expiry;
        const auto v = violations(c.fresh, c.target, o, c.today, true);
        if (!v.empty()) return from_violations(v, c.fresh);
    } else if (c.op == ShareOp::Change && c.change && c.current) {
        ShareMessage m = validate_change(c.fresh, c.target, *c.current, *c.change, c.today);
        if (!m.empty() && m.key != "share_not_changeable") return m;
    } else if (!sharing_offered(c.fresh)) {
        return make("share_not_offered");
    }

    const bool path_status = status == 403 || status == 404;
    if (c.op == ShareOp::Detail) {
        if (status == 404) return make("share_gone");
        ShareMessage m = make("share_refused");
        m.code = "E" + std::to_string(status);
        return m;
    }
    if (c.op == ShareOp::Create || c.op == ShareOp::List) {
        if (path_status) return make(c.item_exists == std::optional<bool>(false) ? "share_item_gone" : "share_item_not_shareable");
    } else {
        if (status == 403) return make("share_not_changeable");
        if (status == 404) return make("share_gone");
    }
    ShareMessage m = make("share_refused");
    m.code = "E" + std::to_string(status);
    return m;
}

}  // namespace pbc
