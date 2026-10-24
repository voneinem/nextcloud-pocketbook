// "No background network activity" (constitution Principle VI, spec FR-034, SC-010): with Wi-Fi on, the app sends exactly one request at
// start and then nothing until a tap asks for something. Each story extends this file with the taps that must stay silent.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include <sys/stat.h>

#include "browse_bridge.h"
#include "connection_bridge.h"
#include "fake_storage.h"
#include "fake_nextcloud_server.h"
#include "pbc/store.h"
#include "pbc/folder_service.h"
#include "queue_bridge.h"
#include "share_bridge.h"
#include "sync_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "transfer_bridge.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_no_background_network";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct FixedClock : pbc::Clock {
    int64_t t;
    explicit FixedClock(int64_t v) : t(v) {}
    int64_t now() const override { return t; }
};

// The event loop runs for a while, like an idle app would: if anything schedules a request, it would show up.
void idle(int milliseconds) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(10);
    }
}

// The downloads of files: GET requests on the WebDAV tree (the account check is a GET as well, on the capabilities).
size_t file_gets(const pbtest::FakeNextcloud& server) {
    size_t n = 0;
    for (const auto& r : server.log())
        if (r.method == "GET" && r.path.find("/dav/files/") != std::string::npos) ++n;
    return n;
}

struct App {
    pbtest::TmpDir dir;
    pbtest::FakeNextcloud server;
    FixedClock clock{server.server_time()};
    std::unique_ptr<pbc::Store> store;
    Worker worker;
    bool wifi = true;
    std::unique_ptr<ConnectionBridge> connection;
    pbtest::FakeStorage storage;
    std::unique_ptr<TransferBridge> transfer;
    std::unique_ptr<BrowseBridge> browse;
    std::unique_ptr<QueueBridge> queue;
    std::unique_ptr<SyncBridge> sync;
    std::unique_ptr<ShareBridge> share;
    std::string picker_root;

    // The queue with Auto Sync's quiet period shortened to milliseconds. The reader's storage is a folder of the test.
    void make_queue(int quiet_ms) {
        picker_root = dir.file("root");
        ::mkdir(picker_root.c_str(), 0700);
        ::mkdir((picker_root + "/Downloads").c_str(), 0700);
        QueueBridge::Config q;
        q.store = store.get();
        q.worker = &worker;
        q.http = &server;
        q.clock = &clock;
        q.rules.root = picker_root;
        q.wifi_up = [this] { return wifi; };
        q.ensure_network = [this](std::function<void(bool)> done) { done(wifi); };
        q.download_root = dir.file("PBCloud");
        q.auto_sync_quiet_ms = quiet_ms;
        queue = std::make_unique<QueueBridge>(q);
    }
    // The sync with the pairs "Books" (start-up and Auto Sync as given) and the reader's storage as a folder of the test. Like main.cpp, the start-up
    // comparison is connected to the end of the account check, once.
    void make_sync(int quiet_ms, bool startup, bool auto_sync, int pairs = 1) {
        picker_root = dir.file("root");
        ::mkdir(picker_root.c_str(), 0700);
        for (const char* d : {"Downloads", "Books", "Books2"}) ::mkdir((picker_root + "/" + d).c_str(), 0700);
        SyncBridge::Config s;
        s.store = store.get();
        s.worker = &worker;
        s.http = &server;
        s.clock = &clock;
        s.rules.root = picker_root;
        s.download_root = dir.file("PBCloud");
        s.wifi_up = [this] { return wifi; };
        s.ensure_network = [this](std::function<void(bool)> done) { done(wifi); };
        s.format_date_time = [](qint64 t) { return QString::number(t); };
        s.storage = &storage;
        s.downloader_options.sleep_ms = [](int) {};
        s.auto_sync_quiet_ms = quiet_ms;
        sync = std::make_unique<SyncBridge>(s);
        server.add_folder("/Books");
        server.add_file("/Books/one.epub", "first");
        server.add_folder("/Books2");
        server.add_file("/Books2/two.epub", "second");
        for (int i = 0; i < pairs; ++i) {
            QVariantMap m;
            m["readerFolder"] = QString::fromStdString(picker_root + (i == 0 ? "/Books" : "/Books2"));
            m["remoteFolder"] = QString::fromLatin1(i == 0 ? "/Books" : "/Books2");
            m["startupSync"] = startup;
            const QVariantMap added = sync->addPair(m);
            if (!added.value("ok").toBool()) PB_FAIL_AT("addPair failed: " + added.value("errorKey").toString().toStdString());
            auto pair = store->sync_pair(added.value("id").toLongLong());
            pair->auto_sync = auto_sync;   // set in the store: saving through the bridge would itself arm the pair
            if (!store->sync_pair_update(*pair).ok()) PB_FAIL_AT("sync_pair_update failed");
        }
        QObject::connect(connection.get(), &ConnectionBridge::startCheckFinished, sync.get(), [this](bool ok) {
            if (ok) sync->startupCompare();
        });
        server.clear_log();
    }
    // The share bridge as main.cpp wires it: a tap may ask the system for Wi-Fi (counted in network_requests), the clipboard on. /Docs/Reports is listed and cached.
    int network_requests = 0;
    int hand_offs = 0;           // the hand-off to the mail app (feature 005): counted, never a network request
    int hand_off_answer = 0;
    void make_share() {
        ShareBridge::Config c;
        c.store = store.get();
        c.worker = &worker;
        c.http = &server;
        c.clock = &clock;
        c.wifi_up = [this] { return wifi; };
        c.ensure_network = [this](std::function<void(bool)> done) {
            ++network_requests;
            done(wifi);
        };
        c.clipboard_available = true;
        c.hand_off = [this](const std::vector<std::string>&) {
            ++hand_offs;
            return hand_off_answer;
        };
        share = std::make_unique<ShareBridge>(c);
        server.add_folder("/Docs");
        server.add_folder("/Docs/Reports");
        pbc::FolderService fs(*store, server, clock);
        if (!fs.load("/Docs", pbc::Fetch::Always).error.ok()) PB_FAIL_AT("cannot cache /Docs");
        server.clear_log();
    }
    size_t share_requests() const {
        size_t n = 0;
        for (const auto& r : server.log())
            if (r.path.find("/files_sharing/") != std::string::npos) ++n;
        return n;
    }
    QString file(const std::string& rel, const std::string& data) {
        const std::string path = picker_root + "/" + rel;
        FILE* f = ::fopen(path.c_str(), "wb");
        if (!f) PB_FAIL_AT("cannot make " + path);
        ::fwrite(data.data(), 1, data.size(), f);
        ::fclose(f);
        return QString::fromStdString(path);
    }

    App() {
        auto r = pbc::Store::open(dir.file("data"), clock);
        if (!r.ok()) PB_FAIL_AT("Store::open failed: " + r.error.key);
        store = std::move(r.value);
        ConnectionBridge::Config c;
        c.store = store.get();
        c.worker = &worker;
        c.http = &server;
        c.clock = &clock;
        c.wifi_up = [this] { return wifi; };
        c.ensure_network = [this](std::function<void(bool)> done) { done(wifi); };
        connection = std::make_unique<ConnectionBridge>(c);
        const std::string root = dir.file("PBCloud");
        ::mkdir(root.c_str(), 0700);
        TransferBridge::Config t;
        t.store = store.get();
        t.worker = &worker;
        t.http = &server;
        t.clock = &clock;
        t.storage = &storage;
        t.download_root = root;
        t.ensure_network = c.ensure_network;
        t.wifi_up = c.wifi_up;
        t.options.sleep_ms = [](int) {};
        transfer = std::make_unique<TransferBridge>(t);
        BrowseBridge::Config b;
        b.store = store.get();
        b.worker = &worker;
        b.http = &server;
        b.clock = &clock;
        b.ensure_network = c.ensure_network;
        b.download_root = root;
        browse = std::make_unique<BrowseBridge>(b);
    }
    ~App() {
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    void sign_in() {
        QSignalSpy spy(connection.get(), &ConnectionBridge::testFinished);
        connection->testConnection(QString::fromStdString(server.base_url()), "demo", "app-pass-0001");
        if (!spy.wait(10000)) PB_FAIL_AT("the Test did not finish");
        server.clear_log();
    }
};

}  // namespace

PB_TEST(with_wifi_on_the_app_sends_one_request_at_start_and_then_nothing) {
    App a;
    a.sign_in();
    a.connection->checkAtStart();
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(1));
    PB_CHECK(a.server.log()[0].path.find("/cloud/capabilities") != std::string::npos);
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(1));   // nothing follows on its own
}

PB_TEST(without_wifi_nothing_is_sent_at_all) {
    App a;
    a.sign_in();
    a.wifi = false;
    a.connection->checkAtStart();
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
}

PB_TEST(without_an_account_nothing_is_sent_at_start) {
    App a;
    a.connection->checkAtStart();
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
}

PB_TEST(kept_items_are_never_checked_or_downloaded_without_the_tap_on_update) {
    App a;
    a.sign_in();
    a.server.add_file("/Books/small.txt", "hello");
    a.server.add_file("/Books/story.epub", std::string(2000, 's'));
    a.transfer->setKept("/Books", "folder", true);
    a.connection->checkAtStart();
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(1));   // only the account check of the start
    PB_CHECK_EQ(a.server.count("PROPFIND"), size_t(0));
    PB_CHECK_EQ(file_gets(a.server), size_t(0));
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(1));
    PB_CHECK(!a.transfer->updateRunning());

    // the tap: now, and only now, the kept items are checked and fetched
    a.server.clear_log();
    QSignalSpy finished(a.transfer.get(), &TransferBridge::updateFinished);
    a.transfer->update();
    PB_CHECK(finished.wait(20000));
    PB_CHECK(a.server.count("PROPFIND") >= 1);
    PB_CHECK(file_gets(a.server) >= 1);
    // and afterwards it is quiet again
    a.server.clear_log();
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
}

PB_TEST(without_wifi_an_update_sends_nothing) {
    App a;
    a.sign_in();
    a.transfer->setKept("/Books", "folder", true);
    a.wifi = false;
    QSignalSpy finished(a.transfer.get(), &TransferBridge::updateFinished);
    a.transfer->update();
    if (finished.count() == 0) finished.wait(20000);   // without Wi-Fi it ends at once
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
    PB_CHECK_EQ(a.transfer->updateReport().value("stoppedBy").toString().toStdString(), std::string("no_network"));
}

PB_TEST(a_search_typed_offline_and_the_favorites_without_wifi_send_nothing) {
    App a;
    a.sign_in();
    a.server.add_file("/Books/moby.epub", "x");
    a.server.set_favorite("/Books/moby.epub", true);
    a.wifi = false;
    for (const char* term : {"m", "mo", "mob", "moby", "moby ", "moby d"}) a.browse->searchOffline(term);   // typing, letter by letter
    a.browse->showFavorites();   // never fetched and no Wi-Fi: nothing to show, nothing sent
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
    PB_CHECK_EQ(a.browse->errorKey().toStdString(), std::string("no_network"));

    // with Wi-Fi on, typing is still silent: only the tap on "Also search the server" sends the search
    a.wifi = true;
    a.server.clear_log();
    a.browse->searchOffline("moby");
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
    QSignalSpy changed(a.browse.get(), &BrowseBridge::changed);
    a.browse->searchServer("moby");
    QElapsedTimer t;
    t.start();
    while (a.browse->busy() && t.elapsed() < 10000) QTest::qWait(5);
    PB_CHECK_EQ(a.server.count("SEARCH"), size_t(1));
    PB_CHECK_EQ(a.server.request_count(), size_t(1));
    a.server.clear_log();
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
}

// Auto Sync (specs/002-manage-server-files, SC-003, research R-14): the only thing that may start a request besides a tap is the one quiet period
// after the user's own action. Nothing else: not the start, not Wi-Fi coming up, not a failed item, not a finished send, not reading properties.
PB_TEST(a_queue_left_by_an_earlier_session_is_not_sent_at_start_when_wifi_comes_up_or_when_properties_are_read) {
    App a;
    a.sign_in();
    a.server.add_folder("/Docs");
    a.make_queue(40);
    const QString f = a.file("Downloads/a.txt", "aaa");
    // queued in an earlier session: it is in the database, nobody acted in this one
    pbc::QueueItem item;
    item.kind = pbc::ItemKind::Upload;
    item.server = a.server.base_url();
    item.user_id = "demo";
    item.src_local = f.toStdString();
    item.src_size = 3;
    item.dst_parent = "/Docs";
    item.dst_name = "a.txt";
    PB_CHECK(a.store->queue_add(item).ok());
    a.make_queue(40);   // the app starts
    a.wifi = false;
    a.connection->checkAtStart();
    idle(300);
    a.wifi = true;      // Wi-Fi comes up
    (void)a.queue->openCount();
    (void)a.queue->items();
    (void)a.queue->offline();
    (void)a.queue->waitingNotice();
    a.queue->refresh();
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
    PB_CHECK_EQ(a.queue->openCount(), 1);
}

PB_TEST(the_users_own_action_is_the_only_trigger_and_after_the_send_it_is_quiet_again) {
    App a;
    a.sign_in();
    a.server.add_folder("/Docs");
    a.make_queue(40);
    QSignalSpy finished(a.queue.get(), &QueueBridge::sendFinished);
    a.queue->addUploads("/Docs", {a.file("Downloads/a.txt", "aaa")});
    PB_CHECK(finished.wait(10000));
    PB_CHECK_EQ(a.server.count("PUT"), size_t(1));
    a.server.clear_log();
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
    a.wifi = false;
    idle(200);
    a.wifi = true;      // a Wi-Fi that comes back is not a trigger either
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
    PB_CHECK_EQ(finished.count(), 1);
}

PB_TEST(an_item_that_fails_is_not_tried_again_by_itself_and_with_auto_sync_off_nothing_is_sent) {
    App a;
    a.sign_in();
    a.server.add_folder("/Docs");
    a.make_queue(40);
    QSignalSpy finished(a.queue.get(), &QueueBridge::sendFinished);
    a.server.answer_next("PUT", "/Docs/", 403);
    a.queue->addUploads("/Docs", {a.file("Downloads/a.txt", "aaa")});
    PB_CHECK(finished.wait(10000));
    a.server.clear_log();
    idle(800);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));   // the failed item waits for the user's tap
    a.queue->setAutoSync(false);
    a.queue->addUploads("/Docs", {a.file("Downloads/b.txt", "bbb")});
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
    PB_CHECK_EQ(a.queue->openCount(), 2);
}

PB_TEST(an_unknown_item_leaving_the_queue_screen_and_wifi_cycles_start_nothing_and_a_new_action_sends_only_the_new_item) {
    App a;
    a.sign_in();
    a.server.add_folder("/Docs");
    a.make_queue(40);
    QSignalSpy finished(a.queue.get(), &QueueBridge::sendFinished);
    a.server.lose_next_answer("PUT");
    a.queue->addUploads("/Docs", {a.file("Downloads/a.txt", "aaa")});
    PB_CHECK(finished.wait(10000));                       // the one Auto Sync send after the user's action: its answer is lost
    PB_CHECK_EQ(finished.at(0).at(0).toMap().value("unknown").toInt(), 1);
    a.server.clear_log();
    a.queue->leaveScreen();
    a.queue->refresh();
    for (int i = 0; i < 3; ++i) {
        a.wifi = false;
        idle(100);
        a.wifi = true;
        idle(150);
    }
    PB_CHECK_EQ(a.server.request_count(), size_t(0));     // the unknown item is never taken again by itself
    a.queue->addUploads("/Docs", {a.file("Downloads/b.txt", "bbb")});   // a new user action: one more send, of the new item only
    PB_CHECK(finished.wait(10000));
    PB_CHECK_EQ(a.server.count("PUT"), size_t(1));
    idle(500);
    PB_CHECK_EQ(a.server.count("PUT"), size_t(1));
    PB_CHECK_EQ(finished.count(), 2);
}

PB_TEST(without_wifi_at_the_end_of_the_quiet_period_nothing_is_sent_and_nothing_follows_when_wifi_returns) {
    App a;
    a.sign_in();
    a.server.add_folder("/Docs");
    a.make_queue(40);
    a.wifi = false;
    a.queue->addUploads("/Docs", {a.file("Downloads/a.txt", "aaa")});
    idle(400);
    a.wifi = true;
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
    PB_CHECK_EQ(a.queue->openCount(), 1);
}

// Folder sync (specs/003-folder-sync US5, FR-006, SC-005): a comparison starts at the start (after the account check, Wi-Fi already on), after the user's
// own change once the quiet period is over, and on a tap - never otherwise. A comparison reads listings and writes nothing; only an approval transfers.
size_t writes(const pbtest::FakeNextcloud& server) {
    size_t n = 0;
    for (const char* m : {"PUT", "MKCOL", "MOVE", "COPY", "DELETE", "PROPPATCH"}) n += server.count(m);
    return n;
}

PB_TEST(folder_sync_at_start_compares_each_pair_with_the_switch_on_once_and_then_nothing_follows) {
    App a;
    a.sign_in();
    a.make_sync(40, true, false, 2);
    a.connection->checkAtStart();
    idle(900);
    PB_CHECK_EQ(a.server.count("PROPFIND"), size_t(2));   // one listing per pair
    PB_CHECK_EQ(file_gets(a.server), size_t(0));
    PB_CHECK_EQ(writes(a.server), size_t(0));
    PB_CHECK_EQ(a.sync->readyCount(), 2);                  // the plans wait for the user
    a.server.clear_log();
    idle(500);
    a.wifi = false;
    idle(200);
    a.wifi = true;   // a Wi-Fi that comes back, a finished comparison, an idle hour: no trigger
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
}

PB_TEST(folder_sync_at_start_without_wifi_or_with_the_switch_off_sends_nothing) {
    {
        App a;
        a.sign_in();
        a.make_sync(40, true, false);
        a.wifi = false;
        a.connection->checkAtStart();
        idle(600);
        PB_CHECK_EQ(a.server.request_count(), size_t(0));
        a.wifi = true;
        idle(600);
        PB_CHECK_EQ(a.server.request_count(), size_t(0));
    }
    App b;
    b.sign_in();
    b.make_sync(40, false, true);
    b.connection->checkAtStart();
    idle(600);
    PB_CHECK_EQ(b.server.count("PROPFIND"), size_t(0));   // only the account check ran
    PB_CHECK_EQ(b.sync->readyCount(), 0);
}

PB_TEST(folder_sync_auto_sync_compares_one_pair_after_the_quiet_period_and_a_file_written_outside_the_app_is_no_trigger) {
    App a;
    a.sign_in();
    a.make_sync(120, false, true, 2);
    // a file that appears in the reader folder without the app's doing is not noticed: no watching
    a.file("Books/dropped.epub", "from a computer");
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
    // the user's own action below the server folder arms that pair only
    a.sync->noteAction("/Books/Novels");
    idle(40);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));   // the quiet period is not over
    idle(900);
    PB_CHECK_EQ(a.server.count("PROPFIND"), size_t(1));
    PB_CHECK_EQ(file_gets(a.server), size_t(0));
    PB_CHECK_EQ(writes(a.server), size_t(0));
    a.server.clear_log();
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));   // once, then quiet
}

PB_TEST(folder_sync_with_the_global_auto_sync_off_or_no_wifi_at_the_end_of_the_quiet_period_compares_nothing) {
    App a;
    a.sign_in();
    a.make_sync(60, false, true);
    PB_CHECK(a.store->set_setting("auto_sync", "0").ok());
    a.sync->noteAction("/Books/x");
    idle(500);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
    PB_CHECK(a.store->set_setting("auto_sync", "1").ok());
    a.sync->noteAction("/Books/x");
    a.wifi = false;
    idle(500);
    a.wifi = true;
    idle(500);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));   // it waited; nothing retried
}

PB_TEST(folder_sync_transfers_only_after_the_approval) {
    App a;
    a.sign_in();
    a.make_sync(40, true, false);
    a.connection->checkAtStart();
    idle(900);
    PB_CHECK_EQ(file_gets(a.server), size_t(0));
    PB_CHECK_EQ(writes(a.server), size_t(0));
    const QVariantList pairs = a.sync->pairs();
    PB_CHECK_EQ(pairs.size(), 1);
    const auto plan = a.store->sync_ready_plan(pairs.at(0).toMap().value("id").toLongLong());
    PB_CHECK(plan.has_value());
    QSignalSpy finished(a.sync.get(), &SyncBridge::runFinished);
    PB_CHECK(a.sync->approve(static_cast<qlonglong>(*plan)).value("ok").toBool());
    PB_CHECK(finished.wait(10000));
    PB_CHECK_EQ(file_gets(a.server), size_t(1));
    a.server.clear_log();
    idle(500);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
}

// ---- share links (specs/004-share-links FR-030, SC-009) ----

PB_TEST(sharing_sends_nothing_at_start_while_idle_when_wifi_cycles_or_when_a_screen_is_left_and_only_taps_read) {
    App a;
    a.sign_in();
    a.connection->checkAtStart();
    idle(300);
    a.make_share();
    a.server.clear_log();
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));   // nothing from sharing at start or while idle
    a.wifi = false;
    idle(150);
    a.wifi = true;
    idle(300);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));   // a Wi-Fi that comes and goes is no trigger
    PB_CHECK(a.share->open("/Docs/Reports").value("ok").toBool());   // the tap
    QElapsedTimer t;
    t.start();
    while (a.share->busy() && t.elapsed() < 10000) QTest::qWait(5);
    PB_CHECK_EQ(a.server.request_count(), size_t(3));   // the capabilities and the two lists, once
    a.server.clear_log();
    idle(600);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));   // the open screen asks for nothing more by itself
    a.share->onApplicationStateChanged(Qt::ApplicationInactive);   // leaving the screen is silent too
    a.wifi = false;
    idle(100);
    a.wifi = true;
    idle(300);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
}

PB_TEST(sharing_asks_the_system_for_wifi_only_after_a_tap_and_sends_nothing_when_it_stays_off) {
    App a;
    a.sign_in();
    a.make_share();
    a.wifi = false;
    idle(400);
    PB_CHECK_EQ(a.network_requests, 0);   // nothing by itself: not at start, not while idle
    PB_CHECK(a.share->open("/Docs/Reports").value("ok").toBool());   // the tap asks once ...
    idle(300);
    PB_CHECK_EQ(a.network_requests, 1);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));   // ... and with Wi-Fi still off nothing is sent
    a.wifi = true;   // and when it comes up later nothing follows on its own
    idle(400);
    PB_CHECK_EQ(a.server.request_count(), size_t(0));
    PB_CHECK_EQ(a.network_requests, 1);
}

PB_TEST(mailing_a_link_sends_no_request_and_does_not_ask_for_wifi_also_when_the_reader_is_offline_and_nothing_follows_by_itself) {
    for (int answer : {0, 1}) {
        App a;
        a.sign_in();
        a.make_share();
        a.hand_off_answer = answer;
        pbc::ShareDetail d;
        d.row.id = 7;
        d.row.kind = pbc::ShareKind::PublicLink;
        d.row.owned_by_me = true;
        d.link = "https://cloud.example.org/s/TOKENforTheTest0123456789";
        d.link_state = pbc::LinkState::Ok;
        a.share->setDetail(d);
        a.wifi = false;
        idle(300);
        PB_CHECK_EQ(a.hand_offs, 0);   // nothing at start and nothing while idle
        a.share->mailLink(true);
        idle(400);
        PB_CHECK_EQ(a.hand_offs, 1);                         // the user's tap is the only trigger
        PB_CHECK_EQ(a.network_requests, 0);                  // no Wi-Fi is asked for: the hand-off needs none
        PB_CHECK_EQ(a.server.request_count(), size_t(0));    // no request, not even when Wi-Fi comes back
        a.wifi = true;
        idle(400);
        PB_CHECK_EQ(a.hand_offs, 1);
        PB_CHECK_EQ(a.server.request_count(), size_t(0));
        PB_CHECK_EQ(a.server.link_hits(), size_t(0));
    }
}

PB_TEST(the_auto_sync_timers_of_the_queue_and_the_pairs_never_start_a_sharing_request) {
    App a;
    a.sign_in();
    a.make_share();
    a.make_queue(40);
    QSignalSpy finished(a.queue.get(), &QueueBridge::sendFinished);
    a.queue->addUploads("/Docs", {a.file("Downloads/a.txt", "aaa")});
    PB_CHECK(finished.wait(10000));
    idle(300);
    PB_CHECK_EQ(a.share_requests(), size_t(0));
}

PB_TEST_MAIN()
