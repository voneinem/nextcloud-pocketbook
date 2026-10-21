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
#include "queue_bridge.h"
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

PB_TEST_MAIN()
