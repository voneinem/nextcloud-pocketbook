// The trash bridge (specs/006-trash-and-versions, contracts/bridge-qml.md "TrashBridge"). This file grows with the stories: Phase 3 (US5) holds what
// the server's apps decide - on, off or unclear - and that no action is offered when the trash cannot be used; Phase 4 (US1) holds the list itself.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include "fake_nextcloud_server.h"
#include "fixtures.h"
#include "pbc/credentials_file.h"
#include "pbc/folder_service.h"
#include "pbc/store.h"
#include "pbc/store_files.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "trash_bridge.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_trash_bridge";
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
    bool net_ok = true;
    int ensure_calls = 0;
    std::unique_ptr<TrashBridge> trash;

    explicit Env(bool with_ensure = false, bool delete_enabled = false) {
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
        TrashBridge::Config c;
        c.store = store.get();
        c.worker = &worker;
        c.http = &server;
        c.clock = &clock;
        c.wifi_up = [this] { return wifi; };
        c.format_date = [](qint64 t) { return QStringLiteral("d%1").arg(t); };
        c.delete_enabled = delete_enabled;
        if (with_ensure)
            c.ensure_network = [this](std::function<void(bool)> done) {
                ++ensure_calls;
                if (net_ok) wifi = true;
                done(net_ok);
            };
        trash = std::make_unique<TrashBridge>(c);
    }
    ~Env() {
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    void wait() {
        QElapsedTimer t;
        t.start();
        while (trash->busy() && t.elapsed() < 10000) QTest::qWait(5);
        QTest::qWait(20);
    }
    std::string err(const QVariantMap& m) { return m.value("errorKey").toString().toStdString(); }
    std::string state() { return trash->state().toStdString(); }
    std::string message_key() { return trash->message().value("key").toString().toStdString(); }
    std::string availability(const char* app) { return trash->availability().value(app).toString().toStdString(); }
    int64_t t(int64_t ago) { return server.server_time() - ago; }
    QVariantMap row(int i) { return trash->rows().at(i).toMap(); }
    void open_and_wait() {
        trash->open();
        wait();
    }
    // the Books folder with a trashed file in it, listed and cached, the trash opened and its first item chosen
    std::string story() {
        server.add_folder("/Books");
        const std::string name = server.add_trash_file("/Books/story.epub", "the story", t(100));
        cache("/Books");
        open_and_wait();
        PB_CHECK(trash->choose(0).value("ok").toBool());
        server.clear_log();
        return name;
    }
    void cache(const std::string& folder) {
        pbc::FolderService fs(*store, server, clock);
        PB_CHECK(fs.load(folder, pbc::Fetch::Always).error.ok());
    }
    bool wait_for(QSignalSpy& spy, int ms = 10000) {
        QElapsedTimer timer;
        timer.start();
        while (spy.isEmpty() && timer.elapsed() < ms) QTest::qWait(5);
        return !spy.isEmpty();
    }
    size_t moves() { return server.count("MOVE"); }
    void hostile_listing() {   // the trash answers the hostile fixture
        server.set_interceptor([](const pbc::Request& r) -> std::optional<pbc::Response> {
            if (r.method != "PROPFIND") return std::nullopt;
            pbc::Response resp;
            resp.status = 207;
            resp.body = pbtest::fixture("nextcloud/trash/trash_list_hostile.unverified.xml");
            return resp;
        });
    }
};

bool mentions(const QVariantList& rows, const QString& needle) {
    for (const QVariant& v : rows)
        for (const QVariant& value : v.toMap())
            if (value.toString().contains(needle)) return true;
    return false;
}

}  // namespace

PB_TEST(a_new_bridge_is_closed_and_offers_nothing) {
    Env e;
    PB_CHECK_EQ(e.state(), std::string("closed"));
    PB_CHECK(!e.trash->busy());
    PB_CHECK(!e.trash->canAct());
    PB_CHECK(e.trash->message().isEmpty());
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(with_the_trash_app_off_the_screen_explains_and_offers_no_action) {
    Env e;
    e.server.set_trash(false);
    const auto r = e.trash->open();
    PB_CHECK(r.value("ok").toBool());
    e.wait();
    PB_CHECK_EQ(e.state(), std::string("explainOff"));
    PB_CHECK_EQ(e.availability("trash"), std::string("off"));
    PB_CHECK(!e.trash->canAct());
    PB_CHECK_EQ(e.message_key(), std::string("trash_unavailable"));
    // exactly one request was made: the capabilities; the trash itself was not asked for
    PB_CHECK_EQ(e.server.request_count(), size_t(1));
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(0));
}

PB_TEST(a_missing_key_is_off_too_and_the_message_does_not_name_the_other_app) {
    Env e;
    e.server.set_trash(std::nullopt);
    e.trash->open();
    e.wait();
    PB_CHECK_EQ(e.state(), std::string("explainOff"));
    PB_CHECK_EQ(e.availability("versions"), std::string("on"));   // the versions app is judged on its own
}

PB_TEST(with_the_trash_app_on_nothing_is_explained) {
    Env e;
    e.trash->open();
    e.wait();
    PB_CHECK(e.state() != "explainOff" && e.state() != "explainUnknown" && e.state() != "failed");
    PB_CHECK_EQ(e.availability("trash"), std::string("on"));
    PB_CHECK(e.trash->message().isEmpty());
}

PB_TEST(an_unclear_answer_is_a_generic_message_with_a_code_and_never_disabled) {
    Env e;
    e.server.set_capabilities_failing(true);
    e.trash->open();
    e.wait();
    PB_CHECK_EQ(e.state(), std::string("explainUnknown"));
    PB_CHECK_EQ(e.availability("trash"), std::string("unknown"));
    PB_CHECK_EQ(e.trash->message().value("code").toString().toStdString(), std::string("T1-500"));
    PB_CHECK(e.message_key() != "trash_unavailable" && e.message_key() != "versions_unavailable");
    PB_CHECK(!e.trash->canAct());
    // an answer without the files object is unclear in the same way
    Env f;
    f.server.set_capabilities_without_files(true);
    f.trash->open();
    f.wait();
    PB_CHECK_EQ(f.state(), std::string("explainUnknown"));
    PB_CHECK_EQ(f.trash->message().value("code").toString().toStdString(), std::string("T1-200"));
}

PB_TEST(a_wrong_password_is_reported_as_such_not_as_an_app_being_off) {
    Env e;
    pbc::Credentials cr;
    cr.password = "wrong";
    PB_CHECK(pbc::save_credentials(e.store->directory(), cr).ok());
    e.trash->open();
    e.wait();
    PB_CHECK_EQ(e.state(), std::string("failed"));
    PB_CHECK_EQ(e.message_key(), std::string("auth_failed"));
    PB_CHECK(e.availability("trash") != "off");
}

PB_TEST(offline_nothing_is_sent_and_nothing_is_claimed_about_the_apps) {
    Env e;
    e.wifi = false;
    const auto r = e.trash->open();
    PB_CHECK_EQ(e.err(r), std::string("no_network"));
    PB_CHECK(e.state() != "explainOff" && e.state() != "explainUnknown");
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(!e.trash->canAct());
    Env f(true);   // wired like main.cpp: a tap brings Wi-Fi up, and when it does not come up the tap fails
    f.wifi = false;
    f.net_ok = false;
    f.trash->open();   // the system is asked to bring Wi-Fi up; when it does not, the tap fails and nothing was sent
    f.wait();
    PB_CHECK_EQ(f.ensure_calls, 1);
    PB_CHECK_EQ(f.state(), std::string("failed"));
    PB_CHECK_EQ(f.message_key(), std::string("no_network"));
    PB_CHECK(!f.trash->busy());
    PB_CHECK_EQ(f.server.request_count(), size_t(0));
}

PB_TEST(a_second_tap_while_a_request_runs_is_refused_with_busy) {
    Env e;
    PB_CHECK(e.trash->open().value("ok").toBool());
    PB_CHECK_EQ(e.err(e.trash->open()), std::string("busy"));
    e.wait();
    PB_CHECK_EQ(e.server.count("GET"), size_t(1));   // one capabilities request, not two
}

PB_TEST(nothing_is_requested_unless_the_user_opens_the_trash) {
    Env e;
    QTest::qWait(100);
    QCoreApplication::processEvents();
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

// ---- Phase 4 (US1): see what was deleted ----

PB_TEST(opening_the_trash_lists_every_item_with_its_fields_newest_first) {
    Env e;
    e.server.add_trash_file("/Books/story.epub", "1234567890", e.t(100));
    e.server.add_trash_folder("/Documents/Old Notes", e.t(200), {{"/a.txt", "a"}});
    e.server.add_trash_file("/readme.txt", "x", e.t(300));
    e.open_and_wait();
    PB_CHECK_EQ(e.state(), std::string("listed"));
    PB_CHECK_EQ(e.trash->total(), 3);
    PB_CHECK(!e.trash->fromStore() && !e.trash->truncated());
    PB_CHECK(e.trash->canAct());
    const auto a = e.row(0);
    PB_CHECK_EQ(a.value("name").toString().toStdString(), std::string("story.epub"));
    PB_CHECK(a.value("originalText").toString().contains("/Books"));
    PB_CHECK(a.value("deletedText").toString().contains(QString::number(e.t(100))));
    PB_CHECK_EQ(a.value("sizeText").toString().toStdString(), std::string("10 B"));
    PB_CHECK(!a.value("isFolder").toBool() && !a.value("unreadable").toBool() && a.value("actionable").toBool() && !a.value("chosen").toBool());
    const auto f = e.row(1);
    PB_CHECK(f.value("isFolder").toBool());
    PB_CHECK(f.value("sizeText").toString().isEmpty());
    PB_CHECK(e.row(2).value("originalText").toString().contains("/"));   // an item from the root
    // the requests: the capabilities, then the trash (Depth 1), nothing else
    PB_CHECK_EQ(e.server.request_count(), size_t(2));
    PB_CHECK_EQ(e.server.log()[1].method, std::string("PROPFIND"));
    // no identity of the server (the trash name) is exposed to the screen
    PB_CHECK(!mentions(e.trash->rows(), QStringLiteral(".d17")));
    PB_CHECK(!e.row(0).contains("trash_name") && !e.row(0).contains("trashName"));
}

PB_TEST(an_empty_trash_says_so_with_an_empty_list) {
    Env e;
    e.open_and_wait();
    PB_CHECK_EQ(e.state(), std::string("listed"));
    PB_CHECK_EQ(e.trash->total(), 0);
    PB_CHECK(e.trash->rows().isEmpty());
    PB_CHECK(e.trash->ageKind() == "just_now");   // the list was just fetched
}

PB_TEST(the_list_is_paged_with_page_buttons) {
    Env e;
    for (int i = 0; i < 25; ++i) e.server.add_trash_file("/F/f" + std::to_string(i) + ".txt", "x", e.t(10 + i));
    e.trash->setPageSize(10);
    e.open_and_wait();
    PB_CHECK_EQ(e.trash->pageCount(), 3);
    PB_CHECK_EQ(e.trash->page(), 0);
    PB_CHECK_EQ(e.trash->rows().size(), 10);
    e.trash->nextPage();
    e.trash->nextPage();
    PB_CHECK_EQ(e.trash->page(), 2);
    PB_CHECK_EQ(e.trash->rows().size(), 5);
    e.trash->nextPage();   // there is no page 4
    PB_CHECK_EQ(e.trash->page(), 2);
    e.trash->previousPage();
    PB_CHECK_EQ(e.trash->page(), 1);
    PB_CHECK_EQ(e.row(0).value("index").toInt(), 10);   // the index is the place in the whole list
    PB_CHECK(e.row(0).value("name").toString().contains("f"));
}

PB_TEST(exactly_one_item_is_chosen_and_the_mark_moves) {
    Env e;
    e.server.add_trash_file("/a.txt", "x", e.t(10));
    e.server.add_trash_file("/b.txt", "x", e.t(20));
    e.open_and_wait();
    PB_CHECK(e.trash->chosenName().isEmpty());
    PB_CHECK(e.trash->choose(1).value("ok").toBool());
    PB_CHECK_EQ(e.trash->chosenName().toStdString(), std::string("b.txt"));
    PB_CHECK(!e.row(0).value("chosen").toBool() && e.row(1).value("chosen").toBool());
    PB_CHECK(e.trash->choose(0).value("ok").toBool());
    PB_CHECK_EQ(e.trash->chosenName().toStdString(), std::string("a.txt"));
    PB_CHECK(e.row(0).value("chosen").toBool() && !e.row(1).value("chosen").toBool());
    PB_CHECK_EQ(e.err(e.trash->choose(5)), std::string("trash_item_gone"));   // no such place in the list
    PB_CHECK_EQ(e.trash->chosenName().toStdString(), std::string("a.txt"));
}

PB_TEST(a_refresh_reads_again_and_drops_the_mark_of_an_item_that_is_gone) {
    Env e;
    e.server.add_trash_file("/a.txt", "x", e.t(10));
    const std::string b = e.server.add_trash_file("/b.txt", "x", e.t(20));
    e.open_and_wait();
    e.trash->choose(1);
    PB_CHECK_EQ(e.trash->chosenName().toStdString(), std::string("b.txt"));
    pbc::Request del;   // another device empties one item
    del.method = "DELETE";
    del.url = e.server.base_url() + "/remote.php/dav/trashbin/demo/trash/" + b;
    del.username = "demo";
    del.password = e.server.app_password();
    PB_CHECK_EQ(e.server.perform(del).status, 204L);
    e.server.clear_log();
    PB_CHECK(e.trash->refresh().value("ok").toBool());
    e.wait();
    PB_CHECK_EQ(e.trash->total(), 1);
    PB_CHECK(e.trash->chosenName().isEmpty());
    PB_CHECK_EQ(e.server.request_count(), size_t(2));   // the capabilities and the list again, on the tap
    PB_CHECK_EQ(e.err(e.trash->refresh()), std::string());   // a second refresh is just another tap
    e.wait();
}

PB_TEST(an_unreadable_or_unavailable_item_is_shown_but_cannot_be_chosen) {
    Env e;
    e.hostile_listing();
    e.open_and_wait();
    PB_CHECK_EQ(e.state(), std::string("listed"));
    int unreadable = 0, not_actionable = 0;
    for (int i = 0; i < e.trash->total(); ++i) {
        e.trash->setPageSize(100);
        const auto r = e.row(i);
        if (r.value("unreadable").toBool()) {
            ++unreadable;
            PB_CHECK_EQ(r.value("name").toString().toStdString(), std::string("Unreadable item"));
        }
        if (!r.value("actionable").toBool()) {
            ++not_actionable;
            PB_CHECK_EQ(e.err(e.trash->choose(r.value("index").toInt())), std::string("trash_not_actionable"));
        }
    }
    PB_CHECK_EQ(unreadable, 4);
    PB_CHECK(not_actionable >= 7);   // four unreadable, one without permission, one encrypted, one of an unknown type
    PB_CHECK(e.trash->chosenName().isEmpty());
}

PB_TEST(more_than_1000_items_are_cut_and_the_screen_says_so) {
    Env e;
    for (int i = 0; i < 1200; ++i) e.server.add_trash_file("/M/f" + std::to_string(i) + ".txt", "x", e.t(10 + i));
    e.open_and_wait();
    PB_CHECK_EQ(e.trash->total(), 1000);
    PB_CHECK(e.trash->truncated());
    PB_CHECK_EQ(e.store->load_trash(1).value.items.size(), size_t(1000));
    PB_CHECK(e.store->load_trash(1).value.truncated);
}

PB_TEST(a_fetched_list_is_stored_and_shown_again_with_its_age_when_the_reader_is_offline) {
    Env e;
    e.server.add_trash_file("/a.txt", "x", e.t(10));
    e.server.add_trash_file("/b.txt", "xy", e.t(20));
    e.open_and_wait();
    PB_CHECK_EQ(e.store->load_trash(1).value.items.size(), size_t(2));
    // a later visit with no Wi-Fi: the stored list, marked with its age, nothing sent, the server actions unavailable
    e.clock.t += 3 * 3600;
    e.wifi = false;
    e.server.clear_log();
    const auto r = e.trash->open();
    PB_CHECK_EQ(e.err(r), std::string("no_network"));
    PB_CHECK_EQ(e.state(), std::string("listed"));
    PB_CHECK(e.trash->fromStore());
    PB_CHECK_EQ(e.trash->total(), 2);
    PB_CHECK_EQ(e.trash->ageKind().toStdString(), std::string("hours"));
    PB_CHECK_EQ(e.trash->ageCount(), 3);
    PB_CHECK(!e.trash->canAct());
    PB_CHECK(e.trash->offline());
    PB_CHECK_EQ(e.message_key(), std::string("no_network"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    PB_CHECK(e.trash->choose(0).value("ok").toBool());   // the item can be looked at; what needs the server stays off
}

PB_TEST(a_lost_connection_while_reading_keeps_the_stored_list_in_view) {
    Env e;
    e.server.add_trash_file("/a.txt", "x", e.t(10));
    e.open_and_wait();
    e.server.set_offline(true);   // the Wi-Fi flag says online, the server cannot be reached
    e.trash->refresh();
    e.wait();
    PB_CHECK_EQ(e.state(), std::string("listed"));
    PB_CHECK(e.trash->fromStore());
    PB_CHECK_EQ(e.trash->total(), 1);
    PB_CHECK_EQ(e.message_key(), std::string("server_unreachable"));
    PB_CHECK(!e.trash->canAct());
}

PB_TEST(a_list_that_fails_for_another_reason_says_so_with_its_code) {
    Env e;
    e.server.add_trash_file("/a.txt", "x", e.t(10));
    e.server.answer_next("PROPFIND", "/trashbin/", 500);
    e.open_and_wait();
    PB_CHECK_EQ(e.state(), std::string("explainUnknown"));
    PB_CHECK_EQ(e.message_key(), std::string("server_error"));
    PB_CHECK_EQ(e.trash->message().value("code").toString().toStdString(), std::string("T2-500"));
    PB_CHECK(e.trash->rows().isEmpty() && !e.trash->canAct());
}

PB_TEST(nothing_is_stored_for_an_account_that_is_gone_and_no_list_is_read_without_a_tap) {
    Env e;
    e.server.add_trash_file("/a.txt", "x", e.t(10));
    e.open_and_wait();
    PB_CHECK(e.store->clear_account().ok());   // signing out drops the stored list
    PB_CHECK(e.store->load_trash(1).value.items.empty());
    e.server.clear_log();
    QTest::qWait(100);
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

// ---- Phase 5 (US2): restore ----

PB_TEST(a_free_original_place_is_proposed_probed_and_after_the_confirmation_the_file_is_back_and_the_item_gone) {
    Env e;
    const std::string name = e.story();
    // a local copy that was shown as "removed on the server", and the cached listing of the folder
    pbc::LocalFile lf;
    lf.remote_path = "/Books/story.epub";
    lf.local_path = "Books/story.epub";
    lf.size = 9;
    PB_CHECK(e.store->commit_download(lf, 0).ok());
    PB_CHECK(e.store->set_server_missing("/Books/story.epub", true).ok());
    PB_CHECK(e.store->folder("/Books").has_value());
    QSignalSpy plan(e.trash.get(), &TrashBridge::restorePlan);
    PB_CHECK(e.trash->restore().value("ok").toBool());
    PB_CHECK(e.wait_for(plan));
    PB_CHECK_EQ(plan.at(0).at(0).toString().toStdString(), std::string("original_free"));
    const QVariantMap proposal = plan.at(0).at(1).toMap();
    PB_CHECK_EQ(proposal.value("place").toString().toStdString(), std::string("/Books"));
    PB_CHECK(proposal.value("originalPlace").toBool() && !proposal.value("destructive").toBool() && proposal.value("defaultCancel").toBool());
    PB_CHECK_EQ(proposal.value("names").toStringList().value(0).toStdString(), std::string("story.epub"));
    PB_CHECK_EQ(e.moves(), size_t(0));   // nothing is moved before the confirmation
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    PB_CHECK(e.trash->restoreConfirmed().value("ok").toBool());
    PB_CHECK(e.wait_for(done));
    const QVariantMap result = done.at(0).at(0).toMap();
    PB_CHECK(result.value("ok").toBool());
    PB_CHECK_EQ(e.moves(), size_t(1));
    PB_CHECK_EQ(e.server.content_of("/Books/story.epub"), std::string("the story"));
    PB_CHECK_EQ(e.trash->total(), 0);
    PB_CHECK(e.trash->chosenName().isEmpty());
    PB_CHECK(e.store->load_trash(1).value.items.empty());                          // the stored list follows
    PB_CHECK(!e.store->folder("/Books").has_value());                              // the next visit of the folder fetches it
    PB_CHECK(!e.store->local_file("/Books/story.epub")->server_missing);           // the local copy is itself again
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM queue_items"), std::string("0"));   // nothing was queued
    PB_CHECK(!e.trash->busy());
    (void)name;
}

PB_TEST(a_taken_name_sends_the_user_to_another_folder_and_a_free_name_there_is_confirmed_and_restored) {
    Env e;
    e.server.add_folder("/Books");
    e.server.add_folder("/Other");
    e.server.add_file("/Books/story.epub", "the current one");
    e.server.add_trash_file("/Books/story.epub", "the old one", e.t(100));
    e.cache("/Books");
    e.open_and_wait();
    PB_CHECK(e.trash->choose(0).value("ok").toBool());
    e.server.clear_log();
    QSignalSpy plan(e.trash.get(), &TrashBridge::restorePlan);
    e.trash->restore();
    PB_CHECK(e.wait_for(plan));
    PB_CHECK_EQ(plan.at(0).at(0).toString().toStdString(), std::string("name_taken"));
    PB_CHECK_EQ(plan.at(0).at(1).toMap().value("folder").toString().toStdString(), std::string("/Books"));
    PB_CHECK_EQ(e.moves(), size_t(0));
    QSignalSpy checked(e.trash.get(), &TrashBridge::targetChecked);
    PB_CHECK(e.trash->chooseTarget("/Other").value("ok").toBool());
    PB_CHECK(e.wait_for(checked));
    const QVariantMap c = checked.at(0).at(0).toMap();
    PB_CHECK(c.value("needsConfirm").toBool());
    PB_CHECK_EQ(c.value("summary").toMap().value("place").toString().toStdString(), std::string("/Other"));
    PB_CHECK(!c.value("summary").toMap().value("originalPlace").toBool());
    PB_CHECK_EQ(e.moves(), size_t(0));
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    PB_CHECK(e.trash->restoreChosenConfirmed().value("ok").toBool());
    PB_CHECK(e.wait_for(done));
    PB_CHECK(done.at(0).at(0).toMap().value("ok").toBool());
    PB_CHECK_EQ(e.server.content_of("/Other/story.epub"), std::string("the old one"));
    PB_CHECK_EQ(e.server.content_of("/Books/story.epub"), std::string("the current one"));   // the existing file was never touched
    const auto& m = e.server.log().back();
    (void)m;
    bool found = false;
    for (const auto& l : e.server.log())
        if (l.method == "MOVE") {
            found = true;
            PB_CHECK_EQ(l.overwrite, std::string("F"));
            PB_CHECK(l.destination.find("/dav/files/demo/Other/story.epub") != std::string::npos);
        }
    PB_CHECK(found);
}

PB_TEST(a_taken_name_in_the_chosen_folder_asks_for_a_new_name_and_offers_no_replace) {
    Env e;
    e.server.add_folder("/Books");
    e.server.add_file("/Books/story.epub", "the current one");
    e.server.add_trash_file("/Books/story.epub", "the old one", e.t(100));
    e.cache("/Books");
    e.open_and_wait();
    e.trash->choose(0);
    QSignalSpy plan(e.trash.get(), &TrashBridge::restorePlan);
    e.trash->restore();   // the original place is taken: the user is sent to the chooser
    PB_CHECK(e.wait_for(plan));
    QSignalSpy checked(e.trash.get(), &TrashBridge::targetChecked);
    e.trash->chooseTarget("/Books");   // the same folder, the same name: taken
    PB_CHECK(e.wait_for(checked));
    const QVariantMap c = checked.at(0).at(0).toMap();
    PB_CHECK(c.value("needsName").toBool());
    PB_CHECK_EQ(c.value("name").toString().toStdString(), std::string("story (2).epub"));   // a free name to start from, never the taken one
    // a bad name is refused at once, nothing is sent
    e.server.clear_log();
    for (const char* bad : {"", "a/b", "..", "x\\y"}) PB_CHECK_EQ(e.err(e.trash->setTargetName(bad)), std::string("name_forbidden"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    // a free name is probed and confirmed
    checked.clear();
    PB_CHECK(e.trash->setTargetName("story (2).epub").value("ok").toBool());
    PB_CHECK(e.wait_for(checked));
    PB_CHECK(checked.at(0).at(0).toMap().value("needsConfirm").toBool());
    // the taken name again: the probe says so again
    checked.clear();
    e.trash->setTargetName("story.epub");
    PB_CHECK(e.wait_for(checked));
    PB_CHECK(checked.at(0).at(0).toMap().value("needsName").toBool());
    // there is no way to replace: the bridge has no such member
    const QMetaObject* mo = e.trash->metaObject();
    for (int i = 0; i < mo->methodCount(); ++i) {
        const QString method = QString::fromLatin1(mo->method(i).name()).toLower();
        PB_CHECK(!method.contains("replace") && !method.contains("overwrite"));
    }
    PB_CHECK_EQ(e.moves(), size_t(0));
}

PB_TEST(a_missing_original_folder_is_a_plan_and_no_folder_is_created) {
    Env e;
    e.server.add_trash_file("/Nowhere/x.txt", "x", e.t(100));
    e.open_and_wait();
    e.trash->choose(0);
    e.server.clear_log();
    QSignalSpy plan(e.trash.get(), &TrashBridge::restorePlan);
    e.trash->restore();
    PB_CHECK(e.wait_for(plan));
    PB_CHECK_EQ(plan.at(0).at(0).toString().toStdString(), std::string("parent_missing"));
    PB_CHECK_EQ(e.moves(), size_t(0));
    PB_CHECK(!e.server.has("/Nowhere"));
    PB_CHECK_EQ(e.server.count("MKCOL"), size_t(0));
}

PB_TEST(without_a_chosen_item_or_a_confirmed_plan_nothing_happens) {
    Env e;
    e.server.add_folder("/Books");
    e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    e.open_and_wait();
    PB_CHECK_EQ(e.err(e.trash->restore()), std::string("trash_item_gone"));   // nothing is chosen
    PB_CHECK_EQ(e.err(e.trash->restoreConfirmed()), std::string("nothing_pending"));
    PB_CHECK_EQ(e.err(e.trash->restoreChosenConfirmed()), std::string("nothing_pending"));
    PB_CHECK_EQ(e.err(e.trash->chooseTarget("/Books")), std::string("nothing_pending"));
    PB_CHECK_EQ(e.err(e.trash->setTargetName("x")), std::string("nothing_pending"));
    e.trash->choose(0);
    e.server.clear_log();
    QSignalSpy plan(e.trash.get(), &TrashBridge::restorePlan);
    e.trash->restore();
    PB_CHECK(e.wait_for(plan));
    e.trash->cancelStep();   // the user left the screen after the proposal
    PB_CHECK_EQ(e.err(e.trash->restoreConfirmed()), std::string("nothing_pending"));
    PB_CHECK_EQ(e.moves(), size_t(0));
}

PB_TEST(a_restore_needs_the_connection_and_the_trash_app) {
    Env e;
    e.server.add_folder("/Books");
    e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    e.open_and_wait();
    e.trash->choose(0);
    e.wifi = false;
    e.server.clear_log();
    PB_CHECK_EQ(e.err(e.trash->restore()), std::string("no_network"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
    e.wifi = true;
    e.server.set_trash(false);
    e.trash->open();
    e.wait();
    PB_CHECK_EQ(e.state(), std::string("explainOff"));
    PB_CHECK_EQ(e.err(e.trash->restore()), std::string("trash_unavailable"));
}

PB_TEST(a_second_confirmation_while_the_request_runs_is_refused_and_one_move_is_sent) {
    Env e;
    e.story();
    QSignalSpy plan(e.trash.get(), &TrashBridge::restorePlan);
    e.trash->restore();
    PB_CHECK(e.wait_for(plan));
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    PB_CHECK(e.trash->restoreConfirmed().value("ok").toBool());
    PB_CHECK_EQ(e.err(e.trash->restoreConfirmed()), std::string("busy"));
    PB_CHECK(e.wait_for(done));
    PB_CHECK_EQ(e.moves(), size_t(1));
}

PB_TEST(a_refused_restore_keeps_the_item_and_says_why_without_a_name) {
    Env e;
    e.story();
    QSignalSpy plan(e.trash.get(), &TrashBridge::restorePlan);
    e.trash->restore();
    PB_CHECK(e.wait_for(plan));
    e.server.answer_next("MOVE", "/trashbin/", 507);
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    e.trash->restoreConfirmed();
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK(!r.value("ok").toBool());
    PB_CHECK_EQ(r.value("key").toString().toStdString(), std::string("server_storage_full"));
    PB_CHECK_EQ(r.value("code").toString().toStdString(), std::string("T4-507"));
    PB_CHECK_EQ(e.trash->total(), 1);   // the item stays
    PB_CHECK_EQ(e.store->load_trash(1).value.items.size(), size_t(1));
    PB_CHECK(!mentions(QVariantList{r}, QStringLiteral("story")) && !mentions(QVariantList{r}, QStringLiteral("Books")));
    PB_CHECK_EQ(e.trash->result().value("key").toString().toStdString(), std::string("server_storage_full"));
    PB_CHECK(!e.server.has("/Books/story.epub"));
}

PB_TEST(a_restore_whose_answer_was_lost_is_unknown_marked_and_not_repeated_and_a_refresh_shows_the_truth) {
    Env e;
    e.story();
    QSignalSpy plan(e.trash.get(), &TrashBridge::restorePlan);
    e.trash->restore();
    PB_CHECK(e.wait_for(plan));
    e.server.lose_next_answer("MOVE", "/trashbin/");
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    e.trash->restoreConfirmed();
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK(r.value("outcomeUnknown").toBool());
    PB_CHECK_EQ(r.value("key").toString().toStdString(), std::string("outcome_unknown"));
    PB_CHECK(e.row(0).value("unknown").toBool());   // the item stays in the list, marked
    PB_CHECK_EQ(e.moves(), size_t(1));
    e.trash->refresh();
    e.wait();
    PB_CHECK_EQ(e.trash->total(), 0);   // the server had done it: the refresh shows the truth, and the mark is gone with the item
    PB_CHECK_EQ(e.moves(), size_t(1));
}

PB_TEST(an_item_that_left_the_trash_elsewhere_leaves_the_list_with_trash_item_gone) {
    Env e;
    const std::string name = e.story();
    QSignalSpy plan(e.trash.get(), &TrashBridge::restorePlan);
    e.trash->restore();
    PB_CHECK(e.wait_for(plan));
    pbc::Request del;
    del.method = "DELETE";
    del.url = e.server.base_url() + "/remote.php/dav/trashbin/demo/trash/" + name;
    del.username = "demo";
    del.password = e.server.app_password();
    PB_CHECK_EQ(e.server.perform(del).status, 204L);
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    e.trash->restoreConfirmed();
    PB_CHECK(e.wait_for(done));
    PB_CHECK_EQ(done.at(0).at(0).toMap().value("key").toString().toStdString(), std::string("trash_item_gone"));
    PB_CHECK_EQ(e.trash->total(), 0);
    PB_CHECK(e.store->load_trash(1).value.items.empty());
}

PB_TEST(a_restore_to_a_chosen_place_that_the_server_cannot_do_says_so_and_changes_nothing) {
    Env e;
    e.server.add_folder("/Other");
    e.server.set_restore_to_files_status(403);
    e.server.add_trash_file("/Books/a.txt", "a", e.t(100));
    e.open_and_wait();
    e.trash->choose(0);
    QSignalSpy plan(e.trash.get(), &TrashBridge::restorePlan);
    e.trash->restore();
    PB_CHECK(e.wait_for(plan));
    QSignalSpy checked(e.trash.get(), &TrashBridge::targetChecked);
    e.trash->chooseTarget("/Other");
    PB_CHECK(e.wait_for(checked));
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    e.trash->restoreChosenConfirmed();
    PB_CHECK(e.wait_for(done));
    PB_CHECK_EQ(done.at(0).at(0).toMap().value("key").toString().toStdString(), std::string("restore_place_unsupported"));
    PB_CHECK(!e.server.has("/Other/a.txt"));
    PB_CHECK_EQ(e.trash->total(), 1);
}

// ---- Phase 7 (US4): delete for good, only knowingly ----

namespace {

// the trash with `files` files and `folders` folders, opened, nothing chosen
void fill(Env& e, int files, int folders = 0) {
    e.server.add_folder("/F");
    for (int i = 0; i < files; ++i) e.server.add_trash_file("/F/f" + std::to_string(i) + ".txt", "x", e.t(10 + i));
    for (int i = 0; i < folders; ++i) e.server.add_trash_folder("/F/d" + std::to_string(i), e.t(5000 + i), {{"/in.txt", "i"}});
    e.open_and_wait();
}

size_t deletes(Env& e) { return e.server.count("DELETE"); }

}  // namespace

PB_TEST(with_the_irreversible_actions_switched_off_nothing_can_start_and_nothing_is_sent) {
    Env e;   // delete_enabled false: the build of the product before the constitution allowed it, or a test of the switch
    fill(e, 2);
    PB_CHECK(!e.trash->deleteEnabled());
    e.trash->choose(0);
    e.server.clear_log();
    PB_CHECK_EQ(e.err(e.trash->beginDelete()), std::string("not_available"));
    PB_CHECK_EQ(e.err(e.trash->beginEmpty()), std::string("not_available"));
    PB_CHECK_EQ(e.err(e.trash->confirmStep()), std::string("nothing_pending"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(a_file_is_deleted_for_good_after_one_confirmation_that_names_it_and_only_that_item_goes) {
    Env e(false, true);
    fill(e, 3);
    PB_CHECK(e.trash->deleteEnabled());
    PB_CHECK(e.trash->choose(1).value("ok").toBool());
    const std::string chosen_name = e.trash->chosenName().toStdString();
    e.server.clear_log();
    const QVariantMap c = e.trash->beginDelete();
    PB_CHECK_EQ(c.value("kind").toString().toStdString(), std::string("delete_file"));
    PB_CHECK_EQ(c.value("names").toStringList().value(0).toStdString(), chosen_name);
    PB_CHECK_EQ(c.value("stage").toInt(), 1);
    PB_CHECK(c.value("destructive").toBool() && c.value("defaultCancel").toBool());
    PB_CHECK_EQ(deletes(e), size_t(0));   // nothing is deleted before the confirmation
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    PB_CHECK(e.trash->confirmStep().value("started").toBool());
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK(r.value("ok").toBool());
    PB_CHECK_EQ(r.value("kind").toString().toStdString(), std::string("delete"));
    PB_CHECK_EQ(r.value("removed").toInt(), 1);
    PB_CHECK_EQ(deletes(e), size_t(1));
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(2));
    PB_CHECK_EQ(e.trash->total(), 2);
    PB_CHECK(e.trash->chosenName().isEmpty());
    PB_CHECK_EQ(e.store->load_trash(1).value.items.size(), size_t(2));
    PB_CHECK_EQ(e.err(e.trash->confirmStep()), std::string("nothing_pending"));   // one confirmation, one request
    PB_CHECK(!mentions(QVariantList{r}, QStringLiteral("f1")));
}

PB_TEST(a_folder_needs_two_confirmations_and_is_removed_only_after_the_second) {
    Env e(false, true);
    fill(e, 0, 2);
    e.trash->choose(0);
    e.server.clear_log();
    const QVariantMap first = e.trash->beginDelete();
    PB_CHECK_EQ(first.value("kind").toString().toStdString(), std::string("delete_folder"));
    PB_CHECK_EQ(first.value("stage").toInt(), 1);
    const QVariantMap second = e.trash->confirmStep();   // the first confirmation: the second one is next, nothing is sent
    PB_CHECK_EQ(second.value("kind").toString().toStdString(), std::string("delete_folder"));
    PB_CHECK_EQ(second.value("stage").toInt(), 2);
    PB_CHECK(second.value("destructive").toBool() && second.value("defaultCancel").toBool());
    PB_CHECK_EQ(second.value("names").toStringList().value(0).toStdString(), first.value("names").toStringList().value(0).toStdString());
    PB_CHECK_EQ(deletes(e), size_t(0));
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    PB_CHECK(e.trash->confirmStep().value("started").toBool());   // the second confirmation
    PB_CHECK(e.wait_for(done));
    PB_CHECK(done.at(0).at(0).toMap().value("ok").toBool());
    PB_CHECK_EQ(deletes(e), size_t(1));
    PB_CHECK_EQ(e.trash->total(), 1);
}

PB_TEST(cancel_at_the_second_confirmation_deletes_nothing) {
    Env e(false, true);
    fill(e, 0, 1);
    e.trash->choose(0);
    e.server.clear_log();
    e.trash->beginDelete();
    PB_CHECK_EQ(e.trash->confirmStep().value("stage").toInt(), 2);
    e.trash->cancelStep();
    PB_CHECK_EQ(e.err(e.trash->confirmStep()), std::string("nothing_pending"));
    PB_CHECK_EQ(deletes(e), size_t(0));
    PB_CHECK_EQ(e.trash->total(), 1);
}

PB_TEST(emptying_the_trash_says_the_count_asks_twice_and_removes_exactly_the_named_items) {
    Env e(false, true);
    fill(e, 7, 2);
    e.server.clear_log();
    const QVariantMap first = e.trash->beginEmpty();
    PB_CHECK_EQ(first.value("kind").toString().toStdString(), std::string("empty_trash"));
    PB_CHECK_EQ(first.value("items").toInt(), 9);
    PB_CHECK_EQ(first.value("folders").toInt(), 2);
    PB_CHECK_EQ(first.value("names").toStringList().size(), 5);
    PB_CHECK_EQ(first.value("stage").toInt(), 1);
    PB_CHECK(first.value("destructive").toBool() && first.value("defaultCancel").toBool());
    const QVariantMap second = e.trash->confirmStep();
    PB_CHECK_EQ(second.value("stage").toInt(), 2);
    PB_CHECK_EQ(second.value("items").toInt(), 9);
    PB_CHECK_EQ(deletes(e), size_t(0));
    // an item that arrives while the run goes on is not in the named set and stays
    e.server.before_next("DELETE", "/trashbin/", [&] { e.server.add_trash_file("/F/late.txt", "late", e.t(1)); }, 2);
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    PB_CHECK(e.trash->confirmStep().value("started").toBool());
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK_EQ(r.value("kind").toString().toStdString(), std::string("empty"));
    PB_CHECK_EQ(r.value("removed").toInt(), 9);
    PB_CHECK_EQ(r.value("notRemoved").toInt(), 0);
    PB_CHECK_EQ(r.value("notTried").toInt(), 0);
    PB_CHECK_EQ(deletes(e), size_t(9));
    PB_CHECK_EQ(e.server.trashbin_count(), size_t(1));   // the late one
    PB_CHECK_EQ(e.trash->total(), 0);                    // the list shows what was removed; the late item shows after the next refresh
    PB_CHECK(e.store->load_trash(1).value.items.empty());
    const QVariantMap p = e.trash->progress();
    PB_CHECK_EQ(p.value("done").toInt(), 9);
    PB_CHECK_EQ(p.value("total").toInt(), 9);
}

PB_TEST(a_confirmation_does_not_survive_cancel_a_refresh_leaving_the_screen_or_an_account_change) {
    Env e(false, true);
    fill(e, 3);
    e.trash->choose(0);
    e.server.clear_log();
    e.trash->beginDelete();
    e.trash->cancelStep();
    PB_CHECK_EQ(e.err(e.trash->confirmStep()), std::string("nothing_pending"));
    e.trash->beginEmpty();
    e.trash->refresh();   // a new look at the trash: what was named may be out of date
    e.wait();
    PB_CHECK_EQ(e.err(e.trash->confirmStep()), std::string("nothing_pending"));
    e.trash->beginEmpty();
    e.trash->onApplicationStateChanged(Qt::ApplicationInactive);   // the app is switched away
    PB_CHECK_EQ(e.err(e.trash->confirmStep()), std::string("nothing_pending"));
    e.trash->beginEmpty();
    e.trash->onConnectionChanged();   // the account changed
    PB_CHECK_EQ(e.err(e.trash->confirmStep()), std::string("nothing_pending"));
    e.trash->beginEmpty();
    e.trash->open();   // the Trash tap again
    e.wait();
    PB_CHECK_EQ(e.err(e.trash->confirmStep()), std::string("nothing_pending"));
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(0));
    PB_CHECK_EQ(e.trash->total(), 3);
}

PB_TEST(a_second_confirmation_while_the_request_runs_does_nothing_and_one_delete_is_sent) {
    Env e(false, true);
    fill(e, 2);
    e.trash->choose(0);
    e.server.clear_log();
    e.trash->beginDelete();
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    PB_CHECK(e.trash->confirmStep().value("started").toBool());
    PB_CHECK_EQ(e.err(e.trash->confirmStep()), std::string("nothing_pending"));   // one confirmation chain, one run: the chain is used up
    PB_CHECK_EQ(e.err(e.trash->beginDelete()), std::string("busy"));              // and a new one cannot begin while the request runs
    PB_CHECK(e.wait_for(done));
    PB_CHECK_EQ(deletes(e), size_t(1));
}

PB_TEST(the_irreversible_actions_need_the_connection_and_a_chosen_item) {
    Env e(false, true);
    fill(e, 2);
    PB_CHECK_EQ(e.err(e.trash->beginDelete()), std::string("trash_item_gone"));   // nothing is chosen
    e.trash->choose(0);
    e.wifi = false;
    e.server.clear_log();
    PB_CHECK_EQ(e.err(e.trash->beginDelete()), std::string("no_network"));
    PB_CHECK_EQ(e.err(e.trash->beginEmpty()), std::string("no_network"));
    PB_CHECK_EQ(e.server.request_count(), size_t(0));
}

PB_TEST(with_the_trash_app_off_there_is_nothing_to_delete) {
    Env e(false, true);
    e.server.set_trash(false);
    e.trash->open();
    e.wait();
    PB_CHECK_EQ(e.err(e.trash->beginEmpty()), std::string("trash_unavailable"));
    PB_CHECK_EQ(e.err(e.trash->beginDelete()), std::string("trash_unavailable"));
}

PB_TEST(items_that_cannot_be_removed_are_counted_and_stay_in_the_list) {
    Env e(false, true);
    fill(e, 5);
    const std::string locked = e.server.trash_names()[2];
    e.server.lock_trash_item(locked);
    e.trash->beginEmpty();
    e.trash->confirmStep();
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    e.trash->confirmStep();
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK_EQ(r.value("removed").toInt(), 4);
    PB_CHECK_EQ(r.value("notRemoved").toInt(), 1);
    PB_CHECK_EQ(e.trash->total(), 1);
    PB_CHECK(e.server.trashbin_has(locked));
    PB_CHECK(!mentions(QVariantList{r}, QStringLiteral("f2")));
}

PB_TEST(a_refused_login_stops_the_run_and_says_how_many_were_not_tried) {
    Env e(false, true);
    fill(e, 10);
    e.server.before_next("DELETE", "/trashbin/", [&] { e.server.answer_next("DELETE", "/trashbin/", 401, 100); }, 3);
    e.trash->beginEmpty();
    e.trash->confirmStep();
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    e.trash->confirmStep();
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK_EQ(r.value("key").toString().toStdString(), std::string("auth_failed"));
    PB_CHECK_EQ(r.value("removed").toInt(), 3);
    PB_CHECK_EQ(r.value("notTried").toInt(), 6);
    PB_CHECK_EQ(e.trash->total(), 7);
}

PB_TEST(a_lost_answer_marks_one_item_unknown_stops_and_is_never_repeated) {
    Env e(false, true);
    fill(e, 6);
    e.server.before_next("DELETE", "/trashbin/", [&] { e.server.lose_next_answer("DELETE", "/trashbin/"); }, 2);
    e.trash->beginEmpty();
    e.trash->confirmStep();
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    e.trash->confirmStep();
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK(r.value("outcomeUnknown").toBool());
    PB_CHECK_EQ(r.value("removed").toInt(), 2);
    PB_CHECK_EQ(r.value("notTried").toInt(), 3);
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(3));
    bool marked = false;
    for (int i = 0; i < e.trash->total(); ++i) {
        e.trash->setPageSize(100);
        marked = marked || e.row(i).value("unknown").toBool();
    }
    PB_CHECK(marked);   // the item of the lost answer stays in the list, marked, until a refresh shows the truth
    e.trash->refresh();
    e.wait();
    PB_CHECK_EQ(e.trash->total(), 3);   // the server had done the third too
}

PB_TEST(cancel_stops_the_run_between_two_requests) {
    Env e(false, true);
    fill(e, 8);
    e.server.before_next("DELETE", "/trashbin/", [&] { e.trash->cancelRun(); }, 2);   // the user taps Cancel while the third request is being made
    e.trash->beginEmpty();
    e.trash->confirmStep();
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    e.trash->confirmStep();
    PB_CHECK(e.wait_for(done));
    const QVariantMap r = done.at(0).at(0).toMap();
    PB_CHECK_EQ(r.value("removed").toInt(), 3);   // the request in flight finishes, none follows
    PB_CHECK_EQ(r.value("notTried").toInt(), 5);
    PB_CHECK(r.value("cancelled").toBool());
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(3));
}

PB_TEST(the_progress_of_a_long_run_is_reported_coarsely) {
    Env e(false, true);
    fill(e, 60);
    e.trash->setPageSize(100);
    e.trash->beginEmpty();
    e.trash->confirmStep();
    QSignalSpy changed(e.trash.get(), &TrashBridge::changed);
    QSignalSpy done(e.trash.get(), &TrashBridge::actionFinished);
    e.trash->confirmStep();
    PB_CHECK(e.wait_for(done));
    QTest::qWait(30);
    PB_CHECK(changed.count() <= 30);   // at most about every tenth item, never one screen update per item (60 deletes)
    PB_CHECK_EQ(e.trash->progress().value("done").toInt(), 60);
}

PB_TEST(an_irreversible_action_is_never_made_by_anything_but_a_confirmed_step) {
    Env e(false, true);
    fill(e, 2);
    e.trash->choose(0);
    e.server.clear_log();
    e.trash->beginDelete();
    e.trash->beginEmpty();
    e.trash->refresh();
    e.wait();
    e.trash->open();
    e.wait();
    e.trash->restore();   // a restore is not a delete
    QTest::qWait(100);
    PB_CHECK_EQ(e.server.count("DELETE"), size_t(0));
}

PB_TEST_MAIN()
