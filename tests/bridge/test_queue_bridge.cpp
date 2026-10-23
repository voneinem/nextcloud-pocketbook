// The QueueBridge (contracts/bridge-qml.md): what the user's picks put into the queue and how the queue is shown. Real core, fake Nextcloud
// only to prove that nothing is sent. A pick, a property read and opening the queue never make a request (FR-011).
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/folder_service.h"
#include "pbc/store.h"
#include "queue_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_queue_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct FixedClock : pbc::Clock {
    int64_t t;
    explicit FixedClock(int64_t v) : t(v) {}
    int64_t now() const override { return t; }
};

void touch(const std::string& path, const std::string& data = "x") {
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    PB_CHECK(fd >= 0);
    if (!data.empty()) PB_CHECK(::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    ::close(fd);
}
void mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0700);
}

struct Env {
    pbtest::TmpDir dir;
    pbtest::FakeNextcloud server;
    FixedClock clock{server.server_time()};
    std::unique_ptr<pbc::Store> store;
    Worker worker;
    bool wifi = true;
    bool net_ok = true;       // what ensure_network answers
    int ensure_calls = 0;
    std::vector<std::string> actions;   // the folders the bridge told about the user's own actions (specs/003-folder-sync)
    int quiet_ms = 600000;    // Auto Sync's quiet period: long, so that only the tests of Auto Sync ever see it end
    std::string root;
    std::unique_ptr<QueueBridge> queue;

    Env() {
        root = dir.file("root");
        mkdirs(root + "/Downloads");
        mkdirs(root + "/Notes");
        mkdirs(root + "/system/pbcloud");
        auto r = pbc::Store::open(dir.file("data"), clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
        store = std::move(r.value);
        pbc::AccountRow a;
        a.base_url = server.base_url();
        a.login_name = "demo";
        a.user_id = "demo";
        a.state = "ok";
        PB_CHECK(store->save_account(a).ok());
        server.add_folder("/Docs");
        pbc::Credentials cr;
        cr.password = server.app_password();
        PB_CHECK(pbc::save_credentials(store->directory(), cr).ok());
        pbc::ServerCaps caps;
        caps.trash = true;
        caps.chunking = true;
        PB_CHECK(store->save_server_caps(caps).ok());
        build();
        server.clear_log();
    }
    void build() {
        queue.reset();
        QueueBridge::Config c;
        c.store = store.get();
        c.worker = &worker;
        c.http = &server;
        c.clock = &clock;
        c.rules.root = root;
        c.rules.deny = {root + "/system/pbcloud"};
        c.wifi_up = [this] { return wifi; };
        c.ensure_network = [this](std::function<void(bool)> done) {
            ++ensure_calls;
            done(net_ok);
        };
        c.format_date_time = [](qint64 t) { return QString::number(t); };
        c.download_root = dir.file("PBCloud");
        c.auto_sync_quiet_ms = quiet_ms;
        c.on_user_action = [this](const std::string& folder) { actions.push_back(folder); };
        queue = std::make_unique<QueueBridge>(c);
    }
    ~Env() {
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    void cache(const std::string& folder) {
        pbc::FolderService fs(*store, server, clock);
        PB_CHECK(fs.load(folder, pbc::Fetch::Always).error.ok());
        server.clear_log();
    }
    QString file(const std::string& rel, const std::string& data = "content") {
        const std::string p = root + "/" + rel;
        mkdirs(p.substr(0, p.rfind('/')));
        touch(p, data);
        return QString::fromStdString(p);
    }
};

// Runs the event loop until the condition holds (the worker answers through queued calls) or the time is up.
template <class F>
bool until(F cond, int timeout_ms = 20000) {
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < timeout_ms) QTest::qWait(5);
    return cond();
}
void wait_ms(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QTest::qWait(5);
}
QString state_of(Env& e, int id) {
    const auto it = e.store->queue_item(id);
    return it ? QString::fromLatin1(pbc::item_state_text(it->state)) : QStringLiteral("gone");
}

}  // namespace

PB_TEST(picking_files_queues_them_for_the_folder_and_makes_no_request) {
    Env e;
    QSignalSpy changed(e.queue.get(), &QueueBridge::changed);
    const QVariantMap r = e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf", "aaaa"), e.file("Downloads/b.epub", "bb")});
    PB_CHECK_EQ(r.value("queued").toInt(), 2);
    PB_CHECK(r.value("errorKey").toString().isEmpty());
    PB_CHECK(r.value("refused").toList().isEmpty());
    PB_CHECK(changed.count() > 0);
    PB_CHECK_EQ(e.queue->openCount(), 2);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    // the rows
    const QVariantList items = e.queue->items();
    PB_CHECK_EQ(items.size(), 2);
    const QVariantMap first = items.at(0).toMap();
    PB_CHECK_EQ(first.value("name").toString().toStdString(), std::string("a.pdf"));
    PB_CHECK_EQ(first.value("kind").toString().toStdString(), std::string("upload"));
    PB_CHECK_EQ(first.value("state").toString().toStdString(), std::string("queued"));
    PB_CHECK_EQ(first.value("targetText").toString().toStdString(), std::string("/Docs"));
    PB_CHECK_EQ(first.value("sizeText").toString().toStdString(), std::string("4 B"));
    PB_CHECK(!first.value("canRetry").toBool());
    PB_CHECK(!first.value("canResolve").toBool());
    // the listing of the target folder shows them as pending rows (the store's overlay, read without any request)
    const auto page = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 10);
    PB_CHECK_EQ(page.total, 2);
}

PB_TEST(the_queue_is_written_before_the_bridge_answers_and_survives_a_new_bridge) {
    Env e;
    e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf")});
    PB_CHECK_EQ(e.store->queue_count_open(), 1);   // already in the database
    e.build();                                      // the app was restarted
    PB_CHECK_EQ(e.queue->openCount(), 1);
    PB_CHECK_EQ(e.queue->items().at(0).toMap().value("name").toString().toStdString(), std::string("a.pdf"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(files_that_cannot_be_sent_are_refused_one_by_one_with_their_key) {
    Env e;
    const QVariantMap r = e.queue->addUploads("/Docs", {e.file("Downloads/ok.txt"), e.file("Downloads/empty.txt", ""), e.file("system/pbcloud/credentials"),
                                                        QStringLiteral("/etc/hostname")});
    PB_CHECK_EQ(r.value("queued").toInt(), 1);
    const QVariantList refused = r.value("refused").toList();
    PB_CHECK_EQ(refused.size(), 3);
    QStringList keys;
    for (const QVariant& v : refused) keys << v.toMap().value("key").toString();
    keys.sort();
    PB_CHECK_EQ(keys.join(",").toStdString(), std::string("source_not_allowed,source_not_allowed,source_unreadable"));
    for (const QVariant& v : refused) {   // only a display name, never a path
        PB_CHECK(v.toMap().value("name").toString().indexOf('/') < 0);
    }
}

PB_TEST(a_name_the_server_forbids_is_refused_with_name_forbidden) {
    Env e;
    pbc::ServerCaps caps;
    caps.forbidden_names = {".htaccess"};
    PB_CHECK(e.store->save_server_caps(caps).ok());
    const QVariantMap r = e.queue->addUploads("/Docs", {e.file("Downloads/.htaccess")});
    PB_CHECK_EQ(r.value("queued").toInt(), 0);
    PB_CHECK_EQ(r.value("refused").toList().at(0).toMap().value("key").toString().toStdString(), std::string("name_forbidden"));
}

PB_TEST(a_folder_pick_is_counted_first_and_queued_only_after_the_confirmation) {
    Env e;
    e.file("Notes/trip/plan.md", "plan");
    e.file("Notes/trip/photos/a.jpg", "aa");
    const QVariantMap pick = e.queue->addFolderPick("/Docs", {QString::fromStdString(e.root + "/Notes/trip")});
    PB_CHECK(pick.value("errorKey").toString().isEmpty());
    PB_CHECK_EQ(pick.value("folders").toInt(), 2);
    PB_CHECK_EQ(pick.value("files").toInt(), 2);
    PB_CHECK_EQ(pick.value("bytes").toLongLong(), 6LL);
    PB_CHECK(!pick.value("overLimit").toBool());
    PB_CHECK(!pick.value("token").toString().isEmpty());
    PB_CHECK_EQ(e.queue->openCount(), 0);                      // counting queues nothing
    const QVariantMap done = e.queue->confirmFolderPick(pick.value("token").toString());
    PB_CHECK_EQ(done.value("queued").toInt(), 4);               // two folders, two files
    PB_CHECK_EQ(e.queue->openCount(), 4);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    // the same token cannot be confirmed twice
    PB_CHECK_EQ(e.queue->confirmFolderPick(pick.value("token").toString()).value("queued").toInt(), 0);
    PB_CHECK_EQ(e.queue->openCount(), 4);
    // the rows: folders and files of the group, dependants waiting for their folder
    int waiting = 0;
    for (const QVariant& v : e.queue->items()) {
        const QVariantMap m = v.toMap();
        if (m.value("state").toString() == QLatin1String("waiting")) {
            ++waiting;
            PB_CHECK(!m.value("waitingFor").toString().isEmpty());
        }
    }
    PB_CHECK_EQ(waiting, 3);   // everything but the top folder waits for something
}

PB_TEST(a_pick_over_the_limit_is_refused_as_a_whole_and_an_unknown_token_queues_nothing) {
    Env e;
    for (int i = 0; i < pbc::kMaxQueueItems - 2; ++i) {
        pbc::QueueItem q;
        q.kind = pbc::ItemKind::Upload;
        q.server = e.server.base_url();
        q.user_id = "demo";
        q.src_local = "/mnt/ext1/x" + std::to_string(i);
        q.dst_parent = "/Docs";
        q.dst_name = "n" + std::to_string(i);
        PB_CHECK(e.store->queue_add(q).ok());
    }
    e.file("Notes/trip/a.txt");
    e.file("Notes/trip/b.txt");
    const QVariantMap pick = e.queue->addFolderPick("/Docs", {QString::fromStdString(e.root + "/Notes/trip")});
    PB_CHECK_EQ(pick.value("items").toInt(), 3);
    const QVariantMap done = e.queue->confirmFolderPick(pick.value("token").toString());
    PB_CHECK_EQ(done.value("queued").toInt(), 0);
    PB_CHECK_EQ(done.value("errorKey").toString().toStdString(), std::string("queue_full"));
    PB_CHECK_EQ(e.queue->openCount(), pbc::kMaxQueueItems - 2);
    PB_CHECK_EQ(e.queue->confirmFolderPick("no-such-token").value("queued").toInt(), 0);
}

PB_TEST(offline_picks_are_queued_exactly_as_online_and_the_bridge_says_it_is_offline) {
    Env e;
    e.wifi = false;
    PB_CHECK(e.queue->offline());
    const QVariantMap r = e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf")});
    PB_CHECK_EQ(r.value("queued").toInt(), 1);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.wifi = true;
    e.queue->refresh();
    PB_CHECK(!e.queue->offline());
}

PB_TEST(removing_a_queued_item_removes_the_row_and_leaves_the_file) {
    Env e;
    const QString f = e.file("Downloads/a.pdf", "aaaa");
    e.queue->addUploads("/Docs", {f});
    const int id = e.queue->items().at(0).toMap().value("id").toInt();
    e.queue->remove(id);
    PB_CHECK_EQ(e.queue->openCount(), 0);
    PB_CHECK(e.queue->items().isEmpty());
    PB_CHECK_EQ(e.dir.read("root/Downloads/a.pdf"), std::string("aaaa"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(removing_a_new_folder_fails_what_waits_for_it_and_shows_why) {
    Env e;
    e.file("Notes/trip/a.txt");
    const QVariantMap pick = e.queue->addFolderPick("/Docs", {QString::fromStdString(e.root + "/Notes/trip")});
    e.queue->confirmFolderPick(pick.value("token").toString());
    int folder_id = 0;
    for (const QVariant& v : e.queue->items())
        if (v.toMap().value("kind").toString() == QLatin1String("mkdir")) folder_id = v.toMap().value("id").toInt();
    e.queue->remove(folder_id);
    const QVariantList items = e.queue->items();
    PB_CHECK_EQ(items.size(), 1);
    PB_CHECK_EQ(items.at(0).toMap().value("state").toString().toStdString(), std::string("failed"));
    PB_CHECK_EQ(items.at(0).toMap().value("reason").toString().toStdString(), std::string("target_gone"));
    PB_CHECK(items.at(0).toMap().value("canRetry").toBool());
}

PB_TEST(items_of_another_account_are_shown_as_such_and_the_queue_pages) {
    Env e;
    for (int i = 0; i < 30; ++i) {
        pbc::QueueItem q;
        q.kind = pbc::ItemKind::Upload;
        q.server = e.server.base_url();
        q.user_id = i == 7 ? "someone" : "demo";
        q.src_local = "/mnt/ext1/x" + std::to_string(i);
        q.dst_parent = "/Docs";
        q.dst_name = "n" + std::to_string(100 + i);
        PB_CHECK(e.store->queue_add(q).ok());
    }
    e.queue->setPageSize(10);
    e.queue->refresh();
    PB_CHECK_EQ(e.queue->total(), 30);
    PB_CHECK_EQ(e.queue->pageCount(), 3);
    PB_CHECK_EQ(e.queue->items().size(), 10);
    e.queue->nextPage();
    PB_CHECK_EQ(e.queue->page(), 1);
    PB_CHECK_EQ(e.queue->items().at(0).toMap().value("name").toString().toStdString(), std::string("n110"));
    e.queue->goToPage(0);
    PB_CHECK_EQ(e.queue->items().at(7).toMap().value("state").toString().toStdString(), std::string("other_account"));
    e.queue->previousPage();
    PB_CHECK_EQ(e.queue->page(), 0);
}

PB_TEST(leaving_the_screen_clears_the_done_rows_only) {
    Env e;
    e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf"), e.file("Downloads/b.pdf")});
    auto first = e.store->queue_page(0, 1).rows[0];
    first.state = pbc::ItemState::Done;
    PB_CHECK(e.store->queue_update(first).ok());
    e.queue->refresh();
    PB_CHECK_EQ(e.queue->items().size(), 2);
    e.queue->leaveScreen();
    PB_CHECK_EQ(e.queue->items().size(), 1);
    PB_CHECK_EQ(e.queue->openCount(), 1);
}

PB_TEST(constructing_the_bridge_and_reading_its_properties_sends_nothing) {
    Env e;
    (void)e.queue->openCount();
    (void)e.queue->items();
    (void)e.queue->offline();
    (void)e.queue->total();
    e.build();
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.queue->openCount(), 0);
}

PB_TEST(send_ensures_the_network_runs_the_queue_on_the_worker_and_reports) {
    Env e;
    e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf", "aaaa"), e.file("Downloads/b.pdf", "bb")});
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(e.queue->running());
    PB_CHECK_EQ(e.ensure_calls, 1);
    PB_CHECK(finished.wait(20000));
    PB_CHECK(!e.queue->running());
    const QVariantMap rep = finished.at(0).at(0).toMap();
    PB_CHECK_EQ(rep.value("done").toInt(), 2);
    PB_CHECK_EQ(rep.value("failed").toInt() + rep.value("unknown").toInt() + rep.value("conflict").toInt(), 0);
    PB_CHECK(rep.value("stoppedBy").toString().isEmpty());
    PB_CHECK_EQ(e.server.count("PUT"), size_t(2));
    PB_CHECK_EQ(e.queue->openCount(), 0);
    PB_CHECK(e.queue->waitingNotice().isEmpty());
    PB_CHECK_EQ(e.queue->runStage().toStdString(), std::string("done"));
    PB_CHECK_EQ(e.queue->lastRunCounts().value("done").toInt(), 2);
    PB_CHECK(!e.queue->lastRunText().isEmpty());
    for (const QVariant& v : e.queue->items()) PB_CHECK_EQ(v.toMap().value("state").toString().toStdString(), std::string("done"));
}

PB_TEST(a_send_that_cannot_get_wifi_sends_nothing_and_the_queue_waits) {
    Env e;
    e.net_ok = false;
    e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf")});
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.queue->waitingNotice().toStdString(), std::string("offline"));
    PB_CHECK_EQ(e.queue->openCount(), 1);
    PB_CHECK(!e.queue->running());
}

PB_TEST(the_connection_going_away_stops_the_run_and_says_offline_without_a_retry) {
    Env e;
    e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf")});
    e.wifi = false;
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(finished.at(0).at(0).toMap().value("stoppedBy").toString().toStdString(), std::string("no_connection"));
    PB_CHECK_EQ(e.queue->waitingNotice().toStdString(), std::string("offline"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.queue->openCount(), 1);
}

PB_TEST(a_server_that_asks_for_a_pause_or_refuses_the_login_is_named_and_not_hammered) {
    {
        Env e;
        e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf"), e.file("Downloads/b.pdf")});
        e.server.answer_next("PUT", "/Docs/", 503);
        QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
        e.queue->send();
        PB_CHECK(finished.wait(20000));
        PB_CHECK_EQ(e.queue->waitingNotice().toStdString(), std::string("maintenance"));
        PB_CHECK_EQ(e.server.count("PUT"), size_t(1));
    }
    {
        Env e;
        e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf"), e.file("Downloads/b.pdf")});
        e.server.answer_next("PUT", "/Docs/", 429, 1, "30");
        QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
        e.queue->send();
        PB_CHECK(finished.wait(20000));
        PB_CHECK_EQ(e.queue->waitingNotice().toStdString(), std::string("rate_limited"));
        PB_CHECK_EQ(e.server.count("PUT"), size_t(1));
    }
    {
        Env e;
        e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf")});
        e.server.answer_next("PUT", "/Docs/", 401);
        QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
        e.queue->send();
        PB_CHECK(finished.wait(20000));
        PB_CHECK_EQ(e.queue->waitingNotice().toStdString(), std::string("signin"));
        PB_CHECK_EQ(e.queue->openCount(), 1);
    }
}

PB_TEST(a_second_send_while_one_runs_is_ignored_and_cancel_stops_after_the_piece_in_flight) {
    Env e;
    e.queue->addUploads("/Docs", {e.file("Downloads/big.bin", std::string(25u << 20, 'x'))});
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.server.before_next("PUT", "/00002", [&] { e.queue->cancelSend(); });
    e.queue->send();
    e.queue->send();   // ignored
    PB_CHECK_EQ(e.ensure_calls, 1);
    PB_CHECK(finished.wait(60000));
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK_EQ(finished.at(0).at(0).toMap().value("stoppedBy").toString().toStdString(), std::string("cancelled"));
    PB_CHECK_EQ(e.queue->openCount(), 1);
    PB_CHECK_EQ(e.queue->items().at(0).toMap().value("state").toString().toStdString(), std::string("queued"));
    PB_CHECK(!e.server.has("/Docs/big.bin"));
}

PB_TEST(the_progress_of_a_run_is_coarse) {
    Env e;
    e.queue->addUploads("/Docs", {e.file("Downloads/big.bin", std::string(25u << 20, 'x'))});
    QSignalSpy changed(e.queue.get(), &QueueBridge::changed);
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(60000));
    // hashing and sending move in steps of 5 %: far fewer updates than the 64 KiB pieces and chunks that went by
    PB_CHECK(changed.count() < 80);
    PB_CHECK(changed.count() >= 2);
}

PB_TEST(retry_sends_that_one_failed_item_and_nothing_else) {
    Env e;
    const QString a = e.file("Downloads/a.txt", "aaa");
    e.queue->addUploads("/Docs", {a, e.file("Downloads/b.txt", "bbb")});
    const int id_a = e.queue->items().at(0).toMap().value("id").toInt();
    const int id_b = e.queue->items().at(1).toMap().value("id").toInt();
    PB_CHECK_EQ(::unlink(a.toStdString().c_str()), 0);   // the file is gone when its turn comes
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(state_of(e, id_a).toStdString(), std::string("failed"));
    PB_CHECK_EQ(state_of(e, id_b).toStdString(), std::string("done"));
    PB_CHECK(e.queue->items().at(0).toMap().value("canRetry").toBool());
    e.file("Downloads/a.txt", "aaa");
    e.queue->addUploads("/Docs", {e.file("Downloads/c.txt", "ccc")});   // a third, queued item that a retry must not take
    e.server.clear_log();
    finished.clear();
    e.queue->retry(id_a);
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(state_of(e, id_a).toStdString(), std::string("done"));
    PB_CHECK_EQ(e.server.count("PUT"), size_t(1));
    PB_CHECK_EQ(e.queue->openCount(), 1);   // c is still waiting
    e.queue->retry(id_b);                    // not failed: ignored
    PB_CHECK_EQ(e.server.count("PUT"), size_t(1));
}

PB_TEST(an_unknown_outcome_is_checked_decided_or_sent_again_from_the_bridge) {
    Env e;
    e.queue->addUploads("/Docs", {e.file("Downloads/a.txt", "aaa")});
    const int id = e.queue->items().at(0).toMap().value("id").toInt();
    e.server.lose_next_answer("PUT", "/Docs/");
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(finished.at(0).at(0).toMap().value("unknown").toInt(), 1);
    PB_CHECK_EQ(e.queue->items().at(0).toMap().value("state").toString().toStdString(), std::string("unknown"));
    PB_CHECK(e.queue->items().at(0).toMap().value("canResolve").toBool());
    // neither Send nor a later run repeats it
    e.server.clear_log();
    finished.clear();
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));

    QSignalSpy checked(e.queue.get(), &QueueBridge::checkFinished);
    e.queue->checkUnknown(id);
    PB_CHECK(checked.wait(20000));
    PB_CHECK_EQ(checked.at(0).at(1).toString().toStdString(), std::string("arrived"));
    PB_CHECK_EQ(e.queue->checkResult().toStdString(), std::string("arrived"));
    PB_CHECK_EQ(state_of(e, id).toStdString(), std::string("unknown"));   // the check only reports

    e.queue->markDone(id);
    PB_CHECK(until([&] { return state_of(e, id) == QLatin1String("done"); }));
    PB_CHECK_EQ(e.server.count("PUT"), size_t(0));
}

PB_TEST(sending_an_unknown_item_again_ends_in_the_collision_question_not_in_a_copy) {
    Env e;
    e.queue->addUploads("/Docs", {e.file("Downloads/a.txt", "aaa")});
    const int id = e.queue->items().at(0).toMap().value("id").toInt();
    e.server.lose_next_answer("PUT", "/Docs/");
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    e.queue->sendAgain(id);
    PB_CHECK_EQ(state_of(e, id).toStdString(), std::string("queued"));
    finished.clear();
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(state_of(e, id).toStdString(), std::string("conflict"));
    PB_CHECK_EQ(e.server.overwrites(), 0);
    PB_CHECK(!e.server.has("/Docs/a (2).txt"));
}

PB_TEST(removing_an_unknown_upload_with_an_upload_folder_deletes_the_folder) {
    Env e;
    e.queue->addUploads("/Docs", {e.file("Downloads/big.bin", std::string(25u << 20, 'x'))});
    const int id = e.queue->items().at(0).toMap().value("id").toInt();
    e.server.before_next("MOVE", "/.file", [&] { e.server.answer_next("MOVE", "/.file", 502); });
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(60000));
    PB_CHECK_EQ(state_of(e, id).toStdString(), std::string("unknown"));
    PB_CHECK_EQ(e.server.upload_sessions(), size_t(1));
    e.queue->remove(id);
    PB_CHECK(until([&] { return state_of(e, id) == QLatin1String("gone"); }));
    PB_CHECK_EQ(e.server.upload_sessions(), size_t(0));
}

PB_TEST(a_row_left_sending_by_a_death_is_recovered_at_start_without_any_request) {
    Env e;
    e.queue->addUploads("/Docs", {e.file("Downloads/a.txt", "aaa"), e.file("Downloads/b.txt", "bbb")});
    auto one = e.store->queue_page(0, 2).rows[0];
    one.state = pbc::ItemState::Sending;
    one.step = "single";
    PB_CHECK(e.store->queue_update(one).ok());
    auto two = e.store->queue_page(0, 2).rows[1];
    two.state = pbc::ItemState::Sending;
    two.step = "chunks";
    PB_CHECK(e.store->queue_update(two).ok());
    e.build();   // the app starts again
    PB_CHECK_EQ(e.queue->items().at(0).toMap().value("state").toString().toStdString(), std::string("unknown"));
    PB_CHECK_EQ(e.queue->items().at(1).toMap().value("state").toString().toStdString(), std::string("queued"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(auto_sync_is_on_by_default_persists_and_takes_effect_at_once) {
    Env e;
    PB_CHECK(e.queue->autoSync());
    QSignalSpy changed(e.queue.get(), &QueueBridge::changed);
    e.queue->setAutoSync(false);
    PB_CHECK(!e.queue->autoSync());
    PB_CHECK(changed.count() > 0);
    PB_CHECK_EQ(e.store->setting("auto_sync"), std::string("0"));
    e.build();
    PB_CHECK(!e.queue->autoSync());
    e.queue->setAutoSync(true);
    PB_CHECK_EQ(e.store->setting("auto_sync"), std::string("1"));
}

PB_TEST(auto_sync_sends_once_after_the_quiet_period_and_never_switches_wifi_on) {
    Env e;
    e.quiet_ms = 80;
    e.build();
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->addUploads("/Docs", {e.file("Downloads/a.txt", "aaa")});
    wait_ms(30);
    e.queue->addUploads("/Docs", {e.file("Downloads/b.txt", "bbb")});   // the next action restarts the quiet period
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(finished.wait(10000));
    PB_CHECK_EQ(finished.at(0).at(0).toMap().value("done").toInt(), 2);
    PB_CHECK_EQ(e.ensure_calls, 0);
    wait_ms(400);
    PB_CHECK_EQ(finished.count(), 1);                 // once, not again
    PB_CHECK_EQ(e.server.count("PUT"), size_t(2));
    PB_CHECK_EQ(e.queue->openCount(), 0);
}

PB_TEST(with_auto_sync_off_nothing_is_ever_sent_by_itself) {
    Env e;
    e.quiet_ms = 40;
    e.build();
    e.queue->setAutoSync(false);
    e.queue->addUploads("/Docs", {e.file("Downloads/a.txt", "aaa")});
    wait_ms(400);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.queue->openCount(), 1);
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();   // only the tap
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(e.server.count("PUT"), size_t(1));
}

PB_TEST(when_the_quiet_period_ends_without_wifi_the_queue_waits_and_nothing_retries) {
    Env e;
    e.quiet_ms = 40;
    e.build();
    e.wifi = false;
    e.queue->addUploads("/Docs", {e.file("Downloads/a.txt", "aaa")});
    PB_CHECK(until([&] { return e.queue->waitingNotice() == QLatin1String("offline"); }, 5000));
    e.wifi = true;   // Wi-Fi coming back is not a trigger
    wait_ms(400);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.queue->openCount(), 1);
}

PB_TEST(a_failed_item_and_a_finished_send_start_nothing_new) {
    Env e;
    e.quiet_ms = 40;
    e.build();
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.server.answer_next("PUT", "/Docs/", 403);   // the server refuses: the item fails
    e.queue->addUploads("/Docs", {e.file("Downloads/a.txt", "aaa")});
    PB_CHECK(finished.wait(10000));
    PB_CHECK_EQ(finished.at(0).at(0).toMap().value("failed").toInt(), 1);
    wait_ms(500);
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK_EQ(e.server.count("PUT"), size_t(1));   // no automatic retry, ever
    PB_CHECK_EQ(e.queue->items().at(0).toMap().value("state").toString().toStdString(), std::string("failed"));
}

PB_TEST(an_action_during_a_send_is_taken_by_that_run_or_by_one_follow_up_and_then_it_is_quiet) {
    Env e;
    e.quiet_ms = 40;
    e.build();
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->addUploads("/Docs", {e.file("Downloads/big.bin", std::string(12u << 20, 'x'))});
    PB_CHECK(until([&] { return e.queue->running(); }, 10000));
    e.queue->addUploads("/Docs", {e.file("Downloads/late.txt", "late")});   // the user queues while it runs
    PB_CHECK(until([&] { return e.queue->openCount() == 0 && !e.queue->running(); }, 20000));
    wait_ms(400);
    PB_CHECK(finished.count() >= 1 && finished.count() <= 2);   // that run took it, or one follow-up did - never a loop
    const int sends = finished.count();
    wait_ms(300);
    PB_CHECK_EQ(finished.count(), sends);
    PB_CHECK(e.server.has("/Docs/late.txt"));
    PB_CHECK(e.server.has("/Docs/big.bin"));
}

PB_TEST(a_taken_name_comes_back_as_a_question_and_nothing_is_queued_until_it_is_answered) {
    Env e;
    e.server.add_file("/Docs/a.pdf", "on the server");
    e.cache("/Docs");
    const QVariantMap r = e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf", "aaaa"), e.file("Downloads/b.pdf", "bb")});
    PB_CHECK_EQ(r.value("queued").toInt(), 1);
    const QVariantList asks = r.value("asks").toList();
    PB_CHECK_EQ(asks.size(), 1);
    const QVariantMap ask = asks.at(0).toMap();
    PB_CHECK_EQ(ask.value("name").toString().toStdString(), std::string("a.pdf"));
    PB_CHECK_EQ(ask.value("suggested").toString().toStdString(), std::string("a (2).pdf"));
    PB_CHECK(ask.value("replacePossible").toBool());
    PB_CHECK_EQ(ask.value("existingSizeText").toString().toStdString(), std::string("13 B"));
    PB_CHECK_EQ(e.queue->openCount(), 1);
    // keep both
    const QVariantMap kept = e.queue->answerCollision(ask.value("askId").toString(), "keep_both", "");
    PB_CHECK_EQ(kept.value("queued").toInt(), 1);
    PB_CHECK_EQ(e.queue->openCount(), 2);
    // the same question cannot be answered twice
    PB_CHECK_EQ(e.queue->answerCollision(ask.value("askId").toString(), "keep_both", "").value("queued").toInt(), 0);
    PB_CHECK_EQ(e.queue->openCount(), 2);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(replace_and_skip_are_answers_and_a_taken_new_name_asks_again) {
    Env e;
    e.server.add_file("/Docs/a.pdf", "on the server");
    e.server.add_file("/Docs/mine.pdf", "x");
    e.cache("/Docs");
    QVariantList asks = e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf", "aaaa")}).value("asks").toList();
    const QVariantMap again = e.queue->answerCollision(asks.at(0).toMap().value("askId").toString(), "keep_both", "mine.pdf");
    PB_CHECK_EQ(again.value("queued").toInt(), 0);
    PB_CHECK_EQ(again.value("asks").toList().size(), 1);
    const QVariantMap bad = e.queue->answerCollision(again.value("asks").toList().at(0).toMap().value("askId").toString(), "keep_both", "a/b");
    PB_CHECK_EQ(bad.value("errorKey").toString().toStdString(), std::string("name_forbidden"));
    asks = e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf", "aaaa")}).value("asks").toList();
    const QVariantMap replaced = e.queue->answerCollision(asks.at(0).toMap().value("askId").toString(), "replace", "");
    PB_CHECK_EQ(replaced.value("queued").toInt(), 1);
    PB_CHECK_EQ(e.queue->items().at(0).toMap().value("kind").toString().toStdString(), std::string("upload"));
    asks = e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf", "aaaa")}).value("asks").toList();   // the entry is being replaced now
    PB_CHECK_EQ(e.queue->answerCollision(asks.at(0).toMap().value("askId").toString(), "skip", "").value("queued").toInt(), 0);
    PB_CHECK_EQ(e.queue->openCount(), 1);
}

PB_TEST(a_folder_pick_answers_every_taken_name_with_one_policy) {
    Env e;
    e.server.add_folder("/Docs/trip");
    e.server.add_file("/Docs/trip/plan.md", "old");
    e.cache("/Docs");
    e.cache("/Docs/trip");
    e.file("Notes/trip/plan.md", "plan");
    e.file("Notes/trip/new.md", "new");
    const QVariantMap pick = e.queue->addFolderPick("/Docs", {QString::fromStdString(e.root + "/Notes/trip")});
    const QVariantMap asked = e.queue->confirmFolderPick(pick.value("token").toString());
    PB_CHECK_EQ(asked.value("queued").toInt(), 1);
    PB_CHECK_EQ(asked.value("asks").toList().size(), 1);
    const QVariantMap pick2 = e.queue->addFolderPick("/Docs", {QString::fromStdString(e.root + "/Notes/trip")});
    const QVariantMap kept = e.queue->confirmFolderPick(pick2.value("token").toString(), "keep_both");
    PB_CHECK(kept.value("asks").toList().isEmpty());
    PB_CHECK_EQ(kept.value("queued").toInt(), 2);
}

PB_TEST(a_held_replace_is_resolved_from_the_bridge_with_a_second_look_at_the_server) {
    Env e;
    e.server.add_file("/Docs/a.pdf", "version one");
    e.cache("/Docs");
    const QVariantList asks = e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf", "my version")}).value("asks").toList();
    e.queue->answerCollision(asks.at(0).toMap().value("askId").toString(), "replace", "");
    const int id = e.queue->items().at(0).toMap().value("id").toInt();
    e.server.set_file("/Docs/a.pdf", "version two, by someone else", 1790000500);
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(state_of(e, id).toStdString(), std::string("conflict"));
    PB_CHECK_EQ(e.server.node("/Docs/a.pdf").content, std::string("version two, by someone else"));
    // the second confirmation shows what the server has now
    QSignalSpy prepared(e.queue.get(), &QueueBridge::resolvePrepared);
    e.queue->prepareResolve(id);
    PB_CHECK(prepared.wait(20000));
    const QVariantMap info = prepared.at(0).at(1).toMap();
    PB_CHECK(info.value("ok").toBool());
    PB_CHECK(info.value("exists").toBool());
    PB_CHECK_EQ(info.value("sizeText").toString().toStdString(), std::string("28 B"));
    PB_CHECK_EQ(state_of(e, id).toStdString(), std::string("conflict"));   // looking changes nothing
    QSignalSpy resolved(e.queue.get(), &QueueBridge::resolveFinished);
    e.queue->resolve(id, "replace_anyway", "");
    PB_CHECK(resolved.wait(20000));
    PB_CHECK(resolved.at(0).at(1).toString().isEmpty());
    PB_CHECK_EQ(state_of(e, id).toStdString(), std::string("queued"));
    finished.clear();
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(state_of(e, id).toStdString(), std::string("done"));
    PB_CHECK_EQ(e.server.node("/Docs/a.pdf").content, std::string("my version"));
}

PB_TEST(replace_anyway_needs_the_second_look_first_and_keep_both_and_skip_need_none) {
    Env e;
    e.server.add_file("/Docs/a.pdf", "version one");
    e.cache("/Docs");
    e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf", "my version")});   // not asked about: the listing... it is cached, so it asks
    const QVariantList asks = e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf", "my version")}).value("asks").toList();
    PB_CHECK_EQ(asks.size(), 1);
    e.queue->answerCollision(asks.at(0).toMap().value("askId").toString(), "replace", "");
    const int id = e.queue->items().at(0).toMap().value("id").toInt();
    e.server.set_file("/Docs/a.pdf", "changed", 1790000500);
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    QSignalSpy resolved(e.queue.get(), &QueueBridge::resolveFinished);
    e.queue->resolve(id, "replace_anyway", "");   // without prepareResolve
    PB_CHECK(resolved.wait(20000));
    PB_CHECK_EQ(resolved.at(0).at(1).toString().toStdString(), std::string("refresh_first"));
    PB_CHECK_EQ(state_of(e, id).toStdString(), std::string("conflict"));
    resolved.clear();
    e.queue->resolve(id, "skip", "");
    PB_CHECK(resolved.wait(20000));
    PB_CHECK_EQ(state_of(e, id).toStdString(), std::string("gone"));
}

PB_TEST(new_folder_and_rename_are_validated_queued_and_shown_at_once_without_a_request) {
    Env e;
    e.server.add_file("/Docs/a.pdf", "x");
    e.server.add_folder("/Docs/Old");
    e.cache("/Docs");
    const QVariantMap made = e.queue->newFolder("/Docs", "Trips");
    PB_CHECK_EQ(made.value("queued").toInt(), 1);
    PB_CHECK(!made.value("exists").toBool());
    const QVariantMap exists = e.queue->newFolder("/Docs", "Old");
    PB_CHECK_EQ(exists.value("queued").toInt(), 0);
    PB_CHECK(exists.value("exists").toBool());
    PB_CHECK_EQ(e.queue->newFolder("/Docs", "bad/name").value("errorKey").toString().toStdString(), std::string("name_forbidden"));
    const QVariantMap renamed = e.queue->rename("/Docs/a.pdf", "b.pdf");
    PB_CHECK_EQ(renamed.value("queued").toInt(), 1);
    PB_CHECK_EQ(e.queue->rename("/Docs/a.pdf", "c.pdf").value("errorKey").toString().toStdString(), std::string("change_pending"));   // one change at a time
    PB_CHECK_EQ(e.queue->rename("/Docs/Old", "Old").value("errorKey").toString().toStdString(), std::string("name_exists"));
    PB_CHECK_EQ(e.queue->rename("/Docs/missing.pdf", "m.pdf").value("errorKey").toString().toStdString(), std::string("refresh_first"));
    PB_CHECK_EQ(e.queue->openCount(), 2);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    const auto page = e.store->entries_page_overlay("/Docs", pbc::SortKey::Name, true, 0, 10);
    PB_CHECK_EQ(page.total, 3);
    // sent
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK(e.server.has("/Docs/Trips"));
    PB_CHECK(e.server.has("/Docs/b.pdf"));
    PB_CHECK(!e.server.has("/Docs/a.pdf"));
}

PB_TEST(move_and_copy_come_back_with_their_refusals_and_questions_and_are_queued_by_the_bridge) {
    Env e;
    e.server.add_folder("/Docs/src");
    e.server.add_folder("/Docs/dst");
    e.server.add_file("/Docs/src/a.txt", "mine");
    e.server.add_file("/Docs/src/b.txt", "mine");
    e.server.add_file("/Docs/dst/b.txt", "theirs");
    e.cache("/Docs/src");
    e.cache("/Docs/dst");
    const QVariantMap r = e.queue->moveOrCopy("move", {"/Docs/src/a.txt", "/Docs/src/b.txt", "/Docs/src/missing.txt"}, "/Docs/dst");
    PB_CHECK_EQ(r.value("queued").toInt(), 1);
    PB_CHECK_EQ(r.value("asks").toList().size(), 1);
    const QVariantList refused = r.value("refused").toList();
    PB_CHECK_EQ(refused.size(), 1);
    PB_CHECK_EQ(refused.at(0).toMap().value("key").toString().toStdString(), std::string("refresh_first"));
    PB_CHECK_EQ(refused.at(0).toMap().value("name").toString().toStdString(), std::string("missing.txt"));
    const QVariantMap kept = e.queue->answerCollision(r.value("asks").toList().at(0).toMap().value("askId").toString(), "keep_both", "");
    PB_CHECK_EQ(kept.value("queued").toInt(), 1);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK(e.server.has("/Docs/dst/a.txt"));
    PB_CHECK(e.server.has("/Docs/dst/b (2).txt"));
    PB_CHECK_EQ(e.server.node("/Docs/dst/b.txt").content, std::string("theirs"));
    const QVariantMap copied = e.queue->moveOrCopy("copy", {"/Docs/dst/a.txt"}, "/Docs/src");
    PB_CHECK_EQ(copied.value("queued").toInt(), 1);
}

PB_TEST(delete_asks_first_with_the_data_of_the_confirmation_and_queues_only_when_confirmed_and_never_without_a_trash) {
    Env e;
    e.server.add_folder("/Docs/old");
    e.server.add_file("/Docs/old/x.txt", "x");
    e.server.add_file("/Docs/a.pdf", "x");
    e.cache("/Docs");
    e.cache("/Docs/old");
    e.queue->addUploads("/Docs/old", {e.file("Downloads/n.txt", "nn")});   // an item inside the folder: it fails with the folder
    const QVariantMap ask = e.queue->deleteEntries({"/Docs/old", "/Docs/a.pdf"}, false);
    PB_CHECK_EQ(ask.value("entries").toInt(), 2);
    PB_CHECK_EQ(ask.value("folders").toInt(), 1);
    PB_CHECK_EQ(ask.value("itemsInside").toInt(), 1);
    PB_CHECK(ask.value("deleteOffered").toBool());
    PB_CHECK_EQ(e.queue->openCount(), 1);                       // asking queued nothing
    const QVariantMap done = e.queue->deleteEntries({"/Docs/a.pdf"}, true);
    PB_CHECK_EQ(done.value("queued").toInt(), 1);
    PB_CHECK_EQ(e.queue->openCount(), 2);
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK(e.server.trash_has("/Docs/a.pdf"));
    // a server without a trash: not offered, nothing queued
    pbc::ServerCaps none;
    none.trash = false;
    PB_CHECK(e.store->save_server_caps(none).ok());
    PB_CHECK(!e.queue->deleteEntries({"/Docs/old"}, false).value("deleteOffered").toBool());
    const QVariantMap refused = e.queue->deleteEntries({"/Docs/old"}, true);
    PB_CHECK_EQ(refused.value("queued").toInt(), 0);
    PB_CHECK_EQ(refused.value("errorKey").toString().toStdString(), std::string("trash_unavailable"));
}

PB_TEST(a_rename_onto_a_taken_name_comes_back_as_a_question_from_the_bridge) {
    Env e;
    e.server.add_file("/Docs/a.pdf", "x");
    e.server.add_file("/Docs/b.pdf", "y");
    e.cache("/Docs");
    const QVariantMap r = e.queue->rename("/Docs/a.pdf", "b.pdf");
    PB_CHECK_EQ(r.value("queued").toInt(), 0);
    PB_CHECK(r.value("errorKey").toString().isEmpty());
    const QVariantList asks = r.value("asks").toList();
    PB_CHECK_EQ(asks.size(), 1);
    PB_CHECK(asks.at(0).toMap().value("replacePossible").toBool());
    const QVariantMap kept = e.queue->answerCollision(asks.at(0).toMap().value("askId").toString(), "keep_both", "");
    PB_CHECK_EQ(kept.value("queued").toInt(), 1);
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK(e.server.has("/Docs/b (2).pdf"));
    PB_CHECK(!e.server.has("/Docs/a.pdf"));
}

PB_TEST(a_refused_login_keeps_the_queue_as_it_is_and_rows_of_another_account_are_never_sent) {
    Env e;
    e.queue->addUploads("/Docs", {e.file("Downloads/a.pdf", "aaaa")});
    pbc::QueueItem other;
    other.kind = pbc::ItemKind::Upload;
    other.server = e.server.base_url();
    other.user_id = "someone";
    other.src_local = e.root + "/Downloads/a.pdf";
    other.dst_parent = "/Docs";
    other.dst_name = "theirs.pdf";
    PB_CHECK(e.store->queue_add(other).ok());
    e.server.answer_next("PUT", "/Docs/", 401);
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(e.queue->waitingNotice().toStdString(), std::string("signin"));
    PB_CHECK_EQ(e.queue->openCount(), 2);                               // nothing was changed or dropped
    for (const QVariant& v : e.queue->items()) PB_CHECK_EQ(v.toMap().value("state").toString() == QLatin1String("failed"), false);
    finished.clear();
    e.queue->send();                                                    // the credentials work again: only this account's row goes
    PB_CHECK(finished.wait(20000));
    PB_CHECK(e.server.has("/Docs/a.pdf"));
    PB_CHECK(!e.server.has("/Docs/theirs.pdf"));
}

PB_TEST(an_upload_whose_file_changed_after_queuing_says_so_and_sends_the_file_as_it_is_then) {
    Env e;
    const QString f = e.file("Downloads/a.txt", "short");
    e.queue->addUploads("/Docs", {f});
    PB_CHECK(!e.queue->items().at(0).toMap().value("sourceChanged").toBool());
    e.file("Downloads/a.txt", "a much longer content than before");   // the user edited it
    e.queue->refresh();
    PB_CHECK(e.queue->items().at(0).toMap().value("sourceChanged").toBool());
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(e.server.node("/Docs/a.txt").content.size(), size_t(33));
    for (const QVariant& v : e.queue->items()) PB_CHECK(!v.toMap().value("sourceChanged").toBool());   // done rows say nothing
}

PB_TEST(a_failed_upload_whose_folder_is_gone_is_given_another_target_from_the_bridge) {
    Env e;
    e.server.add_folder("/Docs/Other");
    e.queue->addUploads("/Gone", {e.file("Downloads/a.txt", "aaa")});
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    const int id = e.queue->items().at(0).toMap().value("id").toInt();
    PB_CHECK_EQ(state_of(e, id).toStdString(), std::string("failed"));
    PB_CHECK(e.queue->items().at(0).toMap().value("canResolve").toBool());   // Edit offers a folder
    QSignalSpy resolved(e.queue.get(), &QueueBridge::resolveFinished);
    e.queue->resolve(id, "retarget", "/Docs/Other");
    PB_CHECK(resolved.wait(20000));
    PB_CHECK(resolved.at(0).at(1).toString().isEmpty());
    finished.clear();
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK(e.server.has("/Docs/Other/a.txt"));
}

PB_TEST(every_queuing_action_of_the_user_tells_the_sync_which_folder_and_nothing_else_does) {
    Env e;
    e.server.add_file("/Docs/a.pdf", "x");
    e.server.add_folder("/Docs/Old");
    e.cache("/Docs");
    // reading, listing, a refused pick and an unknown token tell nothing
    e.queue->items();
    e.queue->refresh();
    e.queue->addUploads("/Docs", {QStringLiteral("/nowhere/at/all.pdf")});
    PB_CHECK(e.actions.empty());
    e.queue->addUploads("/Docs", {e.file("Downloads/a2.pdf", "aaaa")});
    PB_CHECK_EQ(e.actions.size(), size_t(1));
    PB_CHECK_EQ(e.actions.back(), std::string("/Docs"));
    e.queue->newFolder("/Docs", "Trips");
    PB_CHECK_EQ(e.actions.size(), size_t(2));
    PB_CHECK_EQ(e.actions.back(), std::string("/Docs"));
    e.queue->newFolder("/Docs", "Old");   // exists: nothing queued
    e.queue->newFolder("/Docs", "bad/name");
    PB_CHECK_EQ(e.actions.size(), size_t(2));
    e.queue->rename("/Docs/a.pdf", "b.pdf");
    PB_CHECK_EQ(e.actions.size(), size_t(3));
    PB_CHECK_EQ(e.actions.back(), std::string("/Docs"));
    e.queue->deleteEntries({"/Docs/Old"}, false);   // only asking
    PB_CHECK_EQ(e.actions.size(), size_t(3));
    e.queue->deleteEntries({"/Docs/Old"}, true);
    PB_CHECK_EQ(e.actions.size(), size_t(4));
    // a send that ends starts nothing and tells nothing
    QSignalSpy finished(e.queue.get(), &QueueBridge::sendFinished);
    e.queue->send();
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(e.actions.size(), size_t(4));
}

PB_TEST_MAIN()
