// Validation before sending and the diagnosis of a refusal (specs/004-share-links, contracts/core-interfaces.md "share_diagnose.h", research
// R-4, data-model "Validation rules"): the message comes from the rules and the status, never from the server's text.
#include <string>
#include <type_traits>

#include "pbc/civil.h"
#include "pbc/share_date.h"
#include "pbc/share_diagnose.h"
#include "test_support.h"

namespace {

using pbc::LinkChange;
using pbc::LinkPermission;
using pbc::ShareMessage;

const int64_t kToday = pbc::civil::days_from_civil(2026, 10, 22);

pbc::ShareRules open_rules() {
    pbc::ShareRules r;
    r.api_enabled = true;
    r.public_enabled = true;
    r.upload_allowed = true;
    return r;
}

pbc::ShareTarget folder() { return {"/Docs/Reports", true}; }
pbc::ShareTarget file() { return {"/Docs/a.pdf", false}; }

pbc::LinkRequest request(LinkPermission p = LinkPermission::ReadOnly, const std::string& pw = "", std::optional<std::string> expiry = std::nullopt) {
    pbc::LinkRequest r;
    r.permission = p;
    if (!pw.empty()) r.password = pbc::SecretText{std::string(pw)};
    r.expiry = std::move(expiry);
    return r;
}

pbc::ShareRow own_link(int bits = 1, bool password = false, std::optional<std::string> expires = std::nullopt) {
    pbc::ShareRow row;
    row.id = 5;
    row.kind = pbc::ShareKind::PublicLink;
    row.permission_bits = bits;
    row.permission = pbc::link_permission_from_bits(bits, true);
    row.owned_by_me = true;
    row.has_password = password;
    row.expires = std::move(expires);
    return row;
}

LinkChange change(LinkChange::Action a) {
    LinkChange c;
    c.action = a;
    return c;
}

ShareMessage refusal(const pbc::ShareRules& rules, pbc::ShareOp op, long status, std::optional<bool> exists = std::nullopt, int ocs = 0,
                     const pbc::LinkRequest* req = nullptr, const LinkChange* ch = nullptr, const pbc::ShareRow* cur = nullptr, pbc::ShareTarget t = {"/Docs/Reports", true}) {
    pbc::RefusalContext c{rules, t, op, req, ch, cur, status, ocs, exists, kToday};
    return pbc::diagnose_refusal(c);
}

}  // namespace

static_assert(std::is_same<decltype(ShareMessage::key), std::string>::value, "the message key is a stable key");
// the message has no member that could hold the server's text: key, field(s), limit, short code only
static_assert(sizeof(ShareMessage) <= 3 * sizeof(std::string) + 32, "ShareMessage must not grow a free-text member");

PB_TEST(a_valid_request_gives_an_empty_message) {
    PB_CHECK(pbc::validate_create(open_rules(), folder(), request(), kToday).key.empty());
    PB_CHECK(pbc::validate_create(open_rules(), folder(), request(LinkPermission::ReadUpload, "pw", std::string("2026-12-31")), kToday).key.empty());
    PB_CHECK(pbc::validate_create(open_rules(), file(), request(), kToday).key.empty());
}

PB_TEST(a_file_cannot_get_upload_permission) {
    const auto m = pbc::validate_create(open_rules(), file(), request(LinkPermission::ReadUpload), kToday);
    PB_CHECK_EQ(m.key, std::string("permission_invalid"));
    PB_CHECK(m.field == ShareMessage::Field::Permission);
    PB_CHECK_EQ(pbc::validate_create(open_rules(), folder(), request(LinkPermission::UploadOnly), kToday).key, std::string("permission_invalid"));
}

PB_TEST(sharing_off_and_public_links_off_stop_a_create) {
    pbc::ShareRules off = open_rules();
    off.api_enabled = false;
    PB_CHECK_EQ(pbc::validate_create(off, folder(), request(), kToday).key, std::string("share_not_offered"));
    pbc::ShareRules pub = open_rules();
    pub.public_enabled = false;
    const auto m = pbc::validate_create(pub, folder(), request(), kToday);
    PB_CHECK_EQ(m.key, std::string("share_public_disabled"));
    PB_CHECK(m.field == ShareMessage::Field::None);
}

PB_TEST(a_required_password_must_be_given) {
    pbc::ShareRules r = open_rules();
    r.password_enforced = true;
    const auto m = pbc::validate_create(r, folder(), request(), kToday);
    PB_CHECK_EQ(m.key, std::string("share_password_required"));
    PB_CHECK(m.field == ShareMessage::Field::Password);
    PB_CHECK(pbc::validate_create(r, folder(), request(LinkPermission::ReadOnly, "pw"), kToday).key.empty());
    // per permission: only read-and-upload needs one
    pbc::ShareRules per = open_rules();
    per.password_enforced_read_only = false;
    per.password_enforced_read_write = true;
    PB_CHECK(pbc::validate_create(per, folder(), request(), kToday).key.empty());
    PB_CHECK_EQ(pbc::validate_create(per, folder(), request(LinkPermission::ReadUpload), kToday).key, std::string("share_password_required"));
}

PB_TEST(passwords_that_are_not_valid_are_refused_before_sending) {
    PB_CHECK_EQ(pbc::validate_create(open_rules(), folder(), request(LinkPermission::ReadOnly, std::string(257, 'a')), kToday).key, std::string("password_invalid"));
    PB_CHECK(pbc::validate_create(open_rules(), folder(), request(LinkPermission::ReadOnly, std::string(256, 'a')), kToday).key.empty());
    PB_CHECK_EQ(pbc::validate_create(open_rules(), folder(), request(LinkPermission::ReadOnly, "a\nb"), kToday).key, std::string("password_invalid"));
    PB_CHECK_EQ(pbc::validate_create(open_rules(), folder(), request(LinkPermission::ReadOnly, "   "), kToday).key, std::string("password_invalid"));
    const auto m = pbc::validate_create(open_rules(), folder(), request(LinkPermission::ReadOnly, std::string(300, 'a')), kToday);
    PB_CHECK(m.field == ShareMessage::Field::Password);
}

PB_TEST(a_required_expiry_must_be_given_and_inside_the_maximum) {
    pbc::ShareRules r = open_rules();
    r.expiry_enforced = true;
    r.expiry_days = 7;
    const auto none = pbc::validate_create(r, folder(), request(), kToday);
    PB_CHECK_EQ(none.key, std::string("share_expiry_required"));
    PB_CHECK(none.field == ShareMessage::Field::Expiry);
    PB_CHECK(none.limit_days == std::optional<int>(7));
    PB_CHECK(pbc::validate_create(r, folder(), request(LinkPermission::ReadOnly, "", std::string("2026-10-29")), kToday).key.empty());
    const auto far = pbc::validate_create(r, folder(), request(LinkPermission::ReadOnly, "", std::string("2026-10-30")), kToday);
    PB_CHECK_EQ(far.key, std::string("share_expiry_too_far"));
    PB_CHECK(far.limit_days == std::optional<int>(7) && far.field == ShareMessage::Field::Expiry);
    PB_CHECK_EQ(pbc::validate_create(r, folder(), request(LinkPermission::ReadOnly, "", std::string("2026-10-21")), kToday).key, std::string("expiry_in_past"));
    PB_CHECK_EQ(pbc::validate_create(r, folder(), request(LinkPermission::ReadOnly, "", std::string("next week")), kToday).key, std::string("expiry_invalid"));
}

PB_TEST(an_optional_expiry_is_still_checked_for_form_and_past) {
    pbc::ShareRules r = open_rules();
    r.expiry_enabled = true;
    r.expiry_days = 7;   // only a default when not enforced: no upper limit
    PB_CHECK(pbc::validate_create(r, folder(), request(LinkPermission::ReadOnly, "", std::string("2030-01-01")), kToday).key.empty());
    PB_CHECK_EQ(pbc::validate_create(r, folder(), request(LinkPermission::ReadOnly, "", std::string("2026-10-21")), kToday).key, std::string("expiry_in_past"));
}

PB_TEST(upload_while_not_allowed_is_refused) {
    pbc::ShareRules r = open_rules();
    r.upload_allowed = false;
    const auto m = pbc::validate_create(r, folder(), request(LinkPermission::ReadUpload), kToday);
    PB_CHECK_EQ(m.key, std::string("share_upload_not_allowed"));
    PB_CHECK(m.field == ShareMessage::Field::Permission);
    PB_CHECK(pbc::validate_create(r, folder(), request(LinkPermission::ReadOnly), kToday).key.empty());
}

PB_TEST(several_violations_report_the_first_key_and_every_field) {
    pbc::ShareRules r = open_rules();
    r.password_enforced = true;
    r.expiry_enforced = true;
    r.expiry_days = 14;
    r.upload_allowed = false;
    const auto m = pbc::validate_create(r, folder(), request(LinkPermission::ReadUpload), kToday);
    PB_CHECK_EQ(m.key, std::string("share_password_required"));
    PB_CHECK(m.field == ShareMessage::Field::Password);
    PB_CHECK_EQ(m.field_mask, unsigned(ShareMessage::kPassword | ShareMessage::kExpiry | ShareMessage::kPermission));
    PB_CHECK(m.limit_days == std::optional<int>(14));
}

PB_TEST(a_change_of_a_link_the_user_does_not_own_is_refused) {
    pbc::ShareRow row = own_link();
    row.owned_by_me = false;
    LinkChange c = change(LinkChange::Action::ClearExpiry);
    PB_CHECK_EQ(pbc::validate_change(open_rules(), folder(), row, c, kToday).key, std::string("share_not_changeable"));
    pbc::ShareRow user = own_link();
    user.kind = pbc::ShareKind::User;
    PB_CHECK_EQ(pbc::validate_change(open_rules(), folder(), user, c, kToday).key, std::string("share_not_changeable"));
}

PB_TEST(removing_a_required_password_or_expiry_is_refused) {
    pbc::ShareRules r = open_rules();
    r.password_enforced = true;
    r.expiry_enforced = true;
    r.expiry_days = 30;
    const auto pw = pbc::validate_change(r, folder(), own_link(1, true), change(LinkChange::Action::ClearPassword), kToday);
    PB_CHECK_EQ(pw.key, std::string("share_password_required"));
    PB_CHECK(pw.field == ShareMessage::Field::Password);
    const auto ex = pbc::validate_change(r, folder(), own_link(1, true, std::string("2026-11-01")), change(LinkChange::Action::ClearExpiry), kToday);
    PB_CHECK_EQ(ex.key, std::string("share_expiry_required"));
    PB_CHECK(ex.limit_days == std::optional<int>(30));
    // when nothing is forced both removals are fine
    PB_CHECK(pbc::validate_change(open_rules(), folder(), own_link(1, true), change(LinkChange::Action::ClearPassword), kToday).key.empty());
    PB_CHECK(pbc::validate_change(open_rules(), folder(), own_link(1, false, std::string("2026-11-01")), change(LinkChange::Action::ClearExpiry), kToday).key.empty());
}

PB_TEST(a_new_password_or_expiry_is_validated_like_a_create) {
    LinkChange c = change(LinkChange::Action::SetPassword);
    PB_CHECK_EQ(pbc::validate_change(open_rules(), folder(), own_link(), c, kToday).key, std::string("password_invalid"));   // empty
    c.password = pbc::SecretText{std::string("new-pw")};
    PB_CHECK(pbc::validate_change(open_rules(), folder(), own_link(), c, kToday).key.empty());
    pbc::ShareRules r = open_rules();
    r.expiry_enforced = true;
    r.expiry_days = 7;
    LinkChange e = change(LinkChange::Action::SetExpiry);
    e.expiry = "2026-10-30";
    PB_CHECK_EQ(pbc::validate_change(r, folder(), own_link(), e, kToday).key, std::string("share_expiry_too_far"));
    e.expiry = "2026-10-25";
    PB_CHECK(pbc::validate_change(r, folder(), own_link(), e, kToday).key.empty());
    e.expiry = "soon";
    PB_CHECK_EQ(pbc::validate_change(r, folder(), own_link(), e, kToday).key, std::string("expiry_invalid"));
}

PB_TEST(a_permission_change_is_checked_against_the_rules_of_the_new_permission) {
    LinkChange c = change(LinkChange::Action::SetPermission);
    c.permission = LinkPermission::ReadUpload;
    PB_CHECK_EQ(pbc::validate_change(open_rules(), file(), own_link(), c, kToday).key, std::string("permission_invalid"));
    pbc::ShareRules no_upload = open_rules();
    no_upload.upload_allowed = false;
    PB_CHECK_EQ(pbc::validate_change(no_upload, folder(), own_link(), c, kToday).key, std::string("share_upload_not_allowed"));
    pbc::ShareRules pw = open_rules();
    pw.password_enforced_read_write = true;
    PB_CHECK_EQ(pbc::validate_change(pw, folder(), own_link(1, false), c, kToday).key, std::string("share_password_required"));
    PB_CHECK(pbc::validate_change(pw, folder(), own_link(1, true), c, kToday).key.empty());
    c.permission = LinkPermission::ReadOnly;
    PB_CHECK(pbc::validate_change(pw, folder(), own_link(15, false), c, kToday).key.empty());
}

PB_TEST(a_refusal_finds_the_violated_rule_in_a_fixed_order) {
    const auto req = request();
    pbc::ShareRules pub = open_rules();
    pub.public_enabled = false;
    pub.password_enforced = true;
    PB_CHECK_EQ(refusal(pub, pbc::ShareOp::Create, 403, std::nullopt, 0, &req).key, std::string("share_public_disabled"));
    pbc::ShareRules off = open_rules();
    off.api_enabled = false;
    PB_CHECK_EQ(refusal(off, pbc::ShareOp::Create, 404, std::nullopt, 0, &req).key, std::string("share_not_offered"));
    pbc::ShareRules pw = open_rules();
    pw.password_enforced = true;
    pw.expiry_enforced = true;
    PB_CHECK_EQ(refusal(pw, pbc::ShareOp::Create, 400, std::nullopt, 0, &req).key, std::string("share_password_required"));
    pbc::ShareRules ex = open_rules();
    ex.expiry_enforced = true;
    ex.expiry_days = 7;
    const auto m = refusal(ex, pbc::ShareOp::Create, 400, std::nullopt, 0, &req);
    PB_CHECK_EQ(m.key, std::string("share_expiry_required"));
    PB_CHECK(m.limit_days == std::optional<int>(7) && m.field == ShareMessage::Field::Expiry);
    const auto far_req = request(LinkPermission::ReadOnly, "", std::string("2026-12-01"));
    PB_CHECK_EQ(refusal(ex, pbc::ShareOp::Create, 403, std::nullopt, 0, &far_req).key, std::string("share_expiry_too_far"));
    pbc::ShareRules up = open_rules();
    up.upload_allowed = false;
    const auto up_req = request(LinkPermission::ReadUpload);
    PB_CHECK_EQ(refusal(up, pbc::ShareOp::Create, 403, std::nullopt, 0, &up_req).key, std::string("share_upload_not_allowed"));
}

PB_TEST(without_a_violated_rule_a_create_is_not_shareable_or_gone) {
    const auto req = request();
    for (long status : {403L, 404L}) {
        PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Create, status, true, 0, &req).key, std::string("share_item_not_shareable"));
        PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Create, status, std::nullopt, 0, &req).key, std::string("share_item_not_shareable"));
        PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Create, status, false, 0, &req).key, std::string("share_item_gone"));
    }
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::List, 404, false).key, std::string("share_item_gone"));
}

PB_TEST(a_change_or_remove_without_a_violated_rule_is_not_changeable_or_gone) {
    const auto row = own_link();
    const auto c = change(LinkChange::Action::ClearExpiry);
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Change, 403, std::nullopt, 0, nullptr, &c, &row).key, std::string("share_not_changeable"));
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Change, 404, std::nullopt, 0, nullptr, &c, &row).key, std::string("share_gone"));
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Remove, 403).key, std::string("share_not_changeable"));
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Remove, 404).key, std::string("share_gone"));
}

PB_TEST(reading_one_share_that_is_gone_is_share_gone_and_other_refusals_have_a_code) {
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Detail, 404).key, std::string("share_gone"));
    const auto m = refusal(open_rules(), pbc::ShareOp::Detail, 403);
    PB_CHECK_EQ(m.key, std::string("share_refused"));
    PB_CHECK_EQ(m.code, std::string("E403"));
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Detail, 401).key, std::string("auth_failed"));
}

PB_TEST(everything_else_is_refused_with_a_short_code) {
    const auto req = request();
    const auto a = refusal(open_rules(), pbc::ShareOp::Create, 500, std::nullopt, 0, &req);
    PB_CHECK_EQ(a.key, std::string("share_refused"));
    PB_CHECK_EQ(a.code, std::string("E500"));
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Create, 400, std::nullopt, 0, &req).code, std::string("E400"));
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Remove, 409).code, std::string("E409"));
}

PB_TEST(a_200_with_a_failing_ocs_status_counts_as_that_status) {
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Remove, 200, std::nullopt, 404).key, std::string("share_gone"));
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Remove, 200, std::nullopt, 403).key, std::string("share_not_changeable"));
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Create, 200, std::nullopt, 997).code, std::string("E997"));
}

PB_TEST(login_and_rate_limit_statuses_keep_their_keys) {
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Create, 401).key, std::string("auth_failed"));
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::List, 429).key, std::string("rate_limited"));
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Change, 503).key, std::string("maintenance"));
    PB_CHECK_EQ(refusal(open_rules(), pbc::ShareOp::Create, 997, std::nullopt, 997).code, std::string("E997"));
}

PB_TEST_MAIN()
