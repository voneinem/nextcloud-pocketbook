// The TransferBridge (contracts/bridge-qml.md): details of a file, the checks before a download, one download at a time with coarse
// progress, cancel, resume after a restart, the conflict choices, and the one-tap open in the reader's viewer. Real core, fake Nextcloud.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

#include "download_env.h"
#include "fake_nextcloud_server.h"
#include "pbc/folder_service.h"
#include "transfer_bridge.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_transfer_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

std::string pattern(size_t n, int seed = 7) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + static_cast<size_t>(seed)) & 0xff);
    return s;
}

struct Env {
    pbtest::TmpDir dir;
    pbtest::FakeNextcloud server;
    pbtest::DlClock clock;
    pbtest::FakeStorage storage;
    std::unique_ptr<pbc::Store> store;
    std::string root;
    Worker worker;
    bool wifi = true;
    QStringList opened;
    std::unique_ptr<TransferBridge> bridge;

    Env() {
        server.add_folder("/Books");
        server.add_file("/Books/story.epub", pattern(300 * 1024), 1790000000);
        server.add_file("/Books/small.txt", "hello", 1790000100);
        server.add_folder("/Vault");
        server.set_encrypted("/Vault", true);
        clock.t = server.server_time();
        auto r = pbc::Store::open(dir.file("data"), clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
        store = std::move(r.value);
        pbc::AccountRow a;
        a.base_url = server.base_url();
        a.login_name = "demo";
        a.user_id = "demo";
        a.state = "ok";
        PB_CHECK(store->save_account(a).ok());
        pbc::Credentials c;
        c.password = server.app_password();
        PB_CHECK(pbc::save_credentials(store->directory(), c).ok());
        root = dir.file("PBCloud");
        ::mkdir(root.c_str(), 0700);
        make_bridge();
        list("/");
        list("/Books");
        server.clear_log();
    }
    void make_bridge() {
        bridge.reset();
        TransferBridge::Config cfg;
        cfg.store = store.get();
        cfg.worker = &worker;
        cfg.http = &server;
        cfg.clock = &clock;
        cfg.storage = &storage;
        cfg.download_root = root;
        cfg.ensure_network = [this](std::function<void(bool)> done) { done(wifi); };
        cfg.wifi_up = [this] { return wifi; };
        cfg.format_date_time = [](qint64 t) { return QStringLiteral("T%1").arg(t); };
        cfg.open_file = [this](const QString& path) {
            opened << path;
            return true;
        };
        cfg.options.sleep_ms = [](int) {};
        cfg.options.delays_s = {1, 1};
        bridge = std::make_unique<TransferBridge>(cfg);
    }
    void list(const std::string& path) {
        pbc::FolderService svc(*store, server, clock);
        PB_CHECK(svc.load(path, pbc::Fetch::Always).error.ok());
    }
    ~Env() {
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    void settle() {
        QElapsedTimer t;
        t.start();
        while (bridge->jobBusy() && t.elapsed() < 20000) QTest::qWait(5);
        if (bridge->jobBusy()) PB_FAIL_AT("the job did not end");
        QCoreApplication::processEvents();
    }
    std::string state() { return bridge->jobState().toStdString(); }
};

}  // namespace

PB_TEST(the_details_tell_what_the_file_is_and_whether_a_copy_is_on_the_reader) {
    Env e;
    QVariantMap d = e.bridge->details("/Books/story.epub");
    PB_CHECK_EQ(d.value("name").toString().toStdString(), std::string("story.epub"));
    PB_CHECK_EQ(d.value("kind").toString().toStdString(), std::string("file"));
    PB_CHECK_EQ(d.value("sizeText").toString().toStdString(), std::string("300.0 KB"));
    PB_CHECK_EQ(d.value("dateText").toString().toStdString(), std::string("T1790000000"));
    PB_CHECK(!d.value("onReader").toBool());
    PB_CHECK(!d.value("serverNewer").toBool());
    PB_CHECK(d.value("available").toBool());
    PB_CHECK(!d.value("canOpen").toBool());

    e.bridge->download("/Books/story.epub", true);
    e.settle();
    d = e.bridge->details("/Books/story.epub");
    PB_CHECK(d.value("onReader").toBool());
    PB_CHECK(d.value("canOpen").toBool());
    PB_CHECK(!d.value("serverNewer").toBool());

    e.server.set_file("/Books/story.epub", pattern(310 * 1024, 3), 1790009000);
    e.list("/Books");   // the listing now shows the newer version
    d = e.bridge->details("/Books/story.epub");
    PB_CHECK(d.value("onReader").toBool());
    PB_CHECK(d.value("serverNewer").toBool());

    PB_CHECK(!e.bridge->details("/Vault").value("available").toBool());
    PB_CHECK(e.bridge->details("/nowhere").isEmpty() || !e.bridge->details("/nowhere").value("available").toBool());
}

PB_TEST(the_checks_before_a_download_are_offered_to_the_page) {
    Env e;
    QVariantMap p = e.bridge->preflight("/Books/story.epub");
    PB_CHECK_EQ(p.value("verdict").toString().toStdString(), std::string("ok"));
    e.storage.free = 1ll << 20;
    p = e.bridge->preflight("/Books/story.epub");
    PB_CHECK_EQ(p.value("verdict").toString().toStdString(), std::string("refused"));
    PB_CHECK_EQ(p.value("errorKey").toString().toStdString(), std::string("not_enough_space"));
    PB_CHECK(!p.value("freeText").toString().isEmpty());
    PB_CHECK(!p.value("neededText").toString().isEmpty());
    e.storage.free = 1ll << 40;
    PB_CHECK(e.store->set_setting("large_file_warn_mib", "1").ok());   // the warning size is a setting
    e.server.add_file("/Books/two-mib.bin", pattern(2 * 1024 * 1024));
    e.list("/Books");
    PB_CHECK_EQ(e.bridge->preflight("/Books/two-mib.bin").value("verdict").toString().toStdString(), std::string("confirm"));
}

PB_TEST(a_download_runs_to_the_end_shows_coarse_progress_and_refuses_a_second_job_meanwhile) {
    Env e;
    e.server.add_file("/Books/big.bin", pattern(3 * 1024 * 1024));
    e.list("/Books");
    QSignalSpy changed(e.bridge.get(), &TransferBridge::changed);
    e.bridge->download("/Books/big.bin", true);
    PB_CHECK(e.bridge->jobBusy());
    e.bridge->download("/Books/small.txt", true);   // a second tap while one runs is ignored
    e.settle();
    PB_CHECK_EQ(e.state(), std::string("done"));
    PB_CHECK(e.bridge->jobError().isEmpty());
    PB_CHECK_EQ(e.bridge->jobPercent(), 100);
    PB_CHECK(::access((e.root + "/Books/big.bin").c_str(), R_OK) == 0);
    PB_CHECK(::access((e.root + "/Books/small.txt").c_str(), F_OK) != 0);
    PB_CHECK(changed.count() < 40);   // coarse: nothing like one update per 64 KiB (48 of them)
}

PB_TEST(a_cancel_ends_the_job_and_leaves_no_file) {
    Env e;
    e.server.add_file("/Books/big.bin", pattern(3 * 1024 * 1024));
    e.list("/Books");
    e.bridge->download("/Books/big.bin", true);
    e.bridge->cancel();
    e.settle();
    PB_CHECK_EQ(e.state(), std::string("cancelled"));
    PB_CHECK(::access((e.root + "/Books/big.bin").c_str(), F_OK) != 0);
    PB_CHECK(e.store->journal().empty());
    PB_CHECK(e.bridge->pausedDownloads().isEmpty());
}

PB_TEST(without_wifi_or_without_room_nothing_is_sent_and_the_reason_is_shown) {
    Env e;
    e.wifi = false;
    e.bridge->download("/Books/story.epub", true);
    e.settle();
    PB_CHECK_EQ(e.state(), std::string("failed"));
    PB_CHECK_EQ(e.bridge->jobError().toStdString(), std::string("no_network"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.wifi = true;
    e.storage.free = 1ll << 20;
    e.bridge->download("/Books/story.epub", true);
    e.settle();
    PB_CHECK_EQ(e.bridge->jobError().toStdString(), std::string("not_enough_space"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_large_file_waits_for_the_users_confirmation) {
    Env e;
    PB_CHECK(e.store->set_setting("large_file_warn_mib", "1").ok());
    e.server.add_file("/Books/two-mib.bin", pattern(2 * 1024 * 1024));
    e.list("/Books");
    e.server.clear_log();
    e.bridge->download("/Books/two-mib.bin", false);   // not confirmed: nothing starts
    PB_CHECK(!e.bridge->jobBusy());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.bridge->download("/Books/two-mib.bin", true);
    e.settle();
    PB_CHECK_EQ(e.state(), std::string("done"));
}

PB_TEST(a_paused_download_is_offered_after_a_restart_and_resumes) {
    Env e;
    e.server.transport().drop_next_download_after(1000);
    e.server.set_interceptor([](const pbc::Request& r) -> std::optional<pbc::Response> {
        if (r.method == "GET" && !pbc::header_value(r.headers, "Range").empty()) {
            pbc::Response down;
            down.error = pbc::ErrorClass::Network;
            down.error_key = "server_unreachable";
            return down;
        }
        return std::nullopt;
    });
    e.bridge->download("/Books/story.epub", true);
    e.settle();
    PB_CHECK_EQ(e.state(), std::string("paused"));
    e.make_bridge();   // the app was closed and opened again
    const QVariantList paused = e.bridge->pausedDownloads();
    PB_CHECK_EQ(paused.size(), 1);
    PB_CHECK_EQ(paused.at(0).toMap().value("remotePath").toString().toStdString(), std::string("/Books/story.epub"));
    e.server.set_interceptor(nullptr);
    e.bridge->resume(paused.at(0).toMap().value("id").toLongLong());
    e.settle();
    PB_CHECK_EQ(e.state(), std::string("done"));
    PB_CHECK(e.bridge->pausedDownloads().isEmpty());
}

PB_TEST(a_changed_copy_stops_the_download_and_the_choices_do_what_they_say) {
    Env e;
    e.bridge->download("/Books/small.txt", true);
    e.settle();
    {
        std::ofstream f(e.root + "/Books/small.txt", std::ios::trunc);
        f << "edited on the reader";
    }
    e.server.set_file("/Books/small.txt", "hello again, longer", 1790009000);
    e.list("/Books");
    e.server.clear_log();
    e.bridge->download("/Books/small.txt", true);
    e.settle();
    PB_CHECK_EQ(e.state(), std::string("blocked"));
    PB_CHECK_EQ(e.bridge->jobError().toStdString(), std::string("local_changed"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));

    e.bridge->resolveConflict("/Books/small.txt", "keep_mine");
    e.settle();
    PB_CHECK_EQ(e.bridge->details("/Books/small.txt").value("serverNewer").toBool(), false);   // the shown version is remembered
    e.bridge->resolveConflict("/Books/small.txt", "keep_both");
    e.settle();
    PB_CHECK_EQ(e.state(), std::string("done"));
    PB_CHECK(::access((e.root + "/Books/small (server copy).txt").c_str(), R_OK) == 0);
    e.bridge->resolveConflict("/Books/small.txt", "replace");
    e.settle();
    PB_CHECK_EQ(e.state(), std::string("done"));
    std::ifstream in(e.root + "/Books/small.txt");
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    PB_CHECK_EQ(got, std::string("hello again, longer"));
    e.bridge->resolveConflict("/Books/small.txt", "something_else");   // an unknown choice does nothing
    PB_CHECK(!e.bridge->jobBusy());
}

PB_TEST(open_hands_only_a_regular_file_inside_the_download_folder_to_the_reader) {
    Env e;
    e.bridge->download("/Books/story.epub", true);
    e.settle();
    e.bridge->open("/Books/story.epub");
    PB_CHECK_EQ(e.opened.size(), 1);
    PB_CHECK_EQ(e.opened.at(0).toStdString(), e.root + "/Books/story.epub");

    e.opened.clear();
    e.bridge->open("/Books/never-downloaded.epub");   // an unknown remote path
    PB_CHECK(e.opened.isEmpty());

    // rows that point outside the folder, at a directory, or at a link are not opened
    const std::string outside = e.dir.file("outside.txt");
    {
        std::ofstream f(outside);
        f << "secret";
    }
    pbc::LocalFile bad;
    bad.size = 1;
    bad.mtime = 1;
    bad.remote_etag = "\"e\"";
    bad.downloaded_at = 1;
    bad.remote_path = "/h1";
    bad.local_path = "../outside.txt";
    PB_CHECK(e.store->commit_download(bad, 0).ok());
    bad.remote_path = "/h2";
    bad.local_path = "Books";   // a directory
    PB_CHECK(e.store->commit_download(bad, 0).ok());
    PB_CHECK(::symlink(outside.c_str(), (e.root + "/link.txt").c_str()) == 0);
    bad.remote_path = "/h3";
    bad.local_path = "link.txt";
    PB_CHECK(e.store->commit_download(bad, 0).ok());
    bad.remote_path = "/h4";
    bad.local_path = "/etc/passwd";
    PB_CHECK(e.store->commit_download(bad, 0).ok());
    for (const char* p : {"/h1", "/h2", "/h3", "/h4"}) e.bridge->open(p);
    PB_CHECK(e.opened.isEmpty());
}

PB_TEST(marks_are_set_and_removed_and_nothing_is_fetched_by_marking) {
    Env e;
    e.bridge->setKept("/Books", "folder", true);
    PB_CHECK(e.store->is_marked("/Books"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.bridge->setKept("/Books/small.txt", "file", true);
    PB_CHECK(e.store->in_scope("/Books/small.txt"));
    e.bridge->setKept("/Books", "folder", false);
    PB_CHECK(!e.store->is_marked("/Books"));
    PB_CHECK(e.store->is_marked("/Books/small.txt"));
    e.bridge->setKept("/x", "banana", true);   // an unknown kind changes nothing
    PB_CHECK(!e.store->is_marked("/x"));
}

PB_TEST(an_update_runs_only_when_asked_and_reports_what_it_did) {
    Env e;
    e.bridge->setKept("/Books", "folder", true);
    QCoreApplication::processEvents();
    PB_CHECK_EQ(e.server.request_count(), size_t(0));   // marking and waiting do nothing
    PB_CHECK(!e.bridge->updateRunning());
    QSignalSpy finished(e.bridge.get(), &TransferBridge::updateFinished);
    e.bridge->update();
    PB_CHECK(e.bridge->updateRunning());
    PB_CHECK(finished.wait(20000));
    PB_CHECK(!e.bridge->updateRunning());
    const QVariantMap r = e.bridge->updateReport();
    PB_CHECK_EQ(r.value("added").toInt(), 2);          // story.epub and small.txt
    PB_CHECK_EQ(r.value("failed").toInt(), 0);
    PB_CHECK(r.value("stoppedBy").toString().isEmpty());
    PB_CHECK(::access((e.root + "/Books/story.epub").c_str(), R_OK) == 0);
    // a change on the server: the next Update fetches exactly that
    e.server.set_file("/Books/small.txt", "hello, changed", 1790009000);
    e.server.clear_log();
    e.bridge->update();
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(e.bridge->updateReport().value("updated").toInt(), 1);
    PB_CHECK_EQ(e.server.count("GET"), size_t(1));
}

PB_TEST(a_large_update_asks_for_confirmation_with_the_totals_before_it_downloads) {
    Env e;
    PB_CHECK(e.store->set_setting("large_file_warn_mib", "1").ok());
    e.server.add_file("/Big/one.bin", pattern(2 * 1024 * 1024));
    e.bridge->setKept("/Big", "folder", true);
    QSignalSpy ask(e.bridge.get(), &TransferBridge::updateNeedsConfirm);
    QSignalSpy finished(e.bridge.get(), &TransferBridge::updateFinished);
    e.bridge->update();
    PB_CHECK(ask.wait(20000));
    PB_CHECK_EQ(ask.count(), 1);
    PB_CHECK(!ask.at(0).at(0).toString().isEmpty());   // the planned size
    PB_CHECK_EQ(e.server.count("GET"), size_t(0));      // nothing downloaded yet
    PB_CHECK(e.bridge->updateRunning());
    e.bridge->confirmUpdate(true);
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(e.bridge->updateReport().value("added").toInt(), 1);
    // declined: nothing is fetched
    e.server.add_file("/Big/two.bin", pattern(2 * 1024 * 1024));
    e.server.clear_log();
    e.bridge->update();
    PB_CHECK(ask.wait(20000));
    e.bridge->confirmUpdate(false);
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(e.server.count("GET"), size_t(0));
    PB_CHECK(!e.bridge->updateRunning());
}

PB_TEST(an_update_can_be_cancelled_and_does_not_start_during_a_download) {
    Env e;
    e.bridge->setKept("/Books", "folder", true);
    e.server.add_file("/Books/big.bin", pattern(3 * 1024 * 1024));
    e.list("/Books");
    e.bridge->download("/Books/big.bin", true);
    e.bridge->update();   // a download is running: the Update waits for the user
    PB_CHECK(!e.bridge->updateRunning());
    e.settle();
    QSignalSpy finished(e.bridge.get(), &TransferBridge::updateFinished);
    e.bridge->update();
    e.bridge->cancelUpdate();
    PB_CHECK(finished.wait(20000));
    const QVariantMap r = e.bridge->updateReport();
    PB_CHECK(r.value("stoppedBy").toString() == "cancelled" || r.value("added").toInt() >= 0);
}

PB_TEST_MAIN()
