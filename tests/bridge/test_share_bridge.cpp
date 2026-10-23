// The share bridge (specs/004-share-links, contracts/bridge-qml.md "ShareBridge"). This file grows with the stories: Phase 3 (US7) holds what the
// secrets need - a link and a password live only as long as the screen asked for them, and every copy the bridge made is wiped.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/folder_service.h"
#include "pbc/log.h"
#include "privacy_probe.h"
#include "pbc/store.h"
#include "share_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_share_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct FixedClock : pbc::Clock {
    int64_t t;
    explicit FixedClock(int64_t v) : t(v) {}
    int64_t now() const override { return t; }
};

const char* kLink = "https://cloud.example.org/s/TOKENforTheTest0123456789";

struct Env {
    pbtest::TmpDir dir;
    pbtest::FakeNextcloud server;
    FixedClock clock{server.server_time()};
    std::unique_ptr<pbc::Store> store;
    Worker worker;
    bool wifi = true;
    bool net_ok = true;          // what ensure_network answers (it also turns wifi on)
    bool net_hold = false;       // keep the answer of ensure_network until release_network()
    int ensure_calls = 0;
    std::function<void(bool)> held;
    bool clipboard_available = true;
    QStringList cleared_with;    // what the bridge asked the clipboard to clear
    QStringList copied;          // what the bridge put on the clipboard
    std::unique_ptr<ShareBridge> share;

    // `with_ensure`: the bridge is wired like main.cpp does it (a tap brings Wi-Fi up); without it a tap while offline is refused.
    explicit Env(bool with_ensure = false) {
        auto r = pbc::Store::open(dir.file("data"), clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store");
        store = std::move(r.value);
        set_account("demo");
        pbc::Credentials cr;
        cr.password = server.app_password();
        PB_CHECK(pbc::save_credentials(store->directory(), cr).ok());
        server.add_folder("/Docs");
        server.add_file("/Docs/a.pdf", "alpha");
        server.add_folder("/Docs/Reports");
        ShareBridge::Config c;
        c.store = store.get();
        c.worker = &worker;
        c.http = &server;
        c.clock = &clock;
        c.wifi_up = [this] { return wifi; };
        if (with_ensure) {
            c.ensure_network = [this](std::function<void(bool)> done) {
                ++ensure_calls;
                if (net_hold) {
                    held = std::move(done);
                    return;
                }
                if (net_ok) wifi = true;
                done(net_ok);
            };
        }
        c.set_clipboard = [this](const QString& s) { copied << QString::fromStdString(s.toStdString()); };   // a deep copy: the bridge wipes its own buffer in place
        c.clear_clipboard_if_equal = [this](const QString& s) { cleared_with << QString::fromStdString(s.toStdString()); };
        c.clipboard_available = clipboard_available;
        share = std::make_unique<ShareBridge>(c);
    }
    ~Env() {
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    void set_account(const std::string& user) {
        pbc::AccountRow a;
        a.base_url = server.base_url();
        a.login_name = "demo";
        a.user_id = user;
        a.state = "ok";
        PB_CHECK(store->save_account(a).ok());
    }
    void cache(const std::string& folder) {   // the listing the details page comes from
        pbc::FolderService fs(*store, server, clock);
        PB_CHECK(fs.load(folder, pbc::Fetch::Always).error.ok());
        server.clear_log();
    }
    void release_network(bool ok) {   // answers a held ensure_network
        net_hold = false;
        if (ok) wifi = true;
        auto done = std::move(held);
        held = nullptr;
        if (done) done(ok);
    }
    void wait() {
        QElapsedTimer t;
        t.start();
        while (share->busy() && t.elapsed() < 10000) QTest::qWait(5);
        QTest::qWait(20);
    }
    pbc::ShareDetail link_detail(bool password = false) {
        pbc::ShareDetail d;
        d.row.id = 7;
        d.row.kind = pbc::ShareKind::PublicLink;
        d.row.permission = pbc::LinkPermission::ReadOnly;
        d.row.permission_bits = 1;
        d.row.owned_by_me = true;
        d.row.has_password = password;
        d.link = kLink;
        d.link_state = pbc::LinkState::Ok;
        return d;
    }
};

bool all_nul(const QString& s) {
    if (s.isEmpty()) return false;
    for (QChar c : s)
        if (c != QChar(0)) return false;
    return true;
}

}  // namespace

PB_TEST(a_new_bridge_is_closed_and_holds_nothing) {
    Env e;
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("closed"));
    PB_CHECK(e.share->detail().isEmpty() && e.share->rows().isEmpty() && e.share->rules().isEmpty() && e.share->message().isEmpty());
    PB_CHECK(!e.share->busy());
    PB_CHECK(e.share->clipboardAvailable());
}

PB_TEST(clipboard_availability_comes_from_the_config_not_from_the_platform) {
    Env e;
    e.clipboard_available = false;
    ShareBridge::Config c;
    c.store = e.store.get();
    c.worker = &e.worker;
    c.http = &e.server;
    c.clock = &e.clock;
    c.clipboard_available = false;
    ShareBridge off(c);
    PB_CHECK(!off.clipboardAvailable());
    PB_CHECK(!off.copyLink());   // no link open and no clipboard
}

PB_TEST(a_detail_shows_the_link_without_the_password_state_leaking_anything_else) {
    Env e;
    e.share->setDetail(e.link_detail(true));
    const QVariantMap d = e.share->detail();
    PB_CHECK_EQ(d.value("linkText").toString().toStdString(), std::string(kLink));
    PB_CHECK_EQ(d.value("linkLength").toInt(), int(std::string(kLink).size()));
    PB_CHECK_EQ(d.value("linkState").toString().toStdString(), std::string("ok"));
    PB_CHECK(d.value("canChange").toBool());
    PB_CHECK(!d.value("passwordText").toString().isEmpty());
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("listed"));
    QStringList keys = d.keys();
    // the only password-named keys hold a state ("Password set", "Set", true), never a password
    for (const QString& k : keys) PB_CHECK(!k.contains("password", Qt::CaseInsensitive) || k == "passwordText" || k == "passwordValue" || k == "hasPassword");
    PB_CHECK(d.value("hasPassword").type() == QVariant::Bool);
}

PB_TEST(clear_secrets_empties_the_properties_and_wipes_every_copy_of_the_link) {
    Env e;
    e.share->setDetail(e.link_detail());
    QSignalSpy closed(e.share.get(), &ShareBridge::closed);
    const QString keep = e.share->detail().value("linkText").toString();   // a copy that shares the buffer, like the one the QML engine holds
    PB_CHECK_EQ(keep.toStdString(), std::string(kLink));
    e.share->clearSecrets();
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("closed"));
    PB_CHECK(e.share->detail().isEmpty() && e.share->rows().isEmpty() && e.share->rules().isEmpty() && e.share->message().isEmpty());
    PB_CHECK(all_nul(keep));   // the buffer that the copy shares was overwritten in place
    PB_CHECK_EQ(closed.count(), 1);
    e.share->clearSecrets();   // clearing a closed bridge is harmless and says nothing again
    PB_CHECK_EQ(closed.count(), 1);
}

PB_TEST(clearing_asks_the_clipboard_to_drop_exactly_the_link) {
    Env e;
    e.share->setDetail(e.link_detail());
    e.share->clearSecrets();
    PB_CHECK_EQ(e.cleared_with.size(), 1);
    PB_CHECK_EQ(e.cleared_with.at(0).toStdString(), std::string(kLink));
    Env none;
    none.share->clearSecrets();   // nothing was open: the clipboard is not touched
    PB_CHECK(none.cleared_with.isEmpty());
}

PB_TEST(copy_puts_exactly_the_link_on_the_clipboard_only_when_available) {
    Env e;
    PB_CHECK(!e.share->copyLink());   // no link open
    e.share->setDetail(e.link_detail());
    PB_CHECK(e.share->copyLink());
    PB_CHECK_EQ(e.copied.size(), 1);
    PB_CHECK_EQ(e.copied.at(0).toStdString(), std::string(kLink));
}

PB_TEST(leaving_the_application_clears_the_screen_state) {
    for (Qt::ApplicationState s : {Qt::ApplicationInactive, Qt::ApplicationSuspended, Qt::ApplicationHidden}) {
        Env e;
        e.share->setDetail(e.link_detail());
        e.share->onApplicationStateChanged(s);
        PB_CHECK_EQ(e.share->state().toStdString(), std::string("closed"));
        PB_CHECK(e.share->detail().isEmpty());
    }
    Env active;
    active.share->setDetail(active.link_detail());
    active.share->onApplicationStateChanged(Qt::ApplicationActive);
    PB_CHECK_EQ(active.share->state().toStdString(), std::string("listed"));
    PB_CHECK(!active.share->detail().isEmpty());
}

PB_TEST(a_changed_or_removed_account_clears_the_screen_state) {
    Env e;
    e.share->setDetail(e.link_detail());
    e.share->onConnectionChanged();   // nothing changed: the screen stays
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("listed"));
    e.set_account("someone-else");
    e.share->onConnectionChanged();
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("closed"));
    PB_CHECK(e.share->detail().isEmpty());
    Env gone;
    gone.share->setDetail(gone.link_detail());
    PB_CHECK(gone.store->clear_account().ok());
    gone.share->onConnectionChanged();
    PB_CHECK_EQ(gone.share->state().toStdString(), std::string("closed"));
}

// ---- US2: the list ----

namespace {

using pbtest::FakeNextcloud;

FakeNextcloud::Share link_on(const std::string& path, int perms = 1) {
    FakeNextcloud::Share s;
    s.path = path;
    s.permissions = perms;
    return s;
}

QStringList role_names(const QVariantList& rows) {
    QStringList names;
    for (const QVariant& v : rows)
        for (const QString& k : v.toMap().keys())
            if (!names.contains(k)) names << k;
    return names;
}

void mixed_shares(Env& e) {
    e.server.add_share(link_on("/Docs/Reports", 15));
    FakeNextcloud::Share user;
    user.type = 0;
    user.path = "/Docs/Reports";
    user.with = "bob";
    user.with_display = "Bob Example";
    e.server.add_share(user);
    FakeNextcloud::Share group = user;
    group.type = 1;
    group.with = "team";
    group.with_display = "Team Example";
    e.server.add_share(group);
    FakeNextcloud::Share mail = user;
    mail.type = 4;
    e.server.add_share(mail);
}

}  // namespace

PB_TEST(opening_a_folder_reads_rules_and_shares_and_fills_the_rows_without_a_link_role) {
    Env e;
    e.cache("/Docs");
    mixed_shares(e);
    const QVariantMap r = e.share->open("/Docs/Reports");
    PB_CHECK(r.value("ok").toBool());
    PB_CHECK(e.share->busy());
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("loading"));
    e.wait();
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("listed"));
    PB_CHECK_EQ(e.share->itemName().toStdString(), std::string("Reports"));
    PB_CHECK(e.share->isFolder() && e.share->itemShareable());
    PB_CHECK_EQ(e.server.share_log().size(), size_t(2));   // L2 and L3; L1 is the capabilities call
    PB_CHECK_EQ(e.server.request_count(), size_t(3));
    PB_CHECK_EQ(e.share->total(), 4);
    const QVariantList rows = e.share->rows();
    PB_CHECK_EQ(rows.size(), 4);
    const QStringList roles = role_names(rows);
    for (const char* forbidden : {"link", "linkText", "url", "token", "password"}) PB_CHECK(!roles.contains(QString::fromLatin1(forbidden)));
    for (const char* wanted : {"id", "kind", "title", "detailLine", "ownedByMe", "inherited", "limited", "canOpen"}) PB_CHECK(roles.contains(QString::fromLatin1(wanted)));
    PB_CHECK_EQ(rows.at(0).toMap().value("kind").toString().toStdString(), std::string("link"));
    PB_CHECK(rows.at(0).toMap().value("canOpen").toBool());
    for (int i = 1; i < 4; ++i) PB_CHECK(!rows.at(i).toMap().value("canOpen").toBool());
    PB_CHECK_EQ(rows.at(1).toMap().value("kind").toString().toStdString(), std::string("user"));
    PB_CHECK_EQ(rows.at(2).toMap().value("kind").toString().toStdString(), std::string("group"));
    PB_CHECK_EQ(rows.at(3).toMap().value("kind").toString().toStdString(), std::string("other"));
    PB_CHECK(e.share->hasExistingLink());
}

PB_TEST(no_row_of_the_bridge_holds_a_link_or_a_token_anywhere) {
    Env e;
    e.cache("/Docs");
    FakeNextcloud::Share link = link_on("/Docs/Reports");
    link.token = "TOKENzzzzzz999";
    link.password = "Zx9-secret-pw";
    e.server.add_share(link);
    e.share->open("/Docs/Reports");
    e.wait();
    PB_CHECK_EQ(e.share->rows().size(), 1);
    QString everything;
    for (const QVariant& v : e.share->rows())
        for (auto it = v.toMap().constBegin(); it != v.toMap().constEnd(); ++it) everything += it.key() + "=" + it.value().toString() + ";";
    for (auto it = e.share->rules().constBegin(); it != e.share->rules().constEnd(); ++it) everything += it.key() + "=" + it.value().toString() + ";";
    everything += e.share->itemName() + e.share->inheritedNote();
    for (const char* needle : {"TOKENzzzzzz999", "https://", "/s/", "Zx9-secret-pw", "$2y$"}) PB_CHECK(!everything.contains(QString::fromLatin1(needle)));
    PB_CHECK(e.share->detail().isEmpty());
}

PB_TEST(a_file_opens_the_same_way_and_says_it_is_not_a_folder) {
    Env e;
    e.cache("/Docs");
    e.share->open("/Docs/a.pdf");
    e.wait();
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("listed"));
    PB_CHECK(!e.share->isFolder());
    PB_CHECK(e.share->rows().isEmpty());
    PB_CHECK(!e.share->hasExistingLink());
    PB_CHECK(!e.share->rules().value("uploadOffered").toBool());   // a file never offers upload
}

PB_TEST(the_rules_reach_the_controls_and_unknown_values_are_marked_unknown) {
    Env e;
    e.cache("/Docs");
    e.server.set_password_enforced(true);
    e.server.set_expiry(true, true, 14);
    e.share->open("/Docs/Reports");
    e.wait();
    const QVariantMap rules = e.share->rules();
    PB_CHECK(rules.value("sharingOffered").toBool() && rules.value("linkCreationOffered").toBool());
    PB_CHECK(rules.value("passwordRequired").toBool() && rules.value("expiryRequired").toBool());
    PB_CHECK_EQ(rules.value("expiryMaxDays").toInt(), 14);
    PB_CHECK(rules.value("uploadOffered").toBool());
    PB_CHECK(rules.value("known").toMap().value("password").toBool());
    Env quiet;
    quiet.cache("/Docs");
    quiet.server.set_sharing_rules_in_caps(false);
    quiet.share->open("/Docs/Reports");
    quiet.wait();
    PB_CHECK(quiet.share->rules().value("sharingOffered").toBool());   // unknown counts as offered
    PB_CHECK(!quiet.share->rules().value("passwordRequired").toBool());
    for (const char* k : {"public", "password", "expiry", "upload"}) PB_CHECK(!quiet.share->rules().value("known").toMap().value(QString::fromLatin1(k)).toBool());
}

PB_TEST(page_turns_send_nothing_and_refresh_reads_again) {
    Env e;
    e.cache("/Docs");
    e.server.add_generated_shares("/Docs/Reports", 25);
    e.share->setPageSize(10);
    e.share->open("/Docs/Reports");
    e.wait();
    const size_t after_open = e.server.request_count();
    PB_CHECK_EQ(e.share->pageCount(), 3);
    PB_CHECK_EQ(e.share->rows().size(), 10);
    e.share->nextPage();
    e.share->nextPage();
    e.share->nextPage();   // no further
    PB_CHECK_EQ(e.share->page(), 2);
    PB_CHECK_EQ(e.share->rows().size(), 5);
    e.share->previousPage();
    PB_CHECK_EQ(e.share->page(), 1);
    PB_CHECK_EQ(e.server.request_count(), after_open);
    PB_CHECK(e.share->refresh().value("ok").toBool());
    e.wait();
    PB_CHECK_EQ(e.server.request_count(), after_open + 3);
    PB_CHECK_EQ(e.share->page(), 0);
}

PB_TEST(a_second_open_while_one_is_running_answers_busy_and_sends_nothing_more) {
    Env e;
    e.cache("/Docs");
    PB_CHECK(e.share->open("/Docs/Reports").value("ok").toBool());
    const QVariantMap second = e.share->open("/Docs/Reports");
    PB_CHECK_EQ(second.value("errorKey").toString().toStdString(), std::string("busy"));
    e.wait();
    PB_CHECK_EQ(e.server.request_count(), size_t(3));
    PB_CHECK_EQ(e.share->refresh().value("errorKey").toString().toStdString(), std::string(""));   // idle again: a refresh is fine
    e.wait();
}

PB_TEST(offline_nothing_is_sent_and_the_screen_says_so) {
    Env e;
    e.cache("/Docs");
    e.wifi = false;
    const QVariantMap r = e.share->open("/Docs/Reports");
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("no_network"));
    QTest::qWait(100);
    PB_CHECK(e.share->offline());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.share->message().value("key").toString().toStdString(), std::string("no_network"));
}

PB_TEST(when_wifi_drops_while_the_list_is_open_a_refresh_says_offline_and_marks_the_list_stale) {
    Env e;
    e.cache("/Docs");
    e.server.add_share(link_on("/Docs/Reports"));
    e.share->open("/Docs/Reports");
    e.wait();
    PB_CHECK(!e.share->staleNote());
    e.wifi = false;
    e.server.clear_log();
    const QVariantMap r = e.share->refresh();
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("no_network"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.share->staleNote());
    PB_CHECK_EQ(e.share->rows().size(), 1);   // the rows stay, flagged as possibly out of date
}

PB_TEST(an_unavailable_entry_is_not_shareable_and_nothing_is_sent) {
    Env e;
    e.server.set_unavailable("/Docs/Reports", "encrypted");
    e.cache("/Docs");
    const QVariantMap r = e.share->open("/Docs/Reports");
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("share_item_not_shareable"));
    PB_CHECK(!e.share->itemShareable());
    PB_CHECK(!e.share->unavailableReason().isEmpty());
    QTest::qWait(100);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    Env noperm;
    noperm.server.set_permissions("/Docs/a.pdf", "GDNVW");   // no reshare right
    noperm.cache("/Docs");
    PB_CHECK_EQ(noperm.share->open("/Docs/a.pdf").value("errorKey").toString().toStdString(), std::string("share_item_not_shareable"));
}

PB_TEST(an_entry_that_was_never_listed_cannot_be_opened) {
    Env e;
    const QVariantMap r = e.share->open("/Docs/Reports");
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("share_item_gone"));
    QTest::qWait(50);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(shares_of_other_owners_and_parent_folders_are_marked) {
    Env e;
    e.cache("/Docs");
    auto foreign = link_on("/Docs/Reports");
    foreign.owner = "carol";
    e.server.add_share(foreign);
    e.server.set_inherited_shares("/Docs/Reports", {link_on("/Docs")});
    e.share->open("/Docs/Reports");
    e.wait();
    const QVariantList rows = e.share->rows();
    PB_CHECK_EQ(rows.size(), 2);
    PB_CHECK(!rows.at(0).toMap().value("ownedByMe").toBool() && !rows.at(0).toMap().value("inherited").toBool());
    PB_CHECK(rows.at(1).toMap().value("ownedByMe").toBool() && rows.at(1).toMap().value("inherited").toBool());
    PB_CHECK_EQ(e.share->inheritedNote().toStdString(), std::string(""));
}

PB_TEST(a_failing_inherited_list_is_a_note_not_an_error) {
    Env e;
    e.cache("/Docs");
    e.server.add_share(link_on("/Docs/Reports"));
    e.server.answer_next("GET", "/shares/inherited", 500);
    e.share->open("/Docs/Reports");
    e.wait();
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("listed"));
    PB_CHECK_EQ(e.share->rows().size(), 1);
    PB_CHECK_EQ(e.share->inheritedNote().toStdString(), std::string("inherited_failed"));
}

PB_TEST(more_than_500_shares_set_the_truncated_flag) {
    Env e;
    e.cache("/Docs");
    e.server.add_generated_shares("/Docs/Reports", 600);
    e.share->open("/Docs/Reports");
    e.wait();
    PB_CHECK(e.share->truncated());
    PB_CHECK_EQ(e.share->total(), 500);
}

PB_TEST(sharing_switched_off_shows_the_message_and_no_rows) {
    Env e;
    e.cache("/Docs");
    e.server.set_sharing_enabled(false);
    e.share->open("/Docs/Reports");
    e.wait();
    PB_CHECK_EQ(e.share->message().value("key").toString().toStdString(), std::string("share_not_offered"));
    PB_CHECK(e.share->rows().isEmpty());
    PB_CHECK(!e.share->rules().value("sharingOffered").toBool());
}

PB_TEST(leaving_the_screen_drops_the_rows_and_opening_again_asks_the_server_again) {
    Env e;
    e.cache("/Docs");
    e.server.add_share(link_on("/Docs/Reports"));
    e.share->open("/Docs/Reports");
    e.wait();
    PB_CHECK_EQ(e.share->rows().size(), 1);
    e.share->clearSecrets();
    PB_CHECK(e.share->rows().isEmpty() && e.share->rules().isEmpty());
    e.server.clear_log();
    e.share->open("/Docs/Reports");
    e.wait();
    PB_CHECK_EQ(e.server.request_count(), size_t(3));
    PB_CHECK_EQ(e.share->rows().size(), 1);
}

PB_TEST(a_refused_login_shows_auth_failed) {
    Env e;
    e.cache("/Docs");
    pbc::Credentials cr;
    cr.password = "wrong-password";
    PB_CHECK(pbc::save_credentials(e.store->directory(), cr).ok());
    e.share->open("/Docs/Reports");
    e.wait();
    PB_CHECK_EQ(e.share->message().value("key").toString().toStdString(), std::string("auth_failed"));
}

PB_TEST(the_log_of_the_screen_holds_codes_and_counts_only) {
    Env e;
    e.cache("/Docs");
    e.server.add_share(link_on("/Docs/Reports"));
    std::vector<std::string> lines;
    pbc::log::set_sink([&](const std::string& l) { lines.push_back(l); });
    e.share->open("/Docs/Reports");
    e.wait();
    pbc::log::reset_sink();
    for (const auto& l : lines) PB_CHECK(l.find("Reports") == std::string::npos && l.find("TOK") == std::string::npos && l.find("demo") == std::string::npos);
}

// ---- US1: create ----

namespace {

QVariantMap create_options(const QString& permission = QStringLiteral("read"), const QString& password = QString(), const QString& expiry = QString(),
                           bool confirmed = false) {
    QVariantMap m;
    m.insert("permission", permission);
    m.insert("password", password);
    m.insert("expiry", expiry);
    m.insert("confirmed", confirmed);
    return m;
}

QString all_text(const QVariantMap& m) {
    QString t;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) t += it.key() + "=" + it.value().toString() + ";";
    return t;
}

void open_and_clear(Env& e, const std::string& path = "/Docs/Reports") {
    e.cache("/Docs");
    e.share->open(QString::fromStdString(path));
    e.wait();
    e.server.clear_log();
}

}  // namespace

PB_TEST(create_without_confirmation_says_what_the_link_allows_and_sends_nothing) {
    Env e;
    open_and_clear(e);
    const QString pw = QString::fromStdString(std::string("Zx9-secret-pw"));
    const QVariantMap r = e.share->create(create_options("upload", pw, "2026-12-31"));
    PB_CHECK(r.value("needsConfirm").toBool());
    const QString summary = r.value("summary").toString();
    PB_CHECK(summary.contains("add files"));            // the upload permission is stated in plain words
    PB_CHECK(summary.contains("password"));
    PB_CHECK(summary.contains("2026"));                 // the expiry date
    PB_CHECK(!summary.contains("Zx9-secret-pw"));       // the summary never repeats the password
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(pw.toStdString(), std::string("Zx9-secret-pw"));   // the preview does not consume the password: the page may still need it
    const QVariantMap plain = e.share->create(create_options());
    PB_CHECK(plain.value("summary").toString().contains("read this folder"));
    PB_CHECK(plain.value("summary").toString().contains("No password"));
}

PB_TEST(a_file_summary_says_file_and_never_mentions_upload) {
    Env e;
    e.cache("/Docs");
    e.share->open("/Docs/a.pdf");
    e.wait();
    const QString summary = e.share->create(create_options()).value("summary").toString();
    PB_CHECK(summary.contains("read this file"));
    PB_CHECK(!summary.contains("add files"));
}

PB_TEST(create_with_confirmation_sends_one_request_shows_the_link_and_reads_the_list_again) {
    Env e;
    open_and_clear(e);
    QSignalSpy finished(e.share.get(), &ShareBridge::createFinished);
    const QVariantMap r = e.share->create(create_options("upload", QString::fromStdString(std::string("Zx9-secret-pw")), "2026-12-31", true));
    PB_CHECK(r.value("ok").toBool());
    PB_CHECK(e.share->busy());
    e.wait();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK(finished.at(0).at(0).toBool());
    PB_CHECK_EQ(e.server.count("POST"), size_t(1));
    e.wait();
    PB_CHECK_EQ(e.server.request_count(), size_t(4));   // the POST, then the capabilities and the two lists
    const QVariantMap d = e.share->detail();
    PB_CHECK(d.value("linkText").toString().startsWith("https://"));
    PB_CHECK_EQ(d.value("linkState").toString().toStdString(), std::string("ok"));
    PB_CHECK(d.value("passwordText").toString().contains("Password"));
    PB_CHECK(e.share->hasExistingLink());
    PB_CHECK_EQ(e.share->rows().size(), 1);
}

PB_TEST(the_password_string_of_the_call_is_overwritten_when_it_is_sent_and_not_before) {
    Env e;
    open_and_clear(e);
    const QString pw = QString::fromStdString(std::string("Zx9-secret-pw"));
    e.share->create(create_options("read", pw, "", true));
    PB_CHECK(all_nul(pw));   // the buffer the QML engine shares with the map was wiped in place
    e.wait();
    e.wait();
    PB_CHECK(e.share->detail().value("linkText").toString().startsWith("https://"));
}

PB_TEST(input_the_rules_or_the_form_refuse_is_answered_at_once_and_sends_nothing) {
    Env e;
    e.cache("/Docs");
    e.server.set_password_enforced(true);
    e.server.set_expiry(true, true, 7);
    e.share->open("/Docs/Reports");
    e.wait();
    e.server.clear_log();
    const QVariantMap rules = e.share->rules();
    PB_CHECK(rules.value("passwordRequired").toBool() && rules.value("expiryRequired").toBool() && rules.value("expiryMaxDays").toInt() == 7);
    QVariantMap r = e.share->create(create_options("read", "", "", true));
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("share_password_required"));
    PB_CHECK_EQ(r.value("field").toString().toStdString(), std::string("password"));
    PB_CHECK_EQ(e.share->message().value("key").toString().toStdString(), std::string("share_password_required"));
    PB_CHECK_EQ(e.share->message().value("fieldMask").toInt(), 3);   // both the password and the expiry are marked
    PB_CHECK_EQ(e.share->message().value("limitDays").toInt(), 7);
    r = e.share->create(create_options("read", "pw", "soon", true));
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("expiry_invalid"));
    PB_CHECK_EQ(r.value("field").toString().toStdString(), std::string("expiry"));
    r = e.share->create(create_options("read", "pw", "2026-12-31", true));
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("share_expiry_too_far"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(!e.share->busy());
}

PB_TEST(upload_for_a_file_is_refused_at_once) {
    Env e;
    e.cache("/Docs");
    e.share->open("/Docs/a.pdf");
    e.wait();
    e.server.clear_log();
    const QVariantMap r = e.share->create(create_options("upload", "", "", true));
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("permission_invalid"));
    PB_CHECK_EQ(r.value("field").toString().toStdString(), std::string("permission"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(offline_a_create_sends_nothing_and_says_so) {
    Env e;
    open_and_clear(e);
    e.wifi = false;
    const QVariantMap r = e.share->create(create_options("read", "", "", true));
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("no_network"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.share->message().value("key").toString().toStdString(), std::string("no_network"));
}

PB_TEST(a_second_action_while_a_create_runs_answers_busy) {
    Env e;
    open_and_clear(e);
    PB_CHECK(e.share->create(create_options("read", "", "", true)).value("ok").toBool());
    PB_CHECK_EQ(e.share->create(create_options("read", "", "", true)).value("errorKey").toString().toStdString(), std::string("busy"));
    PB_CHECK_EQ(e.share->refresh().value("errorKey").toString().toStdString(), std::string("busy"));
    e.wait();
    e.wait();
    PB_CHECK_EQ(e.server.count("POST"), size_t(1));
}

PB_TEST(a_refusal_the_screen_did_not_know_is_explained_from_a_fresh_read_of_the_rules) {
    Env e;
    e.cache("/Docs");
    e.server.set_sharing_rules_in_caps(false);   // when the screen opened, the capabilities said nothing about sharing
    e.server.set_password_enforced(true);
    e.share->open("/Docs/Reports");
    e.wait();
    PB_CHECK(!e.share->rules().value("passwordRequired").toBool());
    e.server.set_sharing_rules_in_caps(true);    // ... and now they do: the refusal can be explained
    QSignalSpy finished(e.share.get(), &ShareBridge::createFinished);
    PB_CHECK(e.share->create(create_options("read", "", "", true)).value("ok").toBool());
    e.wait();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK(!finished.at(0).at(0).toBool());
    PB_CHECK_EQ(finished.at(0).at(1).toString().toStdString(), std::string("share_password_required"));
    PB_CHECK_EQ(e.share->message().value("key").toString().toStdString(), std::string("share_password_required"));
    PB_CHECK_EQ(e.share->message().value("field").toString().toStdString(), std::string("password"));
    PB_CHECK(e.share->detail().isEmpty());   // nothing is shown that was not created
    PB_CHECK_EQ(e.server.share_count(), size_t(0));
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("listed"));
    // a refusal nothing can explain is refused with its short code and no server text
    Env f;
    f.cache("/Docs");
    f.server.set_sharing_rules_in_caps(false);
    f.server.set_password_enforced(true);
    f.share->open("/Docs/Reports");
    f.wait();
    QSignalSpy finished2(f.share.get(), &ShareBridge::createFinished);
    f.share->create(create_options("read", "", "", true));
    f.wait();
    PB_CHECK_EQ(finished2.at(0).at(1).toString().toStdString(), std::string("share_refused"));
    PB_CHECK_EQ(f.share->message().value("code").toString().toStdString(), std::string("E400"));
}

PB_TEST(a_lost_answer_is_an_unknown_outcome_never_repeated_and_the_list_says_it_may_be_old) {
    Env e;
    open_and_clear(e);
    e.server.lose_next_answer("POST", "/shares");
    QSignalSpy finished(e.share.get(), &ShareBridge::createFinished);
    e.share->create(create_options("read", "Zx9-secret-pw", "", true));
    e.wait();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK_EQ(finished.at(0).at(1).toString().toStdString(), std::string("share_unknown_outcome"));
    PB_CHECK(e.share->staleNote());
    PB_CHECK_EQ(e.server.count("POST"), size_t(1));
    PB_CHECK_EQ(e.server.share_count(), size_t(1));   // the server did it
    PB_CHECK(e.share->detail().isEmpty());
    PB_CHECK(!all_text(e.share->message()).contains("Zx9-secret-pw"));
    PB_CHECK(e.share->refresh().value("ok").toBool());   // Refresh shows what exists and clears the note
    e.wait();
    PB_CHECK(!e.share->staleNote());
    PB_CHECK_EQ(e.share->rows().size(), 1);
}

PB_TEST(no_message_of_a_create_holds_the_password_or_a_link) {
    Env e;
    open_and_clear(e);
    e.server.set_password_enforced(true);
    e.share->open("/Docs/Reports");
    e.wait();
    e.share->create(create_options("read", "", "", true));
    const QString text = all_text(e.share->message());
    PB_CHECK(!text.contains("https://") && !text.contains("TOK"));
}

// ---- US3: show the link ----

namespace {

int64_t add_link(Env& e, const std::string& path = "/Docs/Reports", const std::string& url = std::string(), const std::string& owner = std::string()) {
    FakeNextcloud::Share s = link_on(path);
    s.url_override = url;
    s.owner = owner;
    return e.server.add_share(s);
}

}  // namespace

PB_TEST(showing_a_link_reads_that_one_share_and_fills_the_detail) {
    Env e;
    e.cache("/Docs");
    const int64_t id = add_link(e);
    e.share->open("/Docs/Reports");
    e.wait();
    e.server.clear_log();
    QSignalSpy opened(e.share.get(), &ShareBridge::linkOpened);
    const QVariantMap r = e.share->showLink(id);
    PB_CHECK(r.value("ok").toBool());
    e.wait();
    PB_CHECK_EQ(opened.count(), 1);
    PB_CHECK_EQ(e.server.request_count(), size_t(1));   // L4 only
    const QVariantMap d = e.share->detail();
    PB_CHECK_EQ(d.value("id").toLongLong(), qlonglong(id));
    PB_CHECK_EQ(d.value("linkText").toString().toStdString(), e.server.base_url() + "/s/" + e.server.find_share(id)->token);
    PB_CHECK_EQ(d.value("linkState").toString().toStdString(), std::string("ok"));
    PB_CHECK_EQ(d.value("linkLength").toInt(), d.value("linkText").toString().size());
    PB_CHECK(d.value("canChange").toBool());
    PB_CHECK_EQ(d.value("permissionText").toString().toStdString(), std::string("Read only"));
    PB_CHECK_EQ(d.value("passwordText").toString().toStdString(), std::string("No password"));
    PB_CHECK_EQ(d.value("expiryText").toString().toStdString(), std::string("No expiry"));
}

PB_TEST(the_link_is_in_the_detail_property_and_nowhere_else_of_the_bridge) {
    Env e;
    e.cache("/Docs");
    const int64_t id = add_link(e);
    e.share->open("/Docs/Reports");
    e.wait();
    e.share->showLink(id);
    e.wait();
    const QString token = QString::fromStdString(e.server.find_share(id)->token);
    PB_CHECK(e.share->detail().value("linkText").toString().contains(token));
    QString elsewhere = all_text(e.share->message()) + all_text(e.share->rules()) + e.share->itemName();
    for (const QVariant& v : e.share->rows()) elsewhere += all_text(v.toMap());
    PB_CHECK(!elsewhere.contains(token));
}

PB_TEST(a_link_of_someone_else_cannot_be_changed_but_is_shown) {
    Env e;
    e.cache("/Docs");
    const int64_t id = add_link(e, "/Docs/Reports", "", "carol");
    e.share->open("/Docs/Reports");
    e.wait();
    e.share->showLink(id);
    e.wait();
    PB_CHECK(!e.share->detail().value("canChange").toBool());
    PB_CHECK(!e.share->detail().value("linkText").toString().isEmpty());
}

PB_TEST(copy_puts_the_shown_link_on_the_clipboard_and_leaving_the_link_wipes_it_and_asks_the_clipboard_to_drop_it) {
    Env e;
    e.cache("/Docs");
    const int64_t id = add_link(e);
    e.share->open("/Docs/Reports");
    e.wait();
    e.share->showLink(id);
    e.wait();
    const std::string link = e.share->detail().value("linkText").toString().toStdString();
    PB_CHECK(e.share->copyLink());
    PB_CHECK_EQ(e.copied.size(), 1);
    PB_CHECK_EQ(e.copied.at(0).toStdString(), link);
    const QString keep = e.share->detail().value("linkText").toString();
    e.share->leaveLink();
    PB_CHECK(e.share->detail().isEmpty());
    PB_CHECK(all_nul(keep));
    PB_CHECK_EQ(e.cleared_with.size(), 1);
    PB_CHECK_EQ(e.cleared_with.at(0).toStdString(), link);
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("listed"));   // the list stays; only the link is gone
    PB_CHECK_EQ(e.share->rows().size(), 1);
    PB_CHECK(!e.share->copyLink());
}

PB_TEST(without_a_clipboard_nothing_can_be_copied_but_the_link_is_still_shown) {
    Env e;
    e.clipboard_available = false;
    ShareBridge::Config c;
    c.store = e.store.get();
    c.worker = &e.worker;
    c.http = &e.server;
    c.clock = &e.clock;
    c.clipboard_available = false;
    c.set_clipboard = [&e](const QString& s) { e.copied << s; };
    ShareBridge share(c);
    e.cache("/Docs");
    const int64_t id = add_link(e);
    share.open("/Docs/Reports");
    QElapsedTimer t;
    t.start();
    while (share.busy() && t.elapsed() < 10000) QTest::qWait(5);
    share.showLink(id);
    t.restart();
    while (share.busy() && t.elapsed() < 10000) QTest::qWait(5);
    QTest::qWait(30);
    PB_CHECK(!share.detail().value("linkText").toString().isEmpty());
    PB_CHECK(!share.copyLink());
    PB_CHECK(e.copied.isEmpty());
}

PB_TEST(a_link_on_another_host_is_flagged_and_a_broken_one_is_not_shown) {
    Env e;
    e.cache("/Docs");
    const int64_t other = add_link(e, "/Docs/Reports", "https://public.example.net/s/abc");
    const int64_t plain = add_link(e, "/Docs/Reports", std::string("http") + "://cloud.example.org/s/abc");
    const int64_t broken = add_link(e, "/Docs/Reports", "https://cloud.example.org/s/a b");
    e.share->open("/Docs/Reports");
    e.wait();
    e.share->showLink(other);
    e.wait();
    PB_CHECK_EQ(e.share->detail().value("linkState").toString().toStdString(), std::string("different_address"));
    PB_CHECK(e.share->detail().value("linkText").toString().contains("public.example.net"));
    PB_CHECK(e.share->copyLink());
    e.share->leaveLink();
    e.share->showLink(plain);
    e.wait();
    PB_CHECK_EQ(e.share->detail().value("linkState").toString().toStdString(), std::string("not_secure"));
    e.share->leaveLink();
    e.share->showLink(broken);
    e.wait();
    PB_CHECK_EQ(e.share->detail().value("linkState").toString().toStdString(), std::string("unreadable"));
    PB_CHECK(e.share->detail().value("linkText").toString().isEmpty());
    PB_CHECK(!e.share->copyLink());
}

PB_TEST(showing_the_same_link_again_asks_the_server_again) {
    Env e;
    e.cache("/Docs");
    const int64_t id = add_link(e);
    e.share->open("/Docs/Reports");
    e.wait();
    e.server.clear_log();
    e.share->showLink(id);
    e.wait();
    e.share->leaveLink();
    e.share->showLink(id);
    e.wait();
    PB_CHECK_EQ(e.server.request_count(), size_t(2));
}

PB_TEST(offline_or_busy_a_link_is_not_requested) {
    Env e;
    e.cache("/Docs");
    const int64_t id = add_link(e);
    e.share->open("/Docs/Reports");
    e.wait();
    e.server.clear_log();
    e.wifi = false;
    PB_CHECK_EQ(e.share->showLink(id).value("errorKey").toString().toStdString(), std::string("no_network"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.wifi = true;
    PB_CHECK(e.share->showLink(id).value("ok").toBool());
    PB_CHECK_EQ(e.share->showLink(id).value("errorKey").toString().toStdString(), std::string("busy"));
    e.wait();
}

PB_TEST(a_link_that_was_removed_meanwhile_shows_the_message_and_reads_the_list_again) {
    Env e;
    e.cache("/Docs");
    const int64_t id = add_link(e);
    e.share->open("/Docs/Reports");
    e.wait();
    PB_CHECK_EQ(e.share->rows().size(), 1);
    e.server.remove_share(id);
    e.share->showLink(id);
    e.wait();
    e.wait();
    PB_CHECK(e.share->detail().isEmpty());
    PB_CHECK_EQ(e.share->message().value("key").toString().toStdString(), std::string("share_gone"));
    PB_CHECK_EQ(e.share->rows().size(), 0);   // the list was read again and the link is not in it any more
}

PB_TEST(showing_and_copying_a_link_leave_no_trace_in_the_log_or_the_data_folder_after_the_page_is_left) {
    Env e;
    e.cache("/Docs");
    const int64_t id = add_link(e);
    e.share->open("/Docs/Reports");
    e.wait();
    pbtest::PrivacyProbe probe;
    e.share->showLink(id);
    e.wait();
    const std::string link = e.share->detail().value("linkText").toString().toStdString();
    const std::string token = e.server.find_share(id)->token;
    PB_CHECK(e.share->copyLink());
    probe.expect_only_in(token, link, e.dir.file("data"), "the link while its page is open");
    e.share->clearSecrets();
    probe.expect_absent(token, e.dir.file("data"), "the link after the page was left");
    PB_CHECK_EQ(e.server.link_hits(), size_t(0));   // the app never requested the link's address
}

// ---- US4: change a link ----

namespace {

QVariantMap change_options(const QString& action, const QString& password = QString(), const QString& expiry = QString(),
                           const QString& permission = QString(), bool confirmed = false) {
    QVariantMap m;
    m.insert("action", action);
    m.insert("password", password);
    m.insert("expiry", expiry);
    m.insert("permission", permission);
    m.insert("confirmed", confirmed);
    return m;
}

// opens Share on /Docs/Reports with one link made by the user and shows it
int64_t open_link(Env& e, int perms = 1, const std::string& password = std::string(), const std::string& owner = std::string()) {
    e.cache("/Docs");
    FakeNextcloud::Share s = link_on("/Docs/Reports", perms);
    s.password = password;
    s.owner = owner;
    const int64_t id = e.server.add_share(s);
    e.share->open("/Docs/Reports");
    e.wait();
    e.share->showLink(id);
    e.wait();
    e.server.clear_log();
    return id;
}

}  // namespace

PB_TEST(a_change_without_confirmation_states_its_effect_and_sends_nothing) {
    Env e;
    const int64_t id = open_link(e, 1, "old");
    const QString pw = QString::fromStdString(std::string("Zx9-secret-pw"));
    QVariantMap r = e.share->change(id, change_options("set_password", pw));
    PB_CHECK(r.value("needsConfirm").toBool());
    PB_CHECK(r.value("effectText").toString().contains("old password stops working"));
    PB_CHECK_EQ(pw.toStdString(), std::string("Zx9-secret-pw"));   // the preview does not consume the password
    r = e.share->change(id, change_options("clear_password"));
    PB_CHECK(r.value("effectText").toString().contains("without a password"));
    r = e.share->change(id, change_options("set_expiry", "", "2026-12-31"));
    PB_CHECK(r.value("effectText").toString().contains("2026-12-31"));
    r = e.share->change(id, change_options("clear_expiry"));
    PB_CHECK(r.value("effectText").toString().contains("not expire"));
    r = e.share->change(id, change_options("set_permission", "", "", "upload"));
    PB_CHECK(r.value("effectText").toString().contains("add files"));
    r = e.share->change(id, change_options("set_permission", "", "", "read"));
    PB_CHECK(r.value("effectText").toString().contains("only be able to read"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_confirmed_change_reads_checks_and_puts_once_and_shows_the_state_of_the_server) {
    Env e;
    const int64_t id = open_link(e);
    const std::string link = e.share->detail().value("linkText").toString().toStdString();
    QSignalSpy finished(e.share.get(), &ShareBridge::changeFinished);
    PB_CHECK(e.share->change(id, change_options("set_password", QString::fromStdString(std::string("Zx9-secret-pw")), "", "", true)).value("ok").toBool());
    PB_CHECK(e.share->busy());
    e.wait();
    e.wait();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK(finished.at(0).at(0).toBool());
    PB_CHECK_EQ(e.server.count("PUT"), size_t(1));
    PB_CHECK(e.server.count("GET") >= size_t(4));   // the pre-check of the share, then the capabilities and the two lists again
    PB_CHECK_EQ(e.share->detail().value("passwordText").toString().toStdString(), std::string("Password set"));
    PB_CHECK_EQ(e.share->detail().value("linkText").toString().toStdString(), link);   // the link text does not change
    PB_CHECK(e.server.find_share(id)->password == "Zx9-secret-pw");
}

PB_TEST(the_password_string_is_overwritten_when_the_change_is_sent) {
    Env e;
    const int64_t id = open_link(e);
    const QString pw = QString::fromStdString(std::string("Zx9-secret-pw"));
    e.share->change(id, change_options("set_password", pw, "", "", true));
    PB_CHECK(all_nul(pw));
    e.wait();
    e.wait();
}

PB_TEST(a_link_changed_by_someone_else_is_not_overwritten_and_the_current_state_is_shown) {
    Env e;
    const int64_t id = open_link(e);
    const std::string link = e.share->detail().value("linkText").toString().toStdString();
    e.server.set_share_password(id, "theirs");
    QSignalSpy finished(e.share.get(), &ShareBridge::changeFinished);
    e.share->change(id, change_options("set_expiry", "", "2026-12-31", "", true));
    e.wait();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK_EQ(finished.at(0).at(1).toString().toStdString(), std::string("share_changed_meanwhile"));
    PB_CHECK_EQ(e.server.count("PUT"), size_t(0));
    PB_CHECK_EQ(e.share->message().value("key").toString().toStdString(), std::string("share_changed_meanwhile"));
    PB_CHECK_EQ(e.share->detail().value("passwordText").toString().toStdString(), std::string("Password set"));   // the current state of the server
    PB_CHECK_EQ(e.share->detail().value("linkText").toString().toStdString(), link);
    PB_CHECK(e.server.find_share(id)->expiration.empty());
    // after looking at the current state the user may confirm again
    e.share->change(id, change_options("set_expiry", "", "2026-12-31", "", true));
    e.wait();
    e.wait();
    PB_CHECK_EQ(e.server.count("PUT"), size_t(1));
}

PB_TEST(a_link_of_someone_else_cannot_be_changed_and_nothing_is_sent) {
    Env e;
    const int64_t id = open_link(e, 1, "", "carol");
    const QVariantMap r = e.share->change(id, change_options("set_expiry", "", "2026-12-31", "", true));
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("share_not_changeable"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_change_the_rules_or_the_form_refuse_is_answered_at_once) {
    Env e;
    e.server.set_password_enforced(true);
    e.server.set_expiry(true, true, 7);
    const int64_t id = open_link(e, 1, "old");
    QVariantMap r = e.share->change(id, change_options("clear_password", "", "", "", true));
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("share_password_required"));
    PB_CHECK_EQ(r.value("field").toString().toStdString(), std::string("password"));
    r = e.share->change(id, change_options("set_expiry", "", "soon", "", true));
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("expiry_invalid"));
    r = e.share->change(id, change_options("set_expiry", "", "2026-12-31", "", true));
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("share_expiry_too_far"));
    r = e.share->change(id, change_options("set_password", "", "", "", true));
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("password_invalid"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(offline_busy_and_a_link_that_is_not_open_are_answered_without_a_request) {
    Env e;
    const int64_t id = open_link(e);
    e.wifi = false;
    PB_CHECK_EQ(e.share->change(id, change_options("clear_expiry", "", "", "", true)).value("errorKey").toString().toStdString(), std::string("no_network"));
    e.wifi = true;
    PB_CHECK_EQ(e.share->change(id + 99, change_options("clear_expiry", "", "", "", true)).value("errorKey").toString().toStdString(), std::string("share_item_gone"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.share->change(id, change_options("clear_expiry", "", "", "", true)).value("ok").toBool());
    PB_CHECK_EQ(e.share->change(id, change_options("clear_expiry", "", "", "", true)).value("errorKey").toString().toStdString(), std::string("busy"));
    e.wait();
    e.wait();
}

PB_TEST(removing_a_password_and_an_expiry_works_and_the_state_follows) {
    Env e;
    const int64_t id = open_link(e, 1, "old");
    e.server.set_share_expiration(id, "2026-12-31");
    e.share->showLink(id);   // read the state again
    e.wait();
    PB_CHECK_EQ(e.share->detail().value("passwordText").toString().toStdString(), std::string("Password set"));
    e.share->change(id, change_options("clear_password", "", "", "", true));
    e.wait();
    e.wait();
    PB_CHECK_EQ(e.share->detail().value("passwordText").toString().toStdString(), std::string("No password"));
    e.share->change(id, change_options("clear_expiry", "", "", "", true));
    e.wait();
    e.wait();
    PB_CHECK_EQ(e.share->detail().value("expiryText").toString().toStdString(), std::string("No expiry"));
    e.share->change(id, change_options("set_permission", "", "", "upload", true));
    e.wait();
    e.wait();
    PB_CHECK_EQ(e.share->detail().value("permissionText").toString().toStdString(), std::string("Read and upload"));
}

PB_TEST(no_message_of_a_change_holds_the_password) {
    Env e;
    const int64_t id = open_link(e);
    e.server.set_password_enforced(true);
    e.share->change(id, change_options("clear_password", "Zx9-secret-pw", "", "", true));
    e.wait();
    PB_CHECK(!all_text(e.share->message()).contains("Zx9-secret-pw"));
}

// ---- US5: remove a link ----

PB_TEST(a_removal_without_confirmation_names_the_item_states_the_effect_and_sends_nothing) {
    Env e;
    const int64_t id = open_link(e);
    const QVariantMap r = e.share->remove(id, false);
    PB_CHECK(r.value("needsConfirm").toBool());
    const QString text = r.value("effectText").toString();
    PB_CHECK(text.contains("Reports"));
    PB_CHECK(text.contains("stops working at once"));
    PB_CHECK(text.contains("is not deleted"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.server.share_count(), size_t(1));
}

PB_TEST(a_confirmed_removal_sends_one_delete_drops_the_link_and_reads_the_list_again) {
    Env e;
    const int64_t id = open_link(e);
    const QString token = QString::fromStdString(e.server.find_share(id)->token);
    const QString link = e.share->detail().value("linkText").toString();
    PB_CHECK(link.contains(token));
    QSignalSpy finished(e.share.get(), &ShareBridge::removeFinished);
    PB_CHECK(e.share->remove(id, true).value("ok").toBool());
    PB_CHECK(e.share->busy());
    e.wait();
    e.wait();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK(finished.at(0).at(0).toBool());
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(1));
    PB_CHECK_EQ(e.server.share_count(), size_t(0));
    PB_CHECK(e.share->detail().isEmpty());
    PB_CHECK_EQ(e.share->rows().size(), 0);
    PB_CHECK(e.server.has("/Docs/Reports"));   // the shared item is untouched
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(0));
}

PB_TEST(a_link_that_was_already_removed_is_reported_as_removed_and_the_list_is_shown_again) {
    Env e;
    const int64_t id = open_link(e);
    e.server.remove_share(id);
    QSignalSpy finished(e.share.get(), &ShareBridge::removeFinished);
    e.share->remove(id, true);
    e.wait();
    e.wait();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK(finished.at(0).at(0).toBool());                                                   // not a failure
    PB_CHECK_EQ(finished.at(0).at(1).toString().toStdString(), std::string("share_gone"));    // but the page can say "already removed"
    PB_CHECK(e.share->detail().isEmpty());
    PB_CHECK_EQ(e.share->rows().size(), 0);
}

PB_TEST(a_link_of_someone_else_cannot_be_removed_and_nothing_is_sent) {
    Env e;
    const int64_t id = open_link(e, 1, "", "carol");
    const QVariantMap r = e.share->remove(id, true);
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("share_not_changeable"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(offline_busy_and_a_link_that_is_not_open_are_answered_for_a_removal_without_a_request) {
    Env e;
    const int64_t id = open_link(e);
    e.wifi = false;
    PB_CHECK_EQ(e.share->remove(id, true).value("errorKey").toString().toStdString(), std::string("no_network"));
    e.wifi = true;
    PB_CHECK_EQ(e.share->remove(id + 99, true).value("errorKey").toString().toStdString(), std::string("share_item_gone"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.share->remove(id, true).value("ok").toBool());
    PB_CHECK_EQ(e.share->remove(id, true).value("errorKey").toString().toStdString(), std::string("busy"));
    e.wait();
    e.wait();
}

PB_TEST(a_refused_removal_keeps_the_link_and_says_why) {
    Env e;
    const int64_t id = open_link(e);
    e.server.answer_next("DELETE", "/shares", 500);
    QSignalSpy finished(e.share.get(), &ShareBridge::removeFinished);
    e.share->remove(id, true);
    e.wait();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK(!finished.at(0).at(0).toBool());
    PB_CHECK_EQ(e.share->message().value("key").toString().toStdString(), std::string("share_refused"));
    PB_CHECK_EQ(e.share->message().value("code").toString().toStdString(), std::string("E500"));
    PB_CHECK(!e.share->detail().isEmpty());   // the link is still open and still there
    PB_CHECK_EQ(e.server.share_count(), size_t(1));
}

PB_TEST(a_lost_answer_of_a_removal_is_an_unknown_outcome_and_is_not_repeated) {
    Env e;
    const int64_t id = open_link(e);
    e.server.lose_next_answer("DELETE", "/shares");
    QSignalSpy finished(e.share.get(), &ShareBridge::removeFinished);
    e.share->remove(id, true);
    e.wait();
    PB_CHECK_EQ(finished.at(0).at(1).toString().toStdString(), std::string("share_unknown_outcome"));
    PB_CHECK(e.share->staleNote());
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(1));
    PB_CHECK_EQ(e.server.share_count(), size_t(0));   // the server did it
    e.share->refresh();
    e.wait();
    PB_CHECK_EQ(e.share->rows().size(), 0);
    PB_CHECK(!e.share->staleNote());
}

PB_TEST(the_link_of_a_removed_share_is_found_nowhere_after_the_page_is_left) {
    Env e;
    const int64_t id = open_link(e);
    const std::string token = e.server.find_share(id)->token;
    pbtest::PrivacyProbe probe;
    e.share->remove(id, true);
    e.wait();
    e.wait();
    e.share->clearSecrets();
    probe.expect_absent(token, e.dir.file("data"), "the token of a removed share");
    PB_CHECK(e.cleared_with.size() >= 1);   // the clipboard was asked to drop the link when the link page's text was dropped
    PB_CHECK_EQ(e.server.link_hits(), size_t(0));
}

// ---- US6: messages ----

PB_TEST(a_refusal_has_its_own_key_and_no_server_text_in_the_message_map) {
    Env e;
    e.cache("/Docs");
    e.share->open("/Docs/Reports");
    e.wait();
    e.server.set_unshareable("/Docs/Reports");   // the server forbids sharing this item (the screen could not know)
    QSignalSpy finished(e.share.get(), &ShareBridge::createFinished);
    e.share->create(create_options("read", "", "", true));
    e.wait();
    PB_CHECK_EQ(finished.at(0).at(1).toString().toStdString(), std::string("share_item_not_shareable"));
    const QVariantMap m = e.share->message();
    PB_CHECK_EQ(m.value("key").toString().toStdString(), std::string("share_item_not_shareable"));
    PB_CHECK(m.contains("field") && m.contains("fieldMask") && m.contains("limitDays") && m.contains("code"));
    for (const QString& v : {m.value("key").toString(), m.value("code").toString(), m.value("field").toString()})
        PB_CHECK(!v.contains("allowed") && !v.contains("share this item"));   // the wording of the server never gets here
}

PB_TEST(an_item_that_is_gone_since_the_listing_says_so_and_nothing_is_created) {
    Env e;
    e.cache("/Docs");
    e.share->open("/Docs/Reports");
    e.wait();
    e.server.remove("/Docs/Reports");
    QSignalSpy finished(e.share.get(), &ShareBridge::createFinished);
    e.share->create(create_options("read", "", "", true));
    e.wait();
    PB_CHECK_EQ(finished.at(0).at(1).toString().toStdString(), std::string("share_item_gone"));
    PB_CHECK_EQ(e.server.share_count(), size_t(0));
}

PB_TEST(a_forced_password_and_expiry_report_both_fields_and_the_maximum) {
    Env e;
    e.cache("/Docs");
    e.server.set_password_enforced(true);
    e.server.set_expiry(true, true, 14);
    e.share->open("/Docs/Reports");
    e.wait();
    e.share->create(create_options("read", "", "", true));
    const QVariantMap m = e.share->message();
    PB_CHECK_EQ(m.value("key").toString().toStdString(), std::string("share_password_required"));
    PB_CHECK_EQ(m.value("field").toString().toStdString(), std::string("password"));
    PB_CHECK_EQ(m.value("fieldMask").toInt(), 3);
    PB_CHECK_EQ(m.value("limitDays").toInt(), 14);
}

PB_TEST(the_message_is_dropped_when_the_screen_is_left_or_cleared) {
    Env e;
    e.cache("/Docs");
    e.server.set_password_enforced(true);
    e.share->open("/Docs/Reports");
    e.wait();
    e.share->create(create_options("read", "", "", true));
    PB_CHECK(!e.share->message().isEmpty());
    e.share->clearMessage();
    PB_CHECK(e.share->message().isEmpty());
    e.share->create(create_options("read", "", "", true));
    e.share->clearSecrets();
    PB_CHECK(e.share->message().isEmpty());
}


// ---- a tap brings Wi-Fi up (like the files client); never by itself, nothing is queued ----

PB_TEST(a_tap_while_offline_brings_wifi_up_and_the_list_is_read) {
    Env e(true);
    e.cache("/Docs");
    e.wifi = false;
    PB_CHECK(e.share->open("/Docs/Reports").value("ok").toBool());
    e.wait();
    PB_CHECK_EQ(e.ensure_calls, 1);
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("listed"));
    PB_CHECK(e.share->message().isEmpty());
    PB_CHECK(e.server.request_count() >= size_t(2));
}

PB_TEST(a_tap_with_wifi_already_on_does_not_ask_the_system) {
    Env e(true);
    e.cache("/Docs");
    e.share->open("/Docs/Reports");
    e.wait();
    PB_CHECK_EQ(e.ensure_calls, 0);
}

PB_TEST(when_wifi_does_not_come_up_the_list_says_so_and_nothing_is_sent_or_queued) {
    Env e(true);
    e.cache("/Docs");
    e.wifi = false;
    e.net_ok = false;
    PB_CHECK(e.share->open("/Docs/Reports").value("ok").toBool());
    QTest::qWait(50);
    PB_CHECK(!e.share->busy());
    PB_CHECK_EQ(e.share->message().value("key").toString().toStdString(), std::string("no_network"));
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("failed"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.wifi = true;   // and when it comes up later nothing follows on its own
    QTest::qWait(200);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_create_while_offline_brings_wifi_up_then_sends_one_request) {
    Env e(true);
    open_and_clear(e);
    e.wifi = false;
    QSignalSpy finished(e.share.get(), &ShareBridge::createFinished);
    PB_CHECK(e.share->create(create_options("read", QString::fromStdString(std::string("Zx9-secret-pw")), "", true)).value("ok").toBool());
    e.wait();
    e.wait();
    PB_CHECK_EQ(e.ensure_calls, 1);
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK(finished.at(0).at(0).toBool());
    PB_CHECK_EQ(e.server.count("POST"), size_t(1));
}

PB_TEST(a_create_when_wifi_does_not_come_up_sends_nothing_and_reports_no_network) {
    Env e(true);
    open_and_clear(e);
    e.wifi = false;
    e.net_ok = false;
    QSignalSpy finished(e.share.get(), &ShareBridge::createFinished);
    PB_CHECK(e.share->create(create_options("read", QString::fromStdString(std::string("Zx9-secret-pw")), "", true)).value("ok").toBool());
    e.wait();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK(!finished.at(0).at(0).toBool());
    PB_CHECK_EQ(finished.at(0).at(1).toString().toStdString(), std::string("no_network"));
    PB_CHECK(!e.share->busy());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(while_wifi_is_being_brought_up_a_second_tap_answers_busy_and_the_focus_loss_of_the_system_dialog_keeps_the_screen) {
    Env e(true);
    open_and_clear(e);
    e.wifi = false;
    e.net_hold = true;
    QSignalSpy finished(e.share.get(), &ShareBridge::createFinished);
    PB_CHECK(e.share->create(create_options("read", "", "", true)).value("ok").toBool());
    PB_CHECK(e.share->busy());
    PB_CHECK_EQ(e.share->create(create_options("read", "", "", true)).value("errorKey").toString().toStdString(), std::string("busy"));
    e.share->onApplicationStateChanged(Qt::ApplicationInactive);   // the system's own Wi-Fi dialog takes the focus
    PB_CHECK(e.share->state() != QLatin1String("closed"));
    e.release_network(true);
    e.wait();
    e.wait();
    PB_CHECK_EQ(e.ensure_calls, 1);
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK(finished.at(0).at(0).toBool());
    PB_CHECK_EQ(e.server.count("POST"), size_t(1));
}

PB_TEST(a_screen_left_while_wifi_is_being_brought_up_sends_nothing_when_it_arrives) {
    Env e(true);
    open_and_clear(e);
    e.wifi = false;
    e.net_hold = true;
    PB_CHECK(e.share->create(create_options("read", QString::fromStdString(std::string("Zx9-secret-pw")), "", true)).value("ok").toBool());
    e.share->clearSecrets();   // the user left the screen (or the account changed)
    e.release_network(true);
    QTest::qWait(200);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(!e.share->busy());
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("closed"));
}

PB_TEST(a_link_is_requested_after_bringing_wifi_up_and_a_failure_is_reported) {
    Env e(true);
    e.cache("/Docs");
    const int64_t id = add_link(e);
    e.share->open("/Docs/Reports");
    e.wait();
    e.server.clear_log();
    e.wifi = false;
    e.net_ok = false;
    PB_CHECK(e.share->showLink(id).value("ok").toBool());
    QTest::qWait(50);
    PB_CHECK(!e.share->busy());
    PB_CHECK_EQ(e.share->message().value("key").toString().toStdString(), std::string("no_network"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.net_ok = true;
    PB_CHECK(e.share->showLink(id).value("ok").toBool());
    e.wait();
    PB_CHECK_EQ(e.ensure_calls, 2);
    PB_CHECK(!e.share->detail().value("linkText").toString().isEmpty());
}

PB_TEST_MAIN()
