// The BrowseBridge (contracts/bridge-qml.md): a folder shown page by page from the cache, sorted, with explicit refresh. Real core, fake
// Nextcloud. Turning pages, changing the sort and going up are local; only opening a folder never seen and Refresh use the network.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include <fstream>

#include <sys/stat.h>

#include "pbc/sync_rules.h"

#include "browse_bridge.h"
#include "download_env.h"
#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/store.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_browse_bridge";
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
    int network_taps = 0;
    std::unique_ptr<BrowseBridge> browse;

    Env() {
        server.add_folder("/Books");
        server.add_folder("/Books/Novels");
        for (int i = 0; i < 6; ++i) server.add_file("/Books/book" + std::to_string(i) + ".epub", std::string(100 * (i + 1), 'x'), 1790000000 + i * 1000);
        server.add_file("/readme.txt", "hi", 1790000100);
        server.add_folder("/Vault");
        server.set_encrypted("/Vault", true);
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
        BrowseBridge::Config cfg;
        cfg.store = store.get();
        cfg.worker = &worker;
        cfg.http = &server;
        cfg.clock = &clock;
        cfg.ensure_network = [this](std::function<void(bool)> done) {
            ++network_taps;
            done(wifi);
        };
        cfg.format_date = [](qint64 t) { return QStringLiteral("d%1").arg(t); };
        cfg.download_root = dir.file("PBCloud");
        browse = std::make_unique<BrowseBridge>(cfg);
        server.clear_log();
    }
    ~Env() {
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    void settle() {
        QElapsedTimer t;
        t.start();
        while (browse->busy() && t.elapsed() < 10000) QTest::qWait(5);
        if (browse->busy()) PB_FAIL_AT("the bridge stayed busy");
        QCoreApplication::processEvents();
    }
    QStringList names() {
        QStringList n;
        for (const QVariant& v : browse->rows()) n << v.toMap().value("name").toString();
        return n;
    }
};

}  // namespace

PB_TEST(opening_the_root_fetches_it_once_and_shows_folders_first_with_their_details) {
    Env e;
    e.browse->open("/");
    e.settle();
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(1));
    PB_CHECK_EQ(e.browse->path().toStdString(), std::string("/"));
    PB_CHECK(e.browse->hasListing());
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("Books,Vault,readme.txt"));
    const QVariantMap file = e.browse->rows().at(2).toMap();
    PB_CHECK_EQ(file.value("kind").toString().toStdString(), std::string("file"));
    PB_CHECK_EQ(file.value("sizeText").toString().toStdString(), std::string("2 B"));
    PB_CHECK_EQ(file.value("dateText").toString().toStdString(), std::string("d1790000100"));
    PB_CHECK_EQ(file.value("remotePath").toString().toStdString(), std::string("/readme.txt"));
    PB_CHECK_EQ(e.browse->listingAgeKind().toStdString(), std::string("just_now"));
    PB_CHECK_EQ(e.browse->listingState().toStdString(), std::string("fresh"));
    PB_CHECK(e.browse->errorKey().isEmpty());
}

PB_TEST(rows_hold_one_page_and_turning_pages_is_local) {
    Env e;
    e.browse->setPageSize(3);
    e.browse->open("/Books");
    e.settle();
    PB_CHECK_EQ(e.browse->pageCount(), 3);   // 1 folder + 6 files = 7 entries in pages of 3
    PB_CHECK_EQ(e.browse->page(), 0);
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("Novels,book0.epub,book1.epub"));
    e.server.clear_log();
    e.browse->nextPage();
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("book2.epub,book3.epub,book4.epub"));
    e.browse->nextPage();
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("book5.epub"));
    e.browse->nextPage();   // there is no fourth page
    PB_CHECK_EQ(e.browse->page(), 2);
    e.browse->previousPage();
    e.browse->goToPage(0);
    PB_CHECK_EQ(e.browse->page(), 0);
    e.browse->goToPage(99);
    PB_CHECK_EQ(e.browse->page(), 2);
    e.browse->goToPage(-5);
    PB_CHECK_EQ(e.browse->page(), 0);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.browse->setPageSize(100);
    PB_CHECK_EQ(e.browse->pageCount(), 1);
    PB_CHECK_EQ(e.browse->rows().size(), 7);
}

PB_TEST(the_sort_is_applied_kept_for_the_next_folder_and_needs_no_request) {
    Env e;
    e.browse->open("/Books");
    e.settle();
    e.server.clear_log();
    e.browse->setSort("size", false);
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("Novels,book5.epub,book4.epub,book3.epub,book2.epub,book1.epub,book0.epub"));   // folders stay first
    PB_CHECK_EQ(e.store->setting("sort_key"), std::string("size"));
    PB_CHECK_EQ(e.store->setting("sort_dir"), std::string("desc"));
    PB_CHECK_EQ(e.browse->sortKey().toStdString(), std::string("size"));
    PB_CHECK(!e.browse->sortAscending());
    e.browse->setSort("name", true);
    PB_CHECK_EQ(e.names().first().toStdString(), std::string("Novels"));
    e.browse->setSort("bogus", true);   // a value that is not offered changes nothing
    PB_CHECK_EQ(e.browse->sortKey().toStdString(), std::string("name"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(the_path_line_and_up_work_from_the_cache) {
    Env e;
    e.browse->open("/");
    e.settle();
    e.browse->open("/Books");
    e.settle();
    e.browse->open("/Books/Novels");
    e.settle();
    const QVariantList segs = e.browse->pathSegments();
    PB_CHECK_EQ(segs.size(), 3);
    PB_CHECK_EQ(segs.at(0).toMap().value("path").toString().toStdString(), std::string("/"));
    PB_CHECK_EQ(segs.at(1).toMap().value("label").toString().toStdString(), std::string("Books"));
    PB_CHECK_EQ(segs.at(2).toMap().value("path").toString().toStdString(), std::string("/Books/Novels"));
    e.server.clear_log();
    e.browse->up();
    PB_CHECK_EQ(e.browse->path().toStdString(), std::string("/Books"));
    e.browse->up();
    PB_CHECK_EQ(e.browse->path().toStdString(), std::string("/"));
    e.browse->up();   // the root has no parent
    PB_CHECK_EQ(e.browse->path().toStdString(), std::string("/"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(!e.browse->canGoUp());
}

PB_TEST(a_visited_folder_opens_offline_with_its_age_and_an_unvisited_one_says_it_is_not_available) {
    Env e;
    e.browse->open("/Books");
    e.settle();
    e.clock.t += 2 * 3600 + 5;
    e.wifi = false;
    e.server.set_offline(true);
    e.server.clear_log();
    e.browse->open("/Books");
    e.settle();
    PB_CHECK(e.browse->hasListing());
    PB_CHECK_EQ(e.browse->listingAgeKind().toStdString(), std::string("hours"));
    PB_CHECK_EQ(e.browse->listingAgeCount(), 2);
    PB_CHECK_EQ(e.browse->listingState().toStdString(), std::string("cached"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.browse->errorKey().isEmpty());

    e.browse->open("/Books/Novels");   // never visited
    e.settle();
    PB_CHECK(!e.browse->hasListing());
    PB_CHECK_EQ(e.browse->errorKey().toStdString(), std::string("no_network"));
    PB_CHECK(e.browse->offline());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(refresh_is_the_only_call_that_fetches_a_folder_that_is_shown) {
    Env e;
    e.browse->open("/Books");
    e.settle();
    e.server.add_file("/Books/new.epub", "n", 1790005000);
    e.server.clear_log();
    e.browse->open("/Books");   // opening again does not refresh
    e.settle();
    e.browse->nextPage();
    e.browse->setSort("date", true);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(!e.names().contains("new.epub"));
    e.clock.t += 600;
    e.browse->refresh();
    e.settle();
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(1));
    PB_CHECK(e.names().contains("new.epub"));
    PB_CHECK_EQ(e.browse->listingAgeKind().toStdString(), std::string("just_now"));
}

PB_TEST(a_failed_refresh_keeps_the_rows_and_reports_the_reason) {
    Env e;
    e.browse->open("/Books");
    e.settle();
    const QStringList before = e.names();
    e.server.fail_next(503);
    e.browse->refresh();
    e.settle();
    PB_CHECK_EQ(e.browse->errorKey().toStdString(), std::string("maintenance"));
    PB_CHECK(e.names() == before);
    PB_CHECK(e.browse->hasListing());
    e.server.remove("/Books");
    e.browse->refresh();
    e.settle();
    PB_CHECK_EQ(e.browse->listingState().toStdString(), std::string("gone"));
    PB_CHECK(e.names() == before);   // still readable
}

PB_TEST(a_file_opens_its_details_and_an_unavailable_entry_only_says_why) {
    Env e;
    e.browse->open("/");
    e.settle();
    QSignalSpy details(e.browse.get(), &BrowseBridge::showDetails);
    QSignalSpy reason(e.browse.get(), &BrowseBridge::showReason);
    e.browse->open("/readme.txt");
    PB_CHECK_EQ(details.count(), 1);
    PB_CHECK_EQ(details.at(0).at(0).toString().toStdString(), std::string("/readme.txt"));
    PB_CHECK_EQ(e.browse->path().toStdString(), std::string("/"));
    e.browse->open("/Vault");
    PB_CHECK_EQ(reason.count(), 1);
    PB_CHECK_EQ(reason.at(0).at(0).toString().toStdString(), std::string("end_to_end_encrypted"));
    PB_CHECK_EQ(e.browse->path().toStdString(), std::string("/"));   // it did not navigate
    PB_CHECK_EQ(details.count(), 1);
}

PB_TEST(names_from_the_server_are_made_safe_for_the_screen) {
    Env e;
    e.server.add_file("/Books/evil\xE2\x80\xAE" "gpj.exe", "x", 1790000000);   // a right-to-left override
    e.server.add_file("/Books/" + std::string(300, 'n'), "x", 1790000000);
    e.browse->setPageSize(100);
    e.browse->open("/Books");
    e.settle();
    bool saw_evil = false, saw_long = false;
    for (const QVariant& v : e.browse->rows()) {
        const QString n = v.toMap().value("name").toString();
        PB_CHECK(!n.contains(QChar(0x202e)));
        if (n.startsWith("evil")) saw_evil = (n == "evilgpj.exe");
        if (n.startsWith("nnnn")) saw_long = n.size() <= 203;
    }
    PB_CHECK(saw_evil);
    PB_CHECK(saw_long);
}

PB_TEST(each_row_carries_the_state_of_its_copy_and_whether_it_is_kept_offline) {
    Env e;
    e.browse->setPageSize(100);
    e.browse->open("/Books");
    e.settle();
    auto row = [&](const char* name) -> QVariantMap {
        for (const QVariant& v : e.browse->rows())
            if (v.toMap().value("name").toString() == name) return v.toMap();
        return QVariantMap();
    };
    PB_CHECK_EQ(row("book0.epub").value("state").toString().toStdString(), std::string("not_downloaded"));
    PB_CHECK(!row("book0.epub").value("kept").toBool());

    // a downloaded copy
    pbtest::DlClock c2;
    c2.t = e.clock.t;
    pbtest::FakeStorage storage;
    pbc::Downloader dl(*e.store, e.server, storage, c2, e.dir.file("PBCloud"), [] { return true; }, pbc::DownloaderOptions());
    ::mkdir(e.dir.file("PBCloud").c_str(), 0700);
    pbc::DownloadRequest req;
    req.remote_path = "/Books/book0.epub";
    req.size = static_cast<int64_t>(100);
    pbtest::NoCancel nc;
    PB_CHECK(dl.run(req, nc, [](const pbc::JobProgress&) {}).state == pbc::JobState::Done);
    e.browse->refresh();
    e.settle();
    PB_CHECK_EQ(row("book0.epub").value("state").toString().toStdString(), std::string("downloaded"));

    // the server has a newer version (seen after a refresh) -> outdated
    e.server.set_file("/Books/book0.epub", std::string(150, 'n'), 1790009000);
    e.browse->refresh();
    e.settle();
    PB_CHECK_EQ(row("book0.epub").value("state").toString().toStdString(), std::string("outdated"));

    // the user edited the copy -> conflict with the newer server version
    {
        std::ofstream f(e.dir.file("PBCloud") + "/Books/book0.epub", std::ios::trunc);
        f << "edited";
    }
    e.browse->setSort("name", true);   // a reload of the page
    PB_CHECK_EQ(row("book0.epub").value("state").toString().toStdString(), std::string("conflict"));

    // kept offline: the mark of the folder covers its files
    PB_CHECK(e.store->set_mark("/Books", pbc::Kind::Folder, true).ok());
    e.browse->setSort("name", false);
    PB_CHECK(row("book1.epub").value("kept").toBool());
    PB_CHECK(row("Novels").value("kept").toBool());
    PB_CHECK(row("Novels").value("state").toString().isEmpty());   // folders have no state
}

PB_TEST(favorites_are_fetched_once_when_never_fetched_shown_from_the_store_afterwards_and_refreshed_only_on_request) {
    Env e;
    e.server.set_favorite("/Books/book2.epub", true);
    e.server.set_favorite("/readme.txt", true);
    e.browse->setPageSize(100);
    e.wifi = false;
    e.browse->showFavorites();   // never fetched and offline: nothing to show, nothing sent
    e.settle();
    PB_CHECK_EQ(e.browse->mode().toStdString(), std::string("favorites"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.browse->rows().isEmpty());
    PB_CHECK(e.browse->offline());
    PB_CHECK(!e.browse->hasListing());

    e.wifi = true;
    e.browse->showFavorites();   // online and never fetched: one request
    e.settle();
    PB_CHECK_EQ(e.server.count("REPORT"), size_t(1));
    PB_CHECK_EQ(e.server.request_count(), size_t(1));
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("book2.epub,readme.txt"));
    const QVariantMap first = e.browse->rows().at(0).toMap();
    PB_CHECK_EQ(first.value("folderPath").toString().toStdString(), std::string("/Books"));
    PB_CHECK_EQ(first.value("remotePath").toString().toStdString(), std::string("/Books/book2.epub"));
    PB_CHECK(first.value("favorite").toBool());
    PB_CHECK(e.browse->hasListing());
    PB_CHECK_EQ(e.browse->listingAgeKind().toStdString(), std::string("just_now"));

    e.server.clear_log();
    e.browse->showFavorites();   // known now: no request
    e.browse->setSort("size", false);
    e.settle();
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("book2.epub,readme.txt"));   // book2 is 300 B, readme 2 B

    e.server.set_favorite("/readme.txt", false);
    e.browse->refresh();   // explicit
    e.settle();
    PB_CHECK_EQ(e.server.count("REPORT"), size_t(1));
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("book2.epub"));
}

PB_TEST(a_favorite_the_user_may_not_read_is_shown_as_unavailable_and_says_why) {
    Env e;
    e.server.set_favorite("/readme.txt", true);
    e.server.set_permissions("/readme.txt", "RDNVCK");   // no read permission
    e.browse->showFavorites();
    e.settle();
    const QVariantMap row = e.browse->rows().at(0).toMap();
    PB_CHECK_EQ(row.value("availability").toString().toStdString(), std::string("no_permission"));
    PB_CHECK_EQ(row.value("reason").toString().toStdString(), std::string("no_read_permission"));
    QSignalSpy reason(e.browse.get(), &BrowseBridge::showReason);
    QSignalSpy details(e.browse.get(), &BrowseBridge::showDetails);
    e.browse->open("/readme.txt");   // a tap says why and opens nothing
    PB_CHECK_EQ(reason.count(), 1);
    PB_CHECK_EQ(details.count(), 0);
}

PB_TEST(favorites_stay_readable_offline_and_a_failed_refresh_keeps_them) {
    Env e;
    e.server.set_favorite("/readme.txt", true);
    e.browse->showFavorites();
    e.settle();
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("readme.txt"));
    e.server.set_offline(true);
    e.browse->refresh();
    e.settle();
    PB_CHECK(!e.browse->errorKey().isEmpty());
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("readme.txt"));
    PB_CHECK(e.browse->offline());
}

PB_TEST(the_offline_search_is_local_and_says_where_each_result_comes_from) {
    Env e;
    e.browse->setPageSize(100);
    e.browse->open("/Books");
    e.settle();
    e.server.clear_log();
    const int taps = e.network_taps;
    e.browse->searchOffline("book");
    PB_CHECK_EQ(e.browse->mode().toStdString(), std::string("search"));
    PB_CHECK_EQ(e.names().size(), 6);
    PB_CHECK_EQ(e.browse->rows().at(0).toMap().value("source").toString().toStdString(), std::string("reader"));
    PB_CHECK_EQ(e.browse->rows().at(0).toMap().value("folderPath").toString().toStdString(), std::string("/Books"));
    PB_CHECK(!e.browse->moreResults());
    e.browse->searchOffline("zzz");
    PB_CHECK(e.names().isEmpty());
    e.browse->searchOffline("");
    PB_CHECK(e.names().isEmpty());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK_EQ(e.network_taps, taps);   // not even the Wi-Fi was asked
}

PB_TEST(the_server_search_runs_on_the_explicit_tap_only_and_results_say_server) {
    Env e;
    e.server.add_file("/Far/Away/moby.epub", "x", 1790009000);
    e.browse->setPageSize(100);
    e.browse->searchOffline("moby");
    PB_CHECK(e.names().isEmpty());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.wifi = false;
    e.browse->searchServer("moby");
    e.settle();
    PB_CHECK_EQ(e.browse->errorKey().toStdString(), std::string("no_network"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.wifi = true;
    e.browse->searchServer("moby");
    e.settle();
    PB_CHECK_EQ(e.server.count("SEARCH"), size_t(1));
    PB_CHECK(e.browse->errorKey().isEmpty());
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("moby.epub"));
    const QVariantMap row = e.browse->rows().at(0).toMap();
    PB_CHECK_EQ(row.value("source").toString().toStdString(), std::string("server"));
    PB_CHECK_EQ(row.value("folderPath").toString().toStdString(), std::string("/Far/Away"));
    PB_CHECK(!e.store->folder("/Far/Away").has_value());   // nothing was cached
    QSignalSpy details(e.browse.get(), &BrowseBridge::showDetails);
    e.browse->open("/Far/Away/moby.epub");   // a tap on a result
    PB_CHECK_EQ(details.count(), 1);
}

PB_TEST(more_than_100_server_results_set_the_flag_and_show_100) {
    Env e;
    for (int i = 0; i < 120; ++i) e.server.add_file("/Many/needle" + std::to_string(i) + ".txt", "x", 1790001000 + i);
    e.browse->setPageSize(100);
    e.browse->searchServer("needle");
    e.settle();
    PB_CHECK(e.browse->moreResults());
    PB_CHECK_EQ(e.browse->rows().size(), 100);
    e.browse->searchOffline("book");   // another search clears the flag
    PB_CHECK(!e.browse->moreResults());
}

PB_TEST(opening_a_folder_result_goes_back_to_the_folder_mode) {
    Env e;
    e.browse->searchServer("novels");
    e.settle();
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("Novels"));
    e.browse->open("/Books/Novels");
    e.settle();
    PB_CHECK_EQ(e.browse->mode().toStdString(), std::string("folder"));
    PB_CHECK_EQ(e.browse->path().toStdString(), std::string("/Books/Novels"));
    e.browse->showFavorites();
    e.settle();
    e.browse->showFolder();
    PB_CHECK_EQ(e.browse->mode().toStdString(), std::string("folder"));
    PB_CHECK_EQ(e.browse->path().toStdString(), std::string("/Books/Novels"));
}

// ---- the queue in the listing and the select mode (specs/002-manage-server-files, FR-008) ----

namespace {
pbc::QueueItem upload_item(const std::string& server, const std::string& parent, const std::string& name, int64_t size = 5) {
    pbc::QueueItem q;
    q.kind = pbc::ItemKind::Upload;
    q.server = server;
    q.user_id = "demo";
    q.src_local = "/mnt/ext1/Downloads/" + name;
    q.src_size = size;
    q.dst_parent = parent;
    q.dst_name = name;
    return q;
}
QVariantMap row_named(BrowseBridge& b, const QString& name) {
    for (const QVariant& v : b.rows())
        if (v.toMap().value("name").toString() == name) return v.toMap();
    return QVariantMap();
}
}  // namespace

PB_TEST(a_queued_upload_is_a_pending_row_with_the_sync_state_and_no_request_is_made) {
    Env e;
    e.browse->open("/Books");
    e.settle();
    e.server.clear_log();
    PB_CHECK(e.store->queue_add(upload_item(e.server.base_url(), "/Books", "new.pdf", 2048)).ok());
    e.browse->refreshRows();
    const QVariantMap m = row_named(*e.browse, "new.pdf");
    PB_CHECK(!m.isEmpty());
    PB_CHECK_EQ(m.value("pending").toString().toStdString(), std::string("new"));
    PB_CHECK_EQ(m.value("syncState").toString().toStdString(), std::string("queued"));
    PB_CHECK(m.value("itemId").toInt() > 0);
    PB_CHECK(m.value("pendingRow").toBool());
    PB_CHECK_EQ(m.value("sizeText").toString().toStdString(), std::string("2.0 KB"));
    PB_CHECK_EQ(m.value("state").toString().toStdString(), std::string());   // no download state for something not on the server
    PB_CHECK(!m.value("kept").toBool());
    PB_CHECK_EQ(m.value("remotePath").toString().toStdString(), std::string("/Books/new.pdf"));
    // a real entry keeps its own empty pending fields
    const QVariantMap real = row_named(*e.browse, "book0.epub");
    PB_CHECK_EQ(real.value("pending").toString().toStdString(), std::string());
    PB_CHECK(!real.value("pendingRow").toBool());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_pending_row_opens_its_queue_item_and_not_a_listing_or_the_details) {
    Env e;
    e.browse->open("/Books");
    e.settle();
    const auto id = e.store->queue_add(upload_item(e.server.base_url(), "/Books", "new.pdf"));
    PB_CHECK(id.ok());
    e.server.clear_log();
    QSignalSpy queue(e.browse.get(), &BrowseBridge::showQueueItem);
    QSignalSpy details(e.browse.get(), &BrowseBridge::showDetails);
    e.browse->open("/Books/new.pdf");
    PB_CHECK_EQ(queue.count(), 1);
    PB_CHECK_EQ(queue.first().at(0).toInt(), static_cast<int>(id.value));
    PB_CHECK_EQ(details.count(), 0);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_folder_that_is_only_queued_opens_without_a_request_and_shows_what_is_queued_for_it) {
    Env e;
    e.browse->open("/Books");
    e.settle();
    pbc::QueueItem mk;
    mk.kind = pbc::ItemKind::Mkdir;
    mk.entry_kind = pbc::Kind::Folder;
    mk.server = e.server.base_url();
    mk.user_id = "demo";
    mk.dst_parent = "/Books";
    mk.dst_name = "Fresh";
    PB_CHECK(e.store->queue_add(mk).ok());
    PB_CHECK(e.store->queue_add(upload_item(e.server.base_url(), "/Books/Fresh", "inside.txt")).ok());
    e.browse->refreshRows();
    PB_CHECK_EQ(row_named(*e.browse, "Fresh").value("pending").toString().toStdString(), std::string("new"));
    e.server.clear_log();
    e.browse->open("/Books/Fresh");
    e.settle();
    PB_CHECK_EQ(e.server.request_count(), size_t(0));           // it does not exist on the server yet: nothing to ask
    PB_CHECK_EQ(e.browse->path().toStdString(), std::string("/Books/Fresh"));
    PB_CHECK(e.browse->hasListing());
    PB_CHECK_EQ(e.browse->listingState().toStdString(), std::string("not_synced"));
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("inside.txt"));
    PB_CHECK_EQ(row_named(*e.browse, "inside.txt").value("pending").toString().toStdString(), std::string("new"));
}

PB_TEST(rows_keep_their_sort_and_paging_with_pending_rows_among_them) {
    Env e;
    e.browse->open("/Books");
    e.settle();
    for (int i = 0; i < 3; ++i) PB_CHECK(e.store->queue_add(upload_item(e.server.base_url(), "/Books", "p" + std::to_string(i) + ".txt")).ok());
    e.browse->setPageSize(4);
    PB_CHECK_EQ(e.browse->pageCount(), 3);                      // Novels, six books and three pending files: ten rows in pages of four
    e.browse->nextPage();
    e.browse->nextPage();
    PB_CHECK_EQ(e.browse->rows().size(), 2);
    e.browse->setSort("name", false);
    PB_CHECK_EQ(e.browse->page(), 0);
    PB_CHECK_EQ(e.names().first().toStdString(), std::string("Novels"));   // folders first, whatever the direction
}

PB_TEST(select_mode_toggles_rows_by_path_and_clears_with_the_folder) {
    Env e;
    e.browse->open("/Books");
    e.settle();
    PB_CHECK(!e.browse->selecting());
    e.browse->setSelecting(true);
    PB_CHECK(e.browse->selecting());
    e.browse->toggleSelect("/Books/book0.epub");
    e.browse->toggleSelect("/Books/book1.epub");
    PB_CHECK_EQ(e.browse->selectedCount(), 2);
    PB_CHECK(row_named(*e.browse, "book0.epub").value("selected").toBool());
    PB_CHECK(!row_named(*e.browse, "book2.epub").value("selected").toBool());
    PB_CHECK_EQ(e.browse->selectedPaths().join(",").toStdString(), std::string("/Books/book0.epub,/Books/book1.epub"));
    e.browse->toggleSelect("/Books/book0.epub");
    PB_CHECK_EQ(e.browse->selectedCount(), 1);
    e.browse->selectAllOnPage();
    PB_CHECK_EQ(e.browse->selectedCount(), e.browse->rows().size());   // every selectable row of the shown page
    e.browse->clearSelection();
    PB_CHECK_EQ(e.browse->selectedCount(), 0);
    e.browse->toggleSelect("/Books/book2.epub");
    e.browse->open("/Books/Novels");   // going to another folder leaves the select mode and drops the selection
    e.settle();
    PB_CHECK(!e.browse->selecting());
    PB_CHECK_EQ(e.browse->selectedCount(), 0);
}

PB_TEST(an_entry_with_a_queued_change_is_not_selectable_for_another_one) {
    Env e;
    e.browse->open("/Books");
    e.settle();
    pbc::QueueItem del;
    del.kind = pbc::ItemKind::Delete;
    del.server = e.server.base_url();
    del.user_id = "demo";
    del.src_remote = "/Books/book3.epub";
    del.src_etag = e.server.etag_of("/Books/book3.epub");
    PB_CHECK(e.store->queue_add(del).ok());
    e.browse->setSelecting(true);
    e.browse->toggleSelect("/Books/book3.epub");
    PB_CHECK_EQ(e.browse->selectedCount(), 0);   // one change at a time per entry (specs/002 research R-6)
    PB_CHECK(row_named(*e.browse, "book3.epub").value("busy").toBool());
    e.browse->toggleSelect("/Books/new-and-pending.txt");   // a path that is not a row of the page is ignored
    PB_CHECK_EQ(e.browse->selectedCount(), 0);
}

PB_TEST(delete_is_offered_only_when_the_last_account_check_reported_a_trash) {
    Env e;
    PB_CHECK(!e.browse->deleteOffered());   // nothing known: counts as no trash
    pbc::ServerCaps caps;
    caps.trash = true;
    PB_CHECK(e.store->save_server_caps(caps).ok());
    e.browse->refreshRows();
    PB_CHECK(e.browse->deleteOffered());
    caps.trash = false;
    PB_CHECK(e.store->save_server_caps(caps).ok());
    e.browse->refreshRows();
    PB_CHECK(!e.browse->deleteOffered());
}

// ---------------------------------------------------------------------------------------------- files a folder pair keeps (specs/003-folder-sync)

namespace {

// A pair for /Books with a reader folder and a recorded copy of book2.epub that matches what is on the card.
int64_t pair_with_copy(Env& e, const std::string& rel, const std::string& etag, const std::string& content) {
    const std::string reader = e.dir.file("Reader");
    ::mkdir(reader.c_str(), 0700);
    pbc::SyncPair p = pbc::default_pair(e.server.base_url(), "demo");
    p.remote_folder = "/Books";
    p.local_folder = reader;
    auto r = e.store->sync_pair_add(p);
    PB_CHECK(r.ok());
    std::ofstream(reader + "/" + rel, std::ios::binary) << content;
    struct stat st;
    PB_CHECK(::stat((reader + "/" + rel).c_str(), &st) == 0);
    pbc::SyncRecord rec;
    rec.pair_id = r.value;
    rec.rel = rel;
    rec.local_rel = rel;
    rec.origin = "download";
    rec.server_etag = etag;
    rec.local_size = st.st_size;
    rec.local_mtime = st.st_mtime;
    rec.synced_at = 1;
    PB_CHECK(e.store->sync_record_put(rec).ok());
    return r.value;
}

QVariantMap row_named(Env& e, const char* name) {
    for (const QVariant& v : e.browse->rows())
        if (v.toMap().value("name").toString() == name) return v.toMap();
    return QVariantMap();
}

}  // namespace

PB_TEST(a_file_a_pair_keeps_is_shown_as_downloaded_and_carries_the_pair) {
    Env e;
    e.browse->open("/Books");
    e.settle();
    const std::string etag = e.server.etag_of("/Books/book2.epub");
    const int64_t pair = pair_with_copy(e, "book2.epub", etag, std::string(300, 'x'));
    e.browse->refresh();
    e.settle();
    const QVariantMap row = row_named(e, "book2.epub");
    PB_CHECK_EQ(row.value("syncedBy").toLongLong(), static_cast<qlonglong>(pair));
    PB_CHECK_EQ(row.value("state").toString().toStdString(), std::string("downloaded"));
    // a file of the folder with no record is not synced
    PB_CHECK_EQ(row_named(e, "book3.epub").value("syncedBy").toLongLong(), 0LL);
    PB_CHECK_EQ(row_named(e, "book3.epub").value("state").toString().toStdString(), std::string("not_downloaded"));
    // a folder carries none
    PB_CHECK_EQ(row_named(e, "Novels").value("syncedBy").toLongLong(), 0LL);
    // the server has a newer version, and the copy was changed on the reader: the words of feature 001
    e.server.touch_file("/Books/book2.epub", "newer on the server", 1790009999);
    e.browse->refresh();
    e.settle();
    PB_CHECK_EQ(row_named(e, "book2.epub").value("state").toString().toStdString(), std::string("outdated"));
    std::ofstream(e.dir.file("Reader/book2.epub"), std::ios::trunc) << "changed on the reader, and longer than before";
    e.browse->refresh();
    e.settle();
    PB_CHECK_EQ(row_named(e, "book2.epub").value("state").toString().toStdString(), std::string("conflict"));
}

PB_TEST(a_download_of_feature_001_is_consulted_first_and_has_no_pair) {
    Env e;
    pair_with_copy(e, "book2.epub", "\"old\"", "x");
    // the 001 copy of the same server path, in the download root
    ::mkdir(e.dir.file("PBCloud").c_str(), 0700);
    ::mkdir(e.dir.file("PBCloud/Books").c_str(), 0700);
    std::ofstream(e.dir.file("PBCloud/Books/book2.epub")) << std::string(300, 'x');
    pbc::LocalFile lf;
    lf.remote_path = "/Books/book2.epub";
    lf.local_path = "Books/book2.epub";
    lf.size = 300;
    struct stat st;
    PB_CHECK(::stat(e.dir.file("PBCloud/Books/book2.epub").c_str(), &st) == 0);
    lf.mtime = st.st_mtime;
    lf.remote_etag = e.server.etag_of("/Books/book2.epub");
    lf.downloaded_at = 1;
    PB_CHECK(e.store->commit_download(lf, 0).ok());
    e.browse->open("/Books");
    e.settle();
    const QVariantMap row = row_named(e, "book2.epub");
    PB_CHECK_EQ(row.value("syncedBy").toLongLong(), 0LL);
    PB_CHECK_EQ(row.value("state").toString().toStdString(), std::string("downloaded"));
}

PB_TEST_MAIN()
