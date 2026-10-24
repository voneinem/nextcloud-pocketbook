// The versions bridge (specs/006-trash-and-versions, contracts/bridge-qml.md "VersionsBridge"). This file grows with the stories: Phase 3 (US5) holds
// what the server's versions app decides, and the entries the Versions tool is not offered for.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include "fake_nextcloud_server.h"
#include "pbc/credentials_file.h"
#include "pbc/folder_service.h"
#include "pbc/store.h"
#include "pbc/store_files.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "versions_bridge.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_versions_bridge";
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
    std::unique_ptr<VersionsBridge> versions;

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
        server.add_file("/Docs/a.pdf", "current", 1760000300);
        server.add_version("/Docs/a.pdf", "older", 1760000100);
        pbc::FolderService fs(*store, server, clock);
        PB_CHECK(fs.load("/Docs", pbc::Fetch::Always).error.ok());
        PB_CHECK(fs.load("/", pbc::Fetch::Always).error.ok());
        server.clear_log();
        VersionsBridge::Config c;
        c.store = store.get();
        c.worker = &worker;
        c.http = &server;
        c.clock = &clock;
        c.wifi_up = [this] { return wifi; };
        c.format_date = [](qint64 t) { return QStringLiteral("d%1").arg(t); };
        versions = std::make_unique<VersionsBridge>(c);
    }
    ~Env() {
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    void wait() {
        QElapsedTimer t;
        t.start();
        while (versions->busy() && t.elapsed() < 10000) QTest::qWait(5);
        QTest::qWait(20);
    }
    std::string err(const QVariantMap& m) { return m.value("errorKey").toString().toStdString(); }
    std::string state() { return versions->state().toStdString(); }
    std::string message_key() { return versions->message().value("key").toString().toStdString(); }
    std::string availability(const char* app) { return versions->availability().value(app).toString().toStdString(); }
    QVariantMap row(int i) { return versions->rows().at(i).toMap(); }
    void cache_docs() {
        pbc::FolderService fs(*store, server, clock);
        PB_CHECK(fs.load("/Docs", pbc::Fetch::Always).error.ok());
        server.clear_log();
    }
    void open_and_wait() {
        versions->open("/Docs/a.pdf");
        wait();
    }
    bool wait_for(QSignalSpy& spy, int ms = 10000) {
        QElapsedTimer timer;
        timer.start();
        while (spy.isEmpty() && timer.elapsed() < ms) QTest::qWait(5);
        return !spy.isEmpty();
    }
    size_t moves() { return server.count("MOVE"); }
};

}  // namespace

bool mentions(const QVariantMap& m, const char* needle) {
    for (const QVariant& value : m)
        if (value.toString().contains(QString::fromLatin1(needle))) return true;
    return false;
}

PB_TEST(a_new_bridge_is_closed_and_offers_nothing) {
    Env e;
    PB_CHECK_EQ(e.state(), std::string("closed"));
    PB_CHECK(!e.versions->busy() && !e.versions->canAct());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(with_the_versions_app_off_the_screen_explains_and_offers_no_action) {
    Env e;
    e.server.set_versions(false);
    PB_CHECK(e.versions->open("/Docs/a.pdf").value("ok").toBool());
    e.wait();
    PB_CHECK_EQ(e.state(), std::string("explainOff"));
    PB_CHECK_EQ(e.availability("versions"), std::string("off"));
    PB_CHECK_EQ(e.message_key(), std::string("versions_unavailable"));
    PB_CHECK(!e.versions->canAct());
    PB_CHECK_EQ(e.server.request_count(), size_t(1));   // the capabilities only: no version list was asked for
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(0));
}

PB_TEST(the_two_apps_are_judged_on_their_own) {
    Env e;
    e.server.set_trash(false);   // the trash is off, the versions app is on: the versions screen works
    e.versions->open("/Docs/a.pdf");
    e.wait();
    PB_CHECK_EQ(e.availability("versions"), std::string("on"));
    PB_CHECK_EQ(e.availability("trash"), std::string("off"));
    PB_CHECK(e.state() != "explainOff" && e.state() != "explainUnknown");
}

PB_TEST(an_unclear_answer_is_a_generic_message_with_a_code_never_off) {
    Env e;
    e.server.set_capabilities_failing(true);
    e.versions->open("/Docs/a.pdf");
    e.wait();
    PB_CHECK_EQ(e.state(), std::string("explainUnknown"));
    PB_CHECK_EQ(e.availability("versions"), std::string("unknown"));
    PB_CHECK_EQ(e.versions->message().value("code").toString().toStdString(), std::string("T1-500"));
    PB_CHECK(e.message_key() != "versions_unavailable" && e.message_key() != "trash_unavailable");
}

PB_TEST(a_folder_and_a_missing_entry_send_nothing) {
    Env e;
    PB_CHECK_EQ(e.err(e.versions->open("/Docs")), std::string("trash_not_actionable"));   // folders have no versions
    PB_CHECK_EQ(e.err(e.versions->open("/Docs/none.pdf")), std::string("file_gone"));
    PB_CHECK(e.state() == "closed");
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(offline_nothing_is_sent_and_nothing_is_claimed_about_the_app) {
    Env e;
    e.wifi = false;
    PB_CHECK_EQ(e.err(e.versions->open("/Docs/a.pdf")), std::string("no_network"));
    PB_CHECK(e.state() != "explainOff" && e.state() != "explainUnknown");
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_second_tap_while_a_request_runs_is_refused_with_busy) {
    Env e;
    PB_CHECK(e.versions->open("/Docs/a.pdf").value("ok").toBool());
    PB_CHECK_EQ(e.err(e.versions->open("/Docs/a.pdf")), std::string("busy"));
    e.wait();
}

// ---- Phase 6 (US3): the versions of a file ----

PB_TEST(opening_the_versions_lists_the_older_ones_newest_first_with_date_and_size) {
    Env e;
    e.server.add_version("/Docs/a.pdf", "second older", 1760000200);
    e.open_and_wait();
    PB_CHECK_EQ(e.state(), std::string("listed"));
    PB_CHECK_EQ(e.versions->total(), 2);
    PB_CHECK(e.versions->canAct());
    PB_CHECK_EQ(e.versions->fileName().toStdString(), std::string("a.pdf"));
    PB_CHECK(e.row(0).value("dateText").toString().contains("1760000200"));
    PB_CHECK_EQ(e.row(0).value("sizeText").toString().toStdString(), std::string("12 B"));
    PB_CHECK_EQ(e.row(1).value("sizeText").toString().toStdString(), std::string("5 B"));
    PB_CHECK(!e.row(0).value("chosen").toBool() && !e.row(0).value("future").toBool());
    PB_CHECK_EQ(e.server.request_count(), size_t(3));   // the capabilities, the file, the versions
    // no id of the server is exposed (the date text is the test's own format of the date, which holds the epoch by construction)
    for (const QVariant& v : e.versions->rows()) PB_CHECK(!v.toMap().contains("versionId") && !v.toMap().contains("version_id") && !v.toMap().contains("id"));
}

PB_TEST(a_file_without_older_versions_says_so) {
    Env e;
    e.server.add_file("/Docs/b.pdf", "x");
    e.cache_docs();
    e.versions->open("/Docs/b.pdf");
    e.wait();
    PB_CHECK_EQ(e.state(), std::string("empty"));
    PB_CHECK_EQ(e.message_key(), std::string("no_older_versions"));
    PB_CHECK(e.versions->rows().isEmpty());
    PB_CHECK(e.versions->canAct());   // Reload stays available
}

PB_TEST(the_list_is_paged) {
    Env e;
    for (int i = 0; i < 25; ++i) e.server.add_version("/Docs/a.pdf", "v" + std::to_string(i), 1760001000 + i);
    e.versions->setPageSize(10);
    e.open_and_wait();
    PB_CHECK_EQ(e.versions->pageCount(), 3);
    PB_CHECK_EQ(e.versions->rows().size(), 10);
    e.versions->nextPage();
    e.versions->nextPage();
    PB_CHECK_EQ(e.versions->rows().size(), 6);
    e.versions->nextPage();
    PB_CHECK_EQ(e.versions->page(), 2);
    e.versions->previousPage();
    PB_CHECK_EQ(e.versions->page(), 1);
}

PB_TEST(exactly_one_version_is_chosen_and_the_mark_moves) {
    Env e;
    e.server.add_version("/Docs/a.pdf", "second", 1760000200);
    e.open_and_wait();
    PB_CHECK(e.versions->choose(1).value("ok").toBool());
    PB_CHECK(!e.row(0).value("chosen").toBool() && e.row(1).value("chosen").toBool());
    PB_CHECK(e.versions->choose(0).value("ok").toBool());
    PB_CHECK(e.row(0).value("chosen").toBool() && !e.row(1).value("chosen").toBool());
    PB_CHECK_EQ(e.err(e.versions->choose(9)), std::string("version_gone"));
}

PB_TEST(the_confirmation_names_the_file_and_the_version_and_after_it_the_version_is_current_and_the_previous_content_is_listed) {
    Env e;
    pbc::LocalFile lf;   // a local copy of the file: no file of the reader is touched
    lf.remote_path = "/Docs/a.pdf";
    lf.local_path = "Docs/a.pdf";
    lf.size = 7;
    PB_CHECK(e.store->commit_download(lf, 0).ok());
    e.open_and_wait();
    PB_CHECK_EQ(e.err(e.versions->restore()), std::string("version_gone"));   // nothing is chosen
    e.versions->choose(0);
    e.server.clear_log();
    const QVariantMap c = e.versions->restore();
    PB_CHECK(c.value("needsConfirm").toBool());
    const QVariantMap summary = c.value("summary").toMap();
    PB_CHECK_EQ(summary.value("kind").toString().toStdString(), std::string("restore_version"));
    PB_CHECK_EQ(summary.value("names").toStringList().value(0).toStdString(), std::string("a.pdf"));
    PB_CHECK(summary.value("dateText").toString().contains("1760000100"));
    PB_CHECK_EQ(summary.value("sizeText").toString().toStdString(), std::string("5 B"));
    PB_CHECK(summary.value("defaultCancel").toBool() && !summary.value("destructive").toBool());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));   // nothing is sent before the confirmation
    QSignalSpy done(e.versions.get(), &VersionsBridge::actionFinished);
    PB_CHECK(e.versions->restoreConfirmed().value("ok").toBool());
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK(r.value("ok").toBool() && !r.value("previousNotKept").toBool());
    PB_CHECK_EQ(e.moves(), size_t(1));
    PB_CHECK_EQ(e.server.content_of("/Docs/a.pdf"), std::string("older"));
    PB_CHECK_EQ(e.versions->total(), 2);   // the list shows the previous current content as a version now
    PB_CHECK(!e.store->folder("/Docs").has_value());                                // the next visit of the folder fetches it
    PB_CHECK(e.store->local_file("/Docs/a.pdf").has_value());                        // the local copy is untouched; it shows outdated by the ordinary rule
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM queue_items"), std::string("0"));
    PB_CHECK_EQ(e.err(e.versions->restoreConfirmed()), std::string("nothing_pending"));   // one confirmation, one request
    PB_CHECK(!mentions(r, "a.pdf"));
}

PB_TEST(a_file_that_changed_after_the_list_was_read_is_refused_and_nothing_is_sent_but_the_check) {
    Env e;
    e.open_and_wait();
    e.versions->choose(0);
    PB_CHECK(e.versions->restore().value("needsConfirm").toBool());
    e.server.set_file("/Docs/a.pdf", "changed on another device", 1760000500);
    e.server.clear_log();
    QSignalSpy done(e.versions.get(), &VersionsBridge::actionFinished);
    e.versions->restoreConfirmed();
    PB_CHECK(e.wait_for(done));
    PB_CHECK_EQ(done.at(0).at(0).toMap().value("key").toString().toStdString(), std::string("version_changed_meanwhile"));
    PB_CHECK_EQ(e.moves(), size_t(0));
    PB_CHECK_EQ(e.server.content_of("/Docs/a.pdf"), std::string("changed on another device"));
    // Reload reads the fresh list and the restore can be tried again
    e.versions->reload();
    e.wait();
    PB_CHECK_EQ(e.state(), std::string("listed"));
    PB_CHECK(e.versions->message().isEmpty() || e.message_key().empty());
}

PB_TEST(a_refused_restore_keeps_the_list_and_says_why_without_a_name) {
    Env e;
    e.open_and_wait();
    e.versions->choose(0);
    e.versions->restore();
    e.server.answer_next("MOVE", "/versions/", 507);
    QSignalSpy done(e.versions.get(), &VersionsBridge::actionFinished);
    e.versions->restoreConfirmed();
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK_EQ(r.value("key").toString().toStdString(), std::string("server_storage_full"));
    PB_CHECK_EQ(r.value("code").toString().toStdString(), std::string("T7-507"));
    PB_CHECK_EQ(e.versions->total(), 1);
    PB_CHECK_EQ(e.server.content_of("/Docs/a.pdf"), std::string("current"));
    PB_CHECK(!mentions(r, "a.pdf"));
}

PB_TEST(a_restore_whose_answer_was_lost_is_unknown_not_repeated_and_a_reload_shows_the_truth) {
    Env e;
    e.open_and_wait();
    e.versions->choose(0);
    e.versions->restore();
    e.server.lose_next_answer("MOVE", "/versions/");
    QSignalSpy done(e.versions.get(), &VersionsBridge::actionFinished);
    e.versions->restoreConfirmed();
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK(r.value("outcomeUnknown").toBool());
    PB_CHECK_EQ(r.value("key").toString().toStdString(), std::string("outcome_unknown"));
    PB_CHECK_EQ(e.moves(), size_t(1));
    e.versions->reload();
    e.wait();
    PB_CHECK_EQ(e.versions->total(), 2);   // the server had done it: the previous current content is a version
    PB_CHECK_EQ(e.moves(), size_t(1));
}

PB_TEST(a_server_that_does_not_keep_the_previous_content_is_reported_plainly) {
    Env e;
    e.server.set_versions_keep_current(false);
    e.open_and_wait();
    e.versions->choose(0);
    e.versions->restore();
    QSignalSpy done(e.versions.get(), &VersionsBridge::actionFinished);
    e.versions->restoreConfirmed();
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK(r.value("ok").toBool());
    PB_CHECK(r.value("previousNotKept").toBool());
}

PB_TEST(the_restore_needs_the_connection_and_cancel_drops_the_confirmation) {
    Env e;
    e.open_and_wait();
    e.versions->choose(0);
    PB_CHECK(e.versions->restore().value("needsConfirm").toBool());
    e.versions->cancelStep();
    PB_CHECK_EQ(e.err(e.versions->restoreConfirmed()), std::string("nothing_pending"));
    e.wifi = false;
    PB_CHECK_EQ(e.err(e.versions->restore()), std::string("no_network"));
    e.server.clear_log();
    PB_CHECK_EQ(e.moves(), size_t(0));
}

PB_TEST(with_the_versions_app_turned_off_meanwhile_the_restore_is_refused) {
    Env e;
    e.open_and_wait();
    e.versions->choose(0);
    e.server.set_versions(false);
    e.versions->open("/Docs/a.pdf");
    e.wait();
    PB_CHECK_EQ(e.state(), std::string("explainOff"));
    PB_CHECK_EQ(e.err(e.versions->restore()), std::string("versions_unavailable"));
}

PB_TEST_MAIN()
