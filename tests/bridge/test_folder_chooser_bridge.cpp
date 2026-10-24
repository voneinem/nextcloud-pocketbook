// The folder chooser of Move and Copy (specs/002-manage-server-files US5): folders only, from the stored listings; a never-fetched folder is fetched when
// the user opens it and the reader is online; offline nothing is fetched; a folder that is still queued is a target too. Real core, fake Nextcloud.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include "fake_nextcloud_server.h"
#include "folder_chooser_bridge.h"
#include "pbc/credentials_file.h"
#include "pbc/folder_service.h"
#include "pbc/queue_actions.h"
#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_folder_chooser_bridge";
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
    std::unique_ptr<FolderChooserBridge> chooser;

    Env() {
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
        server.add_folder("/Docs/A");
        server.add_folder("/Docs/B");
        server.add_file("/Docs/readme.txt", "x");
        server.add_folder("/Docs/A/Deep");
        FolderChooserBridge::Config c;
        c.store = store.get();
        c.worker = &worker;
        c.http = &server;
        c.clock = &clock;
        c.wifi_up = [this] { return wifi; };
        chooser = std::make_unique<FolderChooserBridge>(c);
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
};

QStringList names(const FolderChooserBridge& c) {
    QStringList out;
    for (const QVariant& v : c.rows()) out << v.toMap().value("name").toString();
    return out;
}

}  // namespace

PB_TEST(a_stored_listing_shows_its_folders_only_and_makes_no_request) {
    Env e;
    e.cache("/Docs");
    e.chooser->open("/Docs");
    PB_CHECK(e.chooser->hasListing());
    PB_CHECK_EQ(names(*e.chooser).join(",").toStdString(), std::string("A,B"));   // the file is not a place to go to
    PB_CHECK_EQ(e.chooser->path().toStdString(), std::string("/Docs"));
    PB_CHECK(e.chooser->canGoUp());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_never_fetched_folder_is_fetched_once_when_it_is_opened_online) {
    Env e;
    QSignalSpy changed(e.chooser.get(), &FolderChooserBridge::changed);
    e.chooser->open("/Docs");
    PB_CHECK(e.chooser->busy());
    QElapsedTimer t;
    t.start();
    while (e.chooser->busy() && t.elapsed() < 10000) QTest::qWait(5);
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(1));
    PB_CHECK(e.chooser->hasListing());
    PB_CHECK_EQ(names(*e.chooser).join(",").toStdString(), std::string("A,B"));
    // opening it again uses the stored listing
    e.server.clear_log();
    e.chooser->open("/Docs");
    QTest::qWait(100);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(offline_a_never_fetched_folder_shows_nothing_and_sends_nothing) {
    Env e;
    e.wifi = false;
    e.chooser->open("/Docs");
    QTest::qWait(150);
    PB_CHECK(e.chooser->offline());
    PB_CHECK(!e.chooser->hasListing());
    PB_CHECK(e.chooser->rows().isEmpty());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(up_goes_to_the_parent_and_the_path_segments_name_the_way) {
    Env e;
    e.cache("/Docs");
    e.cache("/Docs/A");
    e.chooser->open("/Docs/A");
    PB_CHECK_EQ(names(*e.chooser).join(",").toStdString(), std::string("Deep"));
    const QVariantList segs = e.chooser->pathSegments();
    PB_CHECK_EQ(segs.size(), 3);
    PB_CHECK_EQ(segs.at(2).toMap().value("label").toString().toStdString(), std::string("A"));
    e.chooser->up();
    PB_CHECK_EQ(e.chooser->path().toStdString(), std::string("/Docs"));
    e.chooser->open("");
    PB_CHECK(!e.chooser->canGoUp());
}

PB_TEST(a_folder_that_is_still_queued_is_a_place_to_go_to_and_one_that_is_being_moved_away_is_not) {
    Env e;
    pbc::ServerCaps caps;
    PB_CHECK(e.store->save_server_caps(caps).ok());
    e.cache("/Docs");
    pbc::QueueActions act(*e.store, pbc::SourceRules());
    PB_CHECK(act.new_folder("/Docs", "Fresh").error.ok());
    PB_CHECK(act.move_or_copy(pbc::ItemKind::Move, {"/Docs/B"}, "/Docs/A").queued.size() == 1);
    e.chooser->open("/Docs");
    const QStringList n = names(*e.chooser);
    PB_CHECK(n.contains("Fresh"));
    PB_CHECK(n.contains("A"));
    PB_CHECK(!n.contains("B"));
}

PB_TEST(folders_are_paged) {
    Env e;
    for (int i = 0; i < 25; ++i) e.server.add_folder("/Docs/F" + std::string(i < 10 ? "0" : "") + std::to_string(i));
    e.cache("/Docs");
    e.chooser->setPageSize(10);
    e.chooser->open("/Docs");
    PB_CHECK_EQ(e.chooser->pageCount(), 3);
    PB_CHECK_EQ(e.chooser->rows().size(), 10);
    e.chooser->nextPage();
    PB_CHECK_EQ(e.chooser->page(), 1);
    e.chooser->nextPage();
    e.chooser->nextPage();   // no further
    PB_CHECK_EQ(e.chooser->page(), 2);
    e.chooser->previousPage();
    PB_CHECK_EQ(e.chooser->page(), 1);
}

PB_TEST(a_chooser_that_serves_the_server_only_lists_no_folder_that_is_merely_queued) {
    Env e;
    pbc::ServerCaps caps;
    PB_CHECK(e.store->save_server_caps(caps).ok());
    e.cache("/Docs");
    pbc::QueueActions act(*e.store, pbc::SourceRules());
    PB_CHECK(act.new_folder("/Docs", "Fresh").error.ok());   // queued, not yet on the server
    e.chooser->setServerOnly(true);
    PB_CHECK(e.chooser->serverOnly());
    e.chooser->open("/Docs");
    QStringList n = names(*e.chooser);
    PB_CHECK(!n.contains("Fresh"));   // a restore cannot go to a folder the server does not have
    PB_CHECK(n.contains("A") && n.contains("B"));
    e.chooser->setServerOnly(false);   // the other callers (Move, Copy) still see it
    n = names(*e.chooser);
    PB_CHECK(n.contains("Fresh"));
}

PB_TEST_MAIN()
