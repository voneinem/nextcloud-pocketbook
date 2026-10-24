// Mailing a share link (specs/005-mail-link, contracts/bridge-qml.md): the link screen hands the link to the mail app with ONE call after the user's
// confirmation. The real call exists only on the reader, so the bridge gets a fake `hand_off` that records every argument vector it receives.
// Phase 3 (US3) holds what the secrets need, Phase 4 (US1) the send, Phase 5 (US2) the hand-off that fails.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/folder_service.h"
#include "pbc/log.h"
#include "pbc/store.h"
#include "privacy_probe.h"
#include "share_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_mail_link";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

using pbtest::FakeNextcloud;

struct FixedClock : pbc::Clock {
    int64_t t;
    explicit FixedClock(int64_t v) : t(v) {}
    int64_t now() const override { return t; }
};

const char* kLink = "https://cloud.example.org/s/TOKENforTheTest0123456789";
const char* kPassword = "Zx9-secret-pw-which-must-never-be-handed-over";

struct Env {
    pbtest::TmpDir dir;
    FakeNextcloud server;
    FixedClock clock{server.server_time()};
    std::unique_ptr<pbc::Store> store;
    Worker worker;
    bool wifi = true;
    int ensure_calls = 0;
    std::vector<std::vector<std::string>> calls;   // every argument vector the fake hand-off received
    int answer = 0;                                // what the fake hand-off returns
    std::function<void()> inside;                  // runs inside the hand-off (to prove re-entrancy is refused)
    std::unique_ptr<ShareBridge> share;

    explicit Env(bool with_hand_off = true) {
        auto r = pbc::Store::open(dir.file("data"), clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store");
        store = std::move(r.value);
        pbc::AccountRow a;
        a.base_url = server.base_url();
        a.login_name = "demo";
        a.user_id = "demo";
        a.state = "ok";
        PB_CHECK(store->save_account(a).ok());
        pbc::Credentials cr;
        cr.password = server.app_password();
        PB_CHECK(pbc::save_credentials(store->directory(), cr).ok());
        server.add_folder("/Docs");
        server.add_folder("/Docs/Reports");
        ShareBridge::Config c;
        c.store = store.get();
        c.worker = &worker;
        c.http = &server;
        c.clock = &clock;
        c.wifi_up = [this] { return wifi; };
        c.ensure_network = [this](std::function<void(bool)> done) {
            ++ensure_calls;
            wifi = true;
            done(true);
        };
        if (with_hand_off) {
            c.hand_off = [this](const std::vector<std::string>& argv) {
                calls.push_back(argv);
                if (inside) inside();
                return answer;
            };
        }
        share = std::make_unique<ShareBridge>(c);
    }
    ~Env() {
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    void wait() {
        QElapsedTimer t;
        t.start();
        while (share->busy() && t.elapsed() < 10000) QTest::qWait(5);
        QTest::qWait(20);
    }
    void open_reports() {
        pbc::FolderService fs(*store, server, clock);
        PB_CHECK(fs.load("/Docs", pbc::Fetch::Always).error.ok());
        share->open("/Docs/Reports");
        wait();
        server.clear_log();
    }
    // Share on /Docs/Reports with one link of the user (optionally with a password) and its screen open; returns the share id
    int64_t open_link(const std::string& password = std::string()) {
        pbc::FolderService fs(*store, server, clock);
        PB_CHECK(fs.load("/Docs", pbc::Fetch::Always).error.ok());
        FakeNextcloud::Share s;
        s.path = "/Docs/Reports";
        s.permissions = 1;
        s.password = password;
        const int64_t id = server.add_share(s);
        share->open("/Docs/Reports");
        wait();
        share->showLink(id);
        wait();
        server.clear_log();
        return id;
    }
    pbc::ShareDetail link_detail(pbc::LinkState state = pbc::LinkState::Ok, std::string link = kLink) {
        pbc::ShareDetail d;
        d.row.id = 7;
        d.row.kind = pbc::ShareKind::PublicLink;
        d.row.permission = pbc::LinkPermission::ReadOnly;
        d.row.permission_bits = 1;
        d.row.owned_by_me = true;
        d.link = std::move(link);
        d.link_state = state;
        return d;
    }
};

std::string join(const std::vector<std::string>& v) {
    std::string all;
    for (const std::string& s : v) all += s + "\n";
    return all;
}

}  // namespace

// ---- US3: the link and the password stay private ----

namespace {

QVariantMap create_options(const QString& password) {
    QVariantMap m;
    m.insert("permission", "read");
    m.insert("password", password);
    m.insert("expiry", "");
    m.insert("confirmed", true);
    return m;
}

}  // namespace

PB_TEST(the_password_of_a_link_is_never_handed_over_and_the_link_is_handed_over_only_in_the_call) {
    Env e;
    e.open_reports();
    e.share->create(create_options(QString::fromStdString(std::string(kPassword))));   // the link of the user, made with a password
    e.wait();
    e.wait();
    const std::string link = e.share->detail().value("linkText").toString().toStdString();
    PB_CHECK(link.rfind("https://", 0) == 0);
    pbtest::PrivacyProbe probe;
    PB_CHECK(e.share->mailLink(false).value("needsConfirm").toBool());
    PB_CHECK(e.share->mailLink(true).value("ok").toBool());
    PB_CHECK_EQ(e.calls.size(), size_t(1));
    const std::string handed = join(e.calls.at(0));
    PB_CHECK(handed.find(link) != std::string::npos);
    PB_CHECK(handed.find(kPassword) == std::string::npos);
    PB_CHECK(handed.find("Zx9") == std::string::npos);   // not even the beginning of it
    probe.expect_absent(kPassword, e.dir.file("data"), "the password after a hand-off");
}

PB_TEST(after_a_hand_off_accepted_or_not_the_link_is_in_no_log_and_no_file_and_the_item_name_is_in_no_log) {
    for (int answer : {0, 1}) {
        Env e;
        e.answer = answer;
        const int64_t id = e.open_link();
        const std::string token = e.server.find_share(id)->token;
        pbtest::PrivacyProbe probe;
        e.share->mailLink(true);
        PB_CHECK_EQ(e.calls.size(), size_t(1));
        const std::string handed = join(e.calls.at(0));
        probe.expect_only_in(token, handed, e.dir.file("data"), answer == 0 ? "the link after an accepted hand-off" : "the link after a refused hand-off");
        e.share->clearSecrets();
        probe.expect_absent(token, e.dir.file("data"), "the link after the screens were cleared");
        PB_CHECK(probe.log_text().find("Reports") == std::string::npos);   // the item name is in the listing cache (feature 001) but never in the log
        PB_CHECK(probe.log_text().find("Link to") == std::string::npos);   // nor the subject
    }
}

PB_TEST(a_hand_off_makes_no_request_and_does_not_ask_for_wifi_also_when_the_reader_is_offline) {
    Env e;
    e.open_link();
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.wifi = false;
    PB_CHECK(e.share->mailLink(true).value("ok").toBool());
    PB_CHECK_EQ(e.calls.size(), size_t(1));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.ensure_calls, 0);
    PB_CHECK_EQ(e.server.link_hits(), size_t(0));   // the link's address is never requested
}

// ---- US1: send a link by mail from the link screen ----

PB_TEST(without_confirmation_the_bridge_states_where_the_link_goes_and_calls_nothing) {
    Env e;
    e.open_link();
    const QVariantMap r = e.share->mailLink(false);
    PB_CHECK(r.value("needsConfirm").toBool());
    const QString text = r.value("text").toString();
    PB_CHECK(text.contains("PB Office"));
    PB_CHECK(text.contains("draft"));
    PB_CHECK(text.contains("Drafts"));
    PB_CHECK(!text.contains("password"));   // a link without a password: nothing to say about one
    PB_CHECK(!text.contains(kLink));
    PB_CHECK(e.calls.empty());
    Env pw;
    pw.open_link(kPassword);
    const QString with_password = pw.share->mailLink(false).value("text").toString();
    PB_CHECK(with_password.contains("password"));
    PB_CHECK(with_password.contains("not included"));
    PB_CHECK(!with_password.contains(kPassword));
    PB_CHECK(pw.calls.empty());
}

PB_TEST(a_confirmed_mail_link_calls_the_hand_off_exactly_once_with_the_link_the_length_and_the_subject) {
    Env e;
    const int64_t id = e.open_link();
    const std::string link = e.share->detail().value("linkText").toString().toStdString();
    PB_CHECK(link.rfind("https://", 0) == 0 && link.find(e.server.find_share(id)->token) != std::string::npos);
    QSignalSpy handed(e.share.get(), &ShareBridge::mailHandedOver);
    const QVariantMap r = e.share->mailLink(true);
    PB_CHECK(r.value("ok").toBool());
    PB_CHECK_EQ(e.calls.size(), size_t(1));
    const std::vector<std::string>& a = e.calls.at(0);
    PB_CHECK_EQ(a.size(), size_t(4));
    PB_CHECK_EQ(a[0], std::string("pbo-mail-v1"));
    PB_CHECK_EQ(a[1], "text=" + link);
    PB_CHECK_EQ(a[2], "textlen=" + std::to_string(link.size()));
    PB_CHECK_EQ(a[3], std::string("subject=Link to Reports"));
    for (const std::string& arg : a) PB_CHECK(arg.rfind("to=", 0) != 0);   // no recipient
    PB_CHECK_EQ(handed.count(), 1);
    QTest::qWait(50);
    QCoreApplication::processEvents();
    PB_CHECK_EQ(e.calls.size(), size_t(1));   // nothing repeats by itself
}

PB_TEST(after_an_accepted_hand_off_the_link_is_gone_and_a_second_confirmation_calls_nothing) {
    Env e;
    e.open_link();
    PB_CHECK(e.share->mailLink(true).value("ok").toBool());
    PB_CHECK(e.share->detail().value("linkText").toString().isEmpty());
    PB_CHECK(e.share->detail().isEmpty());
    const QVariantMap again = e.share->mailLink(true);
    PB_CHECK_EQ(again.value("errorKey").toString().toStdString(), std::string("share_item_gone"));
    PB_CHECK_EQ(e.calls.size(), size_t(1));
}

PB_TEST(cancel_and_leaving_the_screen_make_no_call) {
    Env e;
    e.open_link();
    e.share->mailLink(false);   // the confirmation was shown; the user cancels: the page does not call again
    PB_CHECK(e.calls.empty());
    e.share->leaveLink();
    PB_CHECK_EQ(e.share->mailLink(true).value("errorKey").toString().toStdString(), std::string("share_item_gone"));
    e.share->clearSecrets();
    PB_CHECK_EQ(e.share->mailLink(true).value("errorKey").toString().toStdString(), std::string("share_item_gone"));
    PB_CHECK(e.calls.empty());
}

PB_TEST(a_hand_off_started_from_inside_the_hand_off_is_refused_as_busy) {
    Env e;
    e.open_link();
    QVariantMap inner;
    e.inside = [&] { inner = e.share->mailLink(true); };
    e.share->mailLink(true);
    PB_CHECK_EQ(inner.value("errorKey").toString().toStdString(), std::string("busy"));
    PB_CHECK_EQ(e.calls.size(), size_t(1));
}

PB_TEST(an_unreadable_or_foreign_link_is_not_mailed_and_a_link_that_is_not_secure_is) {
    for (pbc::LinkState state : {pbc::LinkState::Unreadable, pbc::LinkState::DifferentAddress}) {
        Env e;
        e.share->setDetail(e.link_detail(state));
        const QVariantMap r = e.share->mailLink(true);
        PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("mail_link_unavailable"));
        PB_CHECK(e.calls.empty());
        PB_CHECK(!e.share->mailLink(false).contains("needsConfirm"));
    }
    Env e;
    e.share->setDetail(e.link_detail(pbc::LinkState::NotSecure));
    PB_CHECK(e.share->mailLink(true).value("ok").toBool());
    PB_CHECK_EQ(e.calls.size(), size_t(1));
}

PB_TEST(with_no_link_open_nothing_is_called) {
    Env e;
    PB_CHECK_EQ(e.share->mailLink(true).value("errorKey").toString().toStdString(), std::string("share_item_gone"));
    PB_CHECK_EQ(e.share->mailLink(false).value("errorKey").toString().toStdString(), std::string("share_item_gone"));
    PB_CHECK(e.calls.empty());
}

PB_TEST(while_a_sharing_request_runs_nothing_is_called) {
    Env e;
    const int64_t id = e.open_link();
    PB_CHECK(e.share->showLink(id).value("ok").toBool());   // the link is asked for again: a request runs
    PB_CHECK(e.share->busy());
    PB_CHECK_EQ(e.share->mailLink(true).value("errorKey").toString().toStdString(), std::string("busy"));
    PB_CHECK(e.calls.empty());
    e.wait();
}

PB_TEST(a_long_item_name_goes_into_the_subject_shortened_in_one_line) {
    Env e;
    const std::string name(200, 'x');   // the cut at 300 characters and the control characters are proved in test_mail_handoff
    e.server.add_folder("/Docs/" + name);
    pbc::FolderService fs(*e.store, e.server, e.clock);
    PB_CHECK(fs.load("/Docs", pbc::Fetch::Always).error.ok());
    FakeNextcloud::Share s;
    s.path = "/Docs/" + name;
    s.permissions = 1;
    const int64_t id = e.server.add_share(s);
    e.share->open(QString::fromStdString(s.path));
    e.wait();
    e.share->showLink(id);
    e.wait();
    PB_CHECK(e.share->mailLink(true).value("ok").toBool());
    PB_CHECK_EQ(e.calls.size(), size_t(1));
    const std::string subject = e.calls.at(0).at(3);   // the item name as the screen shows it: display_safe() shortens it to 120 characters
    PB_CHECK(subject.rfind("subject=Link to xxxxxxxx", 0) == 0);
    PB_CHECK(subject.size() < std::string("subject=").size() + 300 + 8);   // 300 characters at most (an ellipsis is 3 bytes)
    PB_CHECK(subject.find('\n') == std::string::npos);
}

// ---- US2: a hand-off that fails is explained, not repeated ----

PB_TEST(a_refused_hand_off_sets_the_notice_makes_no_second_call_and_keeps_the_link_screen_while_the_app_is_active) {
    for (int answer : {1, 127}) {
        Env e;
        e.answer = answer;
        e.open_link();
        QSignalSpy handed(e.share.get(), &ShareBridge::mailHandedOver);
        const QVariantMap r = e.share->mailLink(true);
        PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("mail_not_accepted"));
        PB_CHECK_EQ(e.share->mailNotice().toStdString(), std::string("mail_not_accepted"));
        PB_CHECK_EQ(handed.count(), 0);
        PB_CHECK_EQ(e.calls.size(), size_t(1));
        QTest::qWait(60);
        QCoreApplication::processEvents();
        PB_CHECK_EQ(e.calls.size(), size_t(1));   // no retry by itself
        PB_CHECK(!e.share->detail().value("linkText").toString().isEmpty());   // the app is active: the user may try again deliberately
        e.share->mailLink(true);   // a deliberate second confirmation by the user is a second call; only this one
        PB_CHECK_EQ(e.calls.size(), size_t(2));
    }
}

PB_TEST(without_a_hand_off_function_the_answer_is_the_same_as_a_refusal) {
    Env e(false);
    e.open_link();
    const QVariantMap r = e.share->mailLink(true);
    PB_CHECK_EQ(r.value("errorKey").toString().toStdString(), std::string("mail_not_accepted"));
    PB_CHECK_EQ(e.share->mailNotice().toStdString(), std::string("mail_not_accepted"));
}

PB_TEST(the_notice_survives_the_clearing_of_the_screens_and_is_dismissed_on_request) {
    Env e;
    e.answer = 1;
    e.open_link();
    e.share->mailLink(true);
    QSignalSpy changed(e.share.get(), &ShareBridge::mailNoticeChanged);
    e.share->onApplicationStateChanged(Qt::ApplicationInactive);   // PB Office came to the front
    PB_CHECK_EQ(e.share->state().toStdString(), std::string("closed"));
    PB_CHECK(e.share->detail().isEmpty());                          // the link is not shown again
    PB_CHECK_EQ(e.share->mailNotice().toStdString(), std::string("mail_not_accepted"));
    e.share->dismissMailNotice();
    PB_CHECK(e.share->mailNotice().isEmpty());
    PB_CHECK_EQ(changed.count(), 1);
    e.share->dismissMailNotice();   // dismissing nothing says nothing
    PB_CHECK_EQ(changed.count(), 1);
}

PB_TEST(the_notice_and_the_messages_hold_no_link_no_subject_and_no_item_name) {
    Env e;
    e.answer = 1;
    const int64_t id = e.open_link();
    const std::string token = e.server.find_share(id)->token;
    const QVariantMap r = e.share->mailLink(true);
    QString all = e.share->mailNotice();
    for (auto it = r.constBegin(); it != r.constEnd(); ++it) all += it.key() + "=" + it.value().toString() + ";";
    for (auto it = e.share->message().constBegin(); it != e.share->message().constEnd(); ++it) all += it.key() + "=" + it.value().toString() + ";";
    PB_CHECK(!all.contains(QString::fromStdString(token)));
    PB_CHECK(!all.contains("Reports"));
    PB_CHECK(!all.contains("Link to"));
}

PB_TEST(a_link_over_the_size_limit_is_not_handed_over) {
    Env e;
    e.share->setDetail(e.link_detail(pbc::LinkState::Ok, "https://cloud.example.org/s/" + std::string(9000, 'a')));
    PB_CHECK_EQ(e.share->mailLink(false).value("errorKey").toString().toStdString(), std::string("mail_too_long"));
    PB_CHECK_EQ(e.share->mailLink(true).value("errorKey").toString().toStdString(), std::string("mail_too_long"));
    PB_CHECK(e.calls.empty());
    PB_CHECK(e.share->mailNotice().isEmpty());   // a link too long is not a refusal by the mail app
}

PB_TEST_MAIN()
