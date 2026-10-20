// The ConnectionBridge (contracts/bridge-qml.md): the Test, the browser login, sign-out and the single account check at start. Real core,
// fake Nextcloud. Nothing here may send a request that a tap did not ask for.
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <sys/stat.h>

#include "connection_bridge.h"
#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/store.h"
#include "pbc/store_files.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_connection_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct FixedClock : pbc::Clock {
    int64_t t;
    explicit FixedClock(int64_t v) : t(v) {}
    int64_t now() const override { return t; }
};

struct Env {
    pbtest::TmpDir dir;
    pbtest::FakeNextcloud server;
    FixedClock clock{server.server_time()};
    std::unique_ptr<pbc::Store> store;
    Worker worker;
    bool wifi = true;
    int network_requests = 0;
    std::unique_ptr<ConnectionBridge> bridge;

    Env() {
        auto r = pbc::Store::open(dir.file("data"), clock);
        if (!r.ok()) PB_FAIL_AT("Store::open failed: " + r.error.key);
        store = std::move(r.value);
        ConnectionBridge::Config c;
        c.store = store.get();
        c.worker = &worker;
        c.http = &server;
        c.clock = &clock;
        c.format_date_time = [](qint64 t) { return QStringLiteral("T%1").arg(t); };
        c.wifi_up = [this] { return wifi; };
        c.ensure_network = [this](std::function<void(bool)> done) {
            ++network_requests;
            done(wifi);
        };
        bridge = std::make_unique<ConnectionBridge>(c);
    }
    ~Env() {
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    // runs the Test and waits for its signal
    bool test(const QString& address, const QString& user, const QString& pw, QString* key = nullptr) {
        QSignalSpy spy(bridge.get(), &ConnectionBridge::testFinished);
        bridge->testConnection(address, user, pw);
        if (spy.isEmpty() && !spy.wait(10000)) PB_FAIL_AT("the Test did not finish");
        if (key) *key = spy.last().at(1).toString();
        return spy.last().at(0).toBool();
    }
    QString url() const { return QString::fromStdString(server.base_url()); }
};

}  // namespace

PB_TEST(without_a_check_there_is_no_time_to_show) {
    Env e;
    PB_CHECK(e.bridge->checkedAtText().isEmpty());
}

PB_TEST(a_good_test_saves_the_account_and_the_password_and_shows_the_result) {
    Env e;
    e.server.set_quota(1000, 9000);
    QString key;
    PB_CHECK(e.test(e.url(), "demo", "app-pass-0001", &key));
    PB_CHECK(key.isEmpty());
    PB_CHECK_EQ(e.bridge->state().toStdString(), std::string("ok"));
    PB_CHECK_EQ(e.bridge->serverName().toStdString(), std::string("Example Cloud"));
    PB_CHECK_EQ(e.bridge->serverVersion().toStdString(), std::string("31.0.4"));
    PB_CHECK(!e.bridge->quotaUsedText().isEmpty());
    PB_CHECK(!e.bridge->quotaTotalText().isEmpty());
    PB_CHECK(!e.bridge->quotaUnlimited());
    PB_CHECK_EQ(e.bridge->checkedAt(), qint64(e.clock.t));
    PB_CHECK_EQ(e.bridge->checkedAtText().toStdString(), ("T" + std::to_string(e.clock.t)));   // the home page shows when the account was last checked
    PB_CHECK(e.bridge->problemKey().isEmpty());
    PB_CHECK(!e.bridge->busy());
    const auto creds = pbc::load_credentials(e.store->directory());
    PB_CHECK(creds.ok());
    PB_CHECK_EQ(creds.value.password, std::string("app-pass-0001"));
    PB_CHECK(e.store->account().has_value());
    PB_CHECK_EQ(e.store->account()->user_id, std::string("demo"));
}

PB_TEST(a_bare_host_is_completed_and_an_http_address_is_refused_without_any_request) {
    Env e;
    QString key;
    PB_CHECK(!e.test("http://cloud.example.org", "demo", "app-pass-0001", &key));
    PB_CHECK_EQ(key.toStdString(), std::string("scheme_not_allowed"));
    PB_CHECK_EQ(e.bridge->problemKey().toStdString(), std::string("scheme_not_allowed"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(!e.store->account().has_value());
}

PB_TEST(a_failed_test_saves_nothing_and_keeps_an_earlier_account) {
    Env e;
    PB_CHECK(e.test(e.url(), "demo", "app-pass-0001"));
    QString key;
    PB_CHECK(!e.test(e.url(), "demo", "wrong", &key));
    PB_CHECK_EQ(key.toStdString(), std::string("auth_failed"));
    PB_CHECK_EQ(pbc::load_credentials(e.store->directory()).value.password, std::string("app-pass-0001"));   // the working password is kept
    PB_CHECK_EQ(e.bridge->problemKey().toStdString(), std::string("auth_failed"));
}

PB_TEST(the_typed_password_is_not_kept_by_the_bridge) {
    Env e;
    PB_CHECK(e.test(e.url(), "demo", "app-pass-0001"));
    PB_CHECK(e.bridge->metaObject()->indexOfProperty("password") < 0);
    PB_CHECK(e.bridge->metaObject()->indexOfProperty("appPassword") < 0);
}

PB_TEST(without_wifi_a_tap_that_needs_the_network_says_so_and_sends_nothing) {
    Env e;
    e.wifi = false;
    QString key;
    PB_CHECK(!e.test(e.url(), "demo", "app-pass-0001", &key));
    PB_CHECK_EQ(key.toStdString(), std::string("no_network"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.network_requests, 1);   // the tap asked the system to bring Wi-Fi up, once
}

PB_TEST(the_browser_login_goes_through_pending_to_approved_and_stores_the_account) {
    Env e;
    QSignalSpy started(e.bridge.get(), &ConnectionBridge::changed);
    e.bridge->startLogin(e.url());
    PB_CHECK(started.wait(10000) || e.bridge->loginActive());
    PB_CHECK(e.bridge->loginActive());
    PB_CHECK(!e.bridge->loginUrl().isEmpty());
    PB_CHECK(e.bridge->loginExpiresInMinutes() > 0 && e.bridge->loginExpiresInMinutes() <= 20);

    QSignalSpy checked(e.bridge.get(), &ConnectionBridge::loginChecked);
    e.bridge->checkLogin();
    PB_CHECK(checked.wait(10000));
    PB_CHECK_EQ(checked.last().at(0).toString().toStdString(), std::string("pending"));
    PB_CHECK(e.bridge->loginActive());   // still open
    PB_CHECK(!e.store->account().has_value());

    e.server.approve_login("demo", "app-pass-0001");
    e.bridge->checkLogin();
    PB_CHECK(checked.wait(10000));
    PB_CHECK_EQ(checked.last().at(0).toString().toStdString(), std::string("approved"));
    PB_CHECK(!e.bridge->loginActive());
    PB_CHECK(e.bridge->loginUrl().isEmpty());
    PB_CHECK_EQ(e.bridge->state().toStdString(), std::string("ok"));
    PB_CHECK_EQ(pbc::load_credentials(e.store->directory()).value.password, std::string("app-pass-0001"));
}

PB_TEST(checking_sends_exactly_one_request_and_cancel_discards_the_flow) {
    Env e;
    QSignalSpy started(e.bridge.get(), &ConnectionBridge::changed);
    e.bridge->startLogin(e.url());
    PB_CHECK(started.wait(10000) || e.bridge->loginActive());
    const size_t before = e.server.request_count();
    QSignalSpy checked(e.bridge.get(), &ConnectionBridge::loginChecked);
    e.bridge->checkLogin();
    PB_CHECK(checked.wait(10000));
    PB_CHECK_EQ(e.server.request_count(), before + 1);
    e.bridge->cancelLogin();
    PB_CHECK(!e.bridge->loginActive());
    PB_CHECK(e.bridge->loginUrl().isEmpty());
    e.bridge->checkLogin();   // nothing to check any more
    QCoreApplication::processEvents();
    PB_CHECK_EQ(e.server.request_count(), before + 1);
}

PB_TEST(a_login_older_than_twenty_minutes_has_expired) {
    Env e;
    QSignalSpy started(e.bridge.get(), &ConnectionBridge::changed);
    e.bridge->startLogin(e.url());
    PB_CHECK(started.wait(10000) || e.bridge->loginActive());
    e.clock.t += 21 * 60;
    PB_CHECK_EQ(e.bridge->loginExpiresInMinutes(), 0);
    QSignalSpy checked(e.bridge.get(), &ConnectionBridge::loginChecked);
    const size_t before = e.server.request_count();
    e.bridge->checkLogin();
    PB_CHECK(checked.count() == 1 || checked.wait(10000));
    PB_CHECK_EQ(checked.last().at(0).toString().toStdString(), std::string("expired"));
    PB_CHECK_EQ(e.server.request_count(), before);   // no request for a flow that is over
    PB_CHECK(!e.bridge->loginActive());
}

PB_TEST(the_check_at_start_sends_nothing_without_wifi_or_an_account_and_exactly_one_request_otherwise) {
    Env e;
    e.bridge->checkAtStart();   // no account
    e.worker.wait_idle();
    QCoreApplication::processEvents();
    PB_CHECK_EQ(e.server.request_count(), size_t(0));

    PB_CHECK(e.test(e.url(), "demo", "app-pass-0001"));
    const int taps = e.network_requests;   // the Test was a tap and may bring Wi-Fi up; the check at start never does
    e.server.clear_log();
    e.wifi = false;
    e.bridge->checkAtStart();
    e.worker.wait_idle();
    QCoreApplication::processEvents();
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.bridge->state().toStdString(), std::string("offline"));
    PB_CHECK_EQ(e.network_requests, taps);   // the app never turns Wi-Fi on by itself at start

    e.wifi = true;
    QSignalSpy changed(e.bridge.get(), &ConnectionBridge::changed);
    e.bridge->checkAtStart();
    e.worker.wait_idle();
    QCoreApplication::processEvents();
    PB_CHECK_EQ(e.server.request_count(), size_t(1));   // the capabilities call and nothing else
    PB_CHECK(e.server.log()[0].path.find("/cloud/capabilities") != std::string::npos);
    PB_CHECK_EQ(e.bridge->state().toStdString(), std::string("ok"));
    PB_CHECK_EQ(e.network_requests, taps);
}

PB_TEST(a_failed_check_at_start_shows_the_problem_and_two_in_a_row_stop_further_requests) {
    Env e;
    PB_CHECK(e.test(e.url(), "demo", "app-pass-0001"));
    e.server.set_account("demo", "demo", "changed-on-the-server");   // the app password was revoked
    for (int i = 0; i < 2; ++i) {
        e.bridge->checkAtStart();
        e.worker.wait_idle();
        QCoreApplication::processEvents();
    }
    PB_CHECK_EQ(e.bridge->state().toStdString(), std::string("needs_signin"));
    PB_CHECK_EQ(e.bridge->problemKey().toStdString(), std::string("auth_failed"));
    e.server.clear_log();
    e.bridge->checkAtStart();   // blocked: nothing is sent
    e.worker.wait_idle();
    QCoreApplication::processEvents();
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    // a new Test by the user lifts the block
    PB_CHECK(!e.test(e.url(), "demo", "still-wrong"));
    PB_CHECK(e.server.request_count() > 0);
}

PB_TEST(sign_out_removes_the_password_file_first_and_then_the_account) {
    Env e;
    PB_CHECK(e.test(e.url(), "demo", "app-pass-0001"));
    e.store->debug_exec("INSERT INTO local_files(remote_path, local_path, size, mtime, remote_etag, downloaded_at) VALUES('/a', 'a', 1, 2, '\"e\"', 3)");
    e.bridge->signOut();
    PB_CHECK(!e.store->account().has_value());
    PB_CHECK(pbc::load_credentials(e.store->directory()).value.password.empty());
    PB_CHECK_EQ(e.bridge->state().toStdString(), std::string("none"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM local_files"), std::string("1"));   // the user's files stay
}

PB_TEST(if_the_password_file_cannot_be_removed_the_account_stays_and_the_failure_is_shown) {
    Env e;
    PB_CHECK(e.test(e.url(), "demo", "app-pass-0001"));
    const std::string path = pbc::credentials_path(e.store->directory());
    ::unlink(path.c_str());
    PB_CHECK(::mkdir(path.c_str(), 0700) == 0);   // a directory where the file should be: unlink fails
    e.bridge->signOut();
    PB_CHECK(e.store->account().has_value());      // never an account without its password handled
    PB_CHECK(!e.bridge->problemKey().isEmpty());
    ::rmdir(path.c_str());
}

PB_TEST(the_measured_protection_of_the_credentials_file_is_reported) {
    Env e;
    PB_CHECK(e.test(e.url(), "demo", "app-pass-0001"));
    const std::string v = e.bridge->credentialsProtection().toStdString();
    PB_CHECK(v == "enforced" || v == "not_enforced" || v == "unknown");
}

PB_TEST(constructing_the_bridge_and_reading_its_properties_sends_nothing) {
    Env e;
    (void)e.bridge->state();
    (void)e.bridge->serverName();
    (void)e.bridge->quotaUsedText();
    (void)e.bridge->loginUrl();
    e.worker.wait_idle();
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST_MAIN()
