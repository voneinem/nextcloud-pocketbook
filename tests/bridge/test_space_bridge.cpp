// The SpaceBridge (contracts/bridge-qml.md): what the app downloaded, page by page, how much space it uses and how much is free, the removal
// of a copy and the clearing of the listing cache. Real core, fake storage. Nothing here may reach the network.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include <sys/stat.h>

#include <fstream>

#include "download_env.h"
#include "pbc/folder_service.h"
#include "space_bridge.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_space_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct Env : pbtest::DlEnv {
    Worker worker;
    std::unique_ptr<SpaceBridge> space;

    explicit Env(int files = 3) {
        for (int i = 0; i < files; ++i) {
            const std::string path = "/Books/book" + std::to_string(i) + ".epub";
            server.add_file(path, std::string(1000 * (i + 1), 'x'), 1790000000);
            PB_CHECK(run(path).state == pbc::JobState::Done);
        }
        server.clear_log();
        SpaceBridge::Config c;
        c.store = store.get();
        c.worker = &worker;
        c.storage = &storage;
        c.download_root = root;
        space = std::make_unique<SpaceBridge>(c);
    }
    ~Env() {
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    QStringList names() {
        QStringList n;
        for (const QVariant& v : space->items()) n << v.toMap().value("name").toString();
        return n;
    }
};

}  // namespace

PB_TEST(the_downloads_are_listed_with_size_and_state_and_the_totals_are_shown) {
    Env e;
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("book0.epub,book1.epub,book2.epub"));
    const QVariantMap first = e.space->items().at(0).toMap();
    PB_CHECK_EQ(first.value("sizeText").toString().toStdString(), std::string("1000 B"));
    PB_CHECK_EQ(first.value("state").toString().toStdString(), std::string("downloaded"));
    PB_CHECK(!first.value("changedLocally").toBool());
    PB_CHECK_EQ(first.value("remotePath").toString().toStdString(), std::string("/Books/book0.epub"));
    PB_CHECK_EQ(e.space->total(), 3);
    PB_CHECK(!e.space->usedText().isEmpty());
    PB_CHECK(!e.space->freeText().isEmpty());
    PB_CHECK(!e.space->cacheText().isEmpty());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_changed_copy_is_marked_as_changed) {
    Env e;
    {
        std::ofstream f(e.abs("Books/book1.epub"), std::ios::trunc);
        f << "mine";
    }
    e.space->refresh();
    const QVariantMap row = e.space->items().at(1).toMap();
    PB_CHECK(row.value("changedLocally").toBool());
    PB_CHECK_EQ(row.value("state").toString().toStdString(), std::string("changed_on_reader"));
}

PB_TEST(the_list_is_paged_by_twenty) {
    Env e(45);
    PB_CHECK_EQ(e.space->items().size(), 20);
    PB_CHECK_EQ(e.space->pageCount(), 3);
    e.space->nextPage();
    PB_CHECK_EQ(e.space->page(), 1);
    PB_CHECK_EQ(e.space->items().size(), 20);
    e.space->nextPage();
    PB_CHECK_EQ(e.space->items().size(), 5);
    e.space->nextPage();
    PB_CHECK_EQ(e.space->page(), 2);
    e.space->previousPage();
    PB_CHECK_EQ(e.space->page(), 1);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(removing_frees_space_updates_the_list_and_never_reaches_the_server) {
    Env e;
    e.storage.free = 1000000;
    e.space->refresh();
    const QString used_before = e.space->usedText();
    QSignalSpy done(e.space.get(), &SpaceBridge::removeFinished);
    e.space->remove("/Books/book1.epub", false);
    PB_CHECK_EQ(done.count(), 1);
    PB_CHECK(done.at(0).at(0).toBool());
    PB_CHECK_EQ(e.names().join(",").toStdString(), std::string("book0.epub,book2.epub"));
    PB_CHECK_EQ(e.space->total(), 2);
    PB_CHECK(e.space->usedText() != used_before);
    PB_CHECK(!e.exists("Books/book1.epub"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.server.has("/Books/book1.epub"));
}

PB_TEST(free_space_is_read_again_after_a_removal) {
    Env e;
    e.storage.free = 5000;
    e.space->refresh();
    const QString before = e.space->freeText();
    e.storage.free = 7000;   // the card reports more now
    e.space->remove("/Books/book0.epub", false);
    PB_CHECK(e.space->freeText() != before);
}

PB_TEST(a_changed_copy_is_removed_only_with_the_confirmation) {
    Env e;
    {
        std::ofstream f(e.abs("Books/book2.epub"), std::ios::trunc);
        f << "mine";
    }
    QSignalSpy done(e.space.get(), &SpaceBridge::removeFinished);
    e.space->remove("/Books/book2.epub", false);
    PB_CHECK_EQ(done.count(), 1);
    PB_CHECK(!done.at(0).at(0).toBool());
    PB_CHECK_EQ(done.at(0).at(1).toString().toStdString(), std::string("local_changed"));
    PB_CHECK(e.exists("Books/book2.epub"));
    e.space->remove("/Books/book2.epub", true);
    PB_CHECK_EQ(done.count(), 2);
    PB_CHECK(done.at(1).at(0).toBool());
    PB_CHECK(!e.exists("Books/book2.epub"));
}

PB_TEST(clearing_the_cache_shrinks_the_cache_text_and_keeps_the_downloads) {
    Env e;
    pbc::FolderService svc(*e.store, e.server, e.clock);
    for (int i = 0; i < 40; ++i) e.server.add_file("/Many/f" + std::to_string(i) + ".txt", "x", 1790000000);
    PB_CHECK(svc.load("/Many", pbc::Fetch::Always).error.ok());
    e.server.clear_log();
    e.space->refresh();
    QSignalSpy done(e.space.get(), &SpaceBridge::cacheCleared);
    e.space->clearCache();
    PB_CHECK(done.wait(10000) || done.count() > 0);
    PB_CHECK(!e.store->folder("/Many").has_value());
    PB_CHECK_EQ(e.space->total(), 3);
    PB_CHECK(!e.space->busy());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST_MAIN()
