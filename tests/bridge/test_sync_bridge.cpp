// The SyncBridge, the pair part (specs/003-folder-sync, contracts/bridge-qml.md): the pair list and adding, changing and removing a pair. Real core,
// real temporary folders, the fake Nextcloud only to prove that none of this makes a request. Saving a pair writes its row and nothing else (FR-003).
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QVariantMap>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "fake_nextcloud_server.h"
#include "fake_storage.h"
#include "pbc/credentials_file.h"
#include "pbc/folder_service.h"
#include "pbc/store.h"
#include "sync_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "worker.h"

PBC_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_sync_bridge";
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

// every path below `dir` with its size, sorted: what a folder holds
void walk(const std::string& dir, const std::string& rel, std::vector<std::string>* out) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        struct stat st;
        const std::string p = dir + "/" + n;
        if (::lstat(p.c_str(), &st) != 0) continue;
        out->push_back(rel + "/" + n + (S_ISDIR(st.st_mode) ? "/" : ":" + std::to_string(st.st_size)));
        if (S_ISDIR(st.st_mode)) walk(p, rel + "/" + n, out);
    }
    ::closedir(d);
}
std::vector<std::string> tree_of(const std::string& dir) {
    std::vector<std::string> out;
    walk(dir, "", &out);
    std::sort(out.begin(), out.end());
    return out;
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
    int quiet_ms = 600000;    // Auto Sync's quiet period: long, so that only the tests of Auto Sync ever see it end
    pbtest::FakeStorage storage;
    std::function<void(const char*)> on_step;   // a test acts at a step of the runner
    std::string root, download;
    std::unique_ptr<SyncBridge> sync;

    Env() {
        root = dir.file("root");
        download = root + "/PBCloud";
        for (const char* d : {"Books", "Books/Sub", "Outbox", "Other", "system/pbcloud", "PBCloud", "PBCloud/Books"}) mkdirs(root + "/" + d);
        touch(root + "/Books/a.epub", "alpha");
        touch(root + "/Outbox/note.txt", "note");
        auto r = pbc::Store::open(dir.file("data"), clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
        store = std::move(r.value);
        account("demo");
        pbc::Credentials cr;
        cr.password = server.app_password();
        PB_CHECK(pbc::save_credentials(store->directory(), cr).ok());
        server.add_folder("/Books");
        server.add_file("/Books/a.epub", "alpha");
        server.add_folder("/Outbox");
        server.add_folder("/Other");
        build();
        server.clear_log();
    }
    void account(const std::string& user) {
        pbc::AccountRow a;
        a.base_url = server.base_url();
        a.login_name = user;
        a.user_id = user;
        a.state = "ok";
        PB_CHECK(store->save_account(a).ok());
    }
    void build() {
        sync.reset();
        SyncBridge::Config c;
        c.store = store.get();
        c.worker = &worker;
        c.http = &server;
        c.clock = &clock;
        c.rules.root = root;
        c.rules.deny = {root + "/system/pbcloud"};
        c.download_root = download;
        c.wifi_up = [this] { return wifi; };
        c.ensure_network = [this](std::function<void(bool)> done) {
            ++ensure_calls;
            done(net_ok);
        };
        c.format_date_time = [](qint64 t) { return QString::number(t); };
        c.storage = &storage;
        c.downloader_options.delays_s = {1};
        c.downloader_options.sleep_ms = [](int) {};
        c.runner_hook = [this](const char* step) {
            if (on_step) on_step(step);
        };
        c.progress_interval_ms = 0;
        c.auto_sync_quiet_ms = quiet_ms;
        sync = std::make_unique<SyncBridge>(c);
    }
    ~Env() {
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    void settle() {
        QElapsedTimer t;
        t.start();
        while (sync->busy() && t.elapsed() < 20000) QTest::qWait(5);
        if (sync->busy()) PB_FAIL_AT("the bridge stayed busy");
        worker.wait_idle();
        QCoreApplication::processEvents();
    }
    // a pair for /Books and the reader folder Books (created above), downloads one file
    int64_t books_pair() {
        ::unlink((root + "/Books/a.epub").c_str());   // the sample file both sides start with would be a clash, not a download
        server.remove("/Books/a.epub");
        const QVariantMap r = sync->addPair(spec("Books", "/Books"));
        PB_CHECK(r.value("ok").toBool());
        return r.value("id").toLongLong();
    }
    QVariantMap spec(const std::string& reader, const std::string& remote, const char* direction = nullptr) const {
        QVariantMap m;
        m["readerFolder"] = QString::fromStdString(root + "/" + reader);
        m["remoteFolder"] = QString::fromStdString(remote);
        if (direction) m["direction"] = QString::fromLatin1(direction);
        return m;
    }
    QVariantMap pair(int index = 0) const { return sync->pairs().at(index).toMap(); }
    std::vector<std::string> disk() const { return tree_of(root); }
    size_t requests() const { return server.request_count(); }
};

std::string key(const QVariantMap& m) { return m.value("errorKey").toString().toStdString(); }

}  // namespace

PB_TEST(adding_a_pair_stores_it_and_changes_nothing_on_either_side_and_makes_no_request) {
    Env e;
    const auto disk_before = e.disk();
    const std::string server_before = e.server.etag_of("/");
    QSignalSpy changed(e.sync.get(), &SyncBridge::changed);
    PB_CHECK_EQ(e.sync->pairCount(), 0);
    PB_CHECK(e.sync->pairs().isEmpty());
    const QVariantMap r = e.sync->addPair(e.spec("Books", "/Books"));
    PB_CHECK(r.value("ok").toBool());
    PB_CHECK(r.value("id").toLongLong() > 0);
    PB_CHECK(changed.count() >= 1);
    PB_CHECK_EQ(e.sync->pairCount(), 1);
    PB_CHECK_EQ(e.requests(), size_t(0));
    PB_CHECK(e.disk() == disk_before);
    PB_CHECK_EQ(e.server.etag_of("/"), server_before);
    // the defaults of a new pair
    const QVariantMap p = e.pair();
    PB_CHECK_EQ(p.value("direction").toString().toStdString(), std::string("down"));
    PB_CHECK_EQ(p.value("maxFiles").toLongLong(), 500LL);
    PB_CHECK_EQ(p.value("maxBytes").toLongLong(), 2LL << 30);
    PB_CHECK(!p.value("startupSync").toBool());
    PB_CHECK(!p.value("autoSync").toBool());
    PB_CHECK_EQ(p.value("state").toString().toStdString(), std::string("never"));
}

PB_TEST(a_sending_direction_needs_the_confirmation_and_stores_nothing_without_it) {
    Env e;
    for (const char* dir : {"up", "both"}) {
        const QVariantMap r = e.sync->addPair(e.spec("Outbox", "/Outbox", dir));
        PB_CHECK(!r.value("ok").toBool());
        PB_CHECK_EQ(key(r), std::string("confirmation_needed"));
        PB_CHECK_EQ(e.sync->pairCount(), 0);
    }
    QVariantMap confirmed = e.sync->addPair(e.spec("Outbox", "/Outbox", "up"));
    PB_CHECK_EQ(key(confirmed), std::string("confirmation_needed"));
    QVariantMap s = e.spec("Outbox", "/Outbox", "up");
    s["confirmed"] = true;
    PB_CHECK(e.sync->addPair(s).value("ok").toBool());
    PB_CHECK_EQ(e.pair().value("direction").toString().toStdString(), std::string("up"));
    PB_CHECK_EQ(e.requests(), size_t(0));
}

PB_TEST(a_change_of_the_direction_needs_the_confirmation_only_when_it_starts_sending) {
    Env e;
    const int64_t id = e.sync->addPair(e.spec("Books", "/Books")).value("id").toLongLong();
    const auto disk_before = e.disk();
    QVariantMap to_up;
    to_up["direction"] = "up";
    PB_CHECK_EQ(key(e.sync->updatePair(id, to_up)), std::string("confirmation_needed"));
    PB_CHECK_EQ(e.pair().value("direction").toString().toStdString(), std::string("down"));   // unchanged by the refusal
    to_up["confirmed"] = true;
    PB_CHECK(e.sync->updatePair(id, to_up).value("ok").toBool());
    PB_CHECK_EQ(e.pair().value("direction").toString().toStdString(), std::string("up"));
    QVariantMap to_both;
    to_both["direction"] = "both";   // from up to both: it was sending already
    PB_CHECK(e.sync->updatePair(id, to_both).value("ok").toBool());
    QVariantMap to_down;
    to_down["direction"] = "down";   // stopping to send needs nothing
    PB_CHECK(e.sync->updatePair(id, to_down).value("ok").toBool());
    PB_CHECK_EQ(e.pair().value("direction").toString().toStdString(), std::string("down"));
    PB_CHECK_EQ(key(e.sync->updatePair(id, to_both)), std::string("confirmation_needed"));   // sending again needs it again
    // a change takes effect at the next comparison: nothing is transferred, nothing deleted, no request
    PB_CHECK_EQ(e.requests(), size_t(0));
    PB_CHECK(e.disk() == disk_before);
    PB_CHECK(e.server.has("/Books/a.epub"));
}

PB_TEST(limits_and_switches_are_changed_in_place) {
    Env e;
    const int64_t id = e.sync->addPair(e.spec("Books", "/Books")).value("id").toLongLong();
    QVariantMap m;
    m["maxFiles"] = 40;
    m["maxBytes"] = 123456789LL;
    m["startupSync"] = true;
    m["autoSync"] = true;
    PB_CHECK(e.sync->updatePair(id, m).value("ok").toBool());
    const QVariantMap p = e.pair();
    PB_CHECK_EQ(p.value("maxFiles").toLongLong(), 40LL);
    PB_CHECK_EQ(p.value("maxBytes").toLongLong(), 123456789LL);
    PB_CHECK(p.value("startupSync").toBool());
    PB_CHECK(p.value("autoSync").toBool());
    QVariantMap bad;
    bad["maxFiles"] = 0;
    PB_CHECK_EQ(key(e.sync->updatePair(id, bad)), std::string("pair_limit_invalid"));
    PB_CHECK_EQ(e.pair().value("maxFiles").toLongLong(), 40LL);
    PB_CHECK_EQ(key(e.sync->updatePair(id + 99, m)), std::string("not_found"));
    PB_CHECK_EQ(e.requests(), size_t(0));
}

PB_TEST(removing_a_pair_removes_rows_only) {
    Env e;
    const int64_t id = e.sync->addPair(e.spec("Books", "/Books")).value("id").toLongLong();
    pbc::SyncRecord rec;
    rec.pair_id = id;
    rec.rel = "a.epub";
    rec.local_rel = "a.epub";
    rec.origin = "download";
    rec.synced_at = 1;
    PB_CHECK(e.store->sync_record_put(rec).ok());
    PB_CHECK(e.store->debug_exec("INSERT INTO queue_items(kind, entry_kind, server, user_id, created_at, updated_at, pair_id, sync_rel, sync_role) VALUES('upload', 'file', '" +
                                 e.server.base_url() + "', 'demo', 1, 1, " + std::to_string(id) + ", 'x', 'file')").empty());
    const auto disk_before = e.disk();
    const QVariantMap r = e.sync->removePair(id);
    PB_CHECK(r.value("ok").toBool());
    PB_CHECK_EQ(r.value("queueItemsKept").toInt(), 1);
    PB_CHECK_EQ(e.sync->pairCount(), 0);
    PB_CHECK(e.store->sync_records(id).empty());
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM queue_items"), std::string("1"));
    PB_CHECK(e.disk() == disk_before);          // downloaded files stay
    PB_CHECK(e.server.has("/Books/a.epub"));    // server files stay
    PB_CHECK_EQ(e.requests(), size_t(0));
    PB_CHECK(e.sync->removePair(id).value("ok").toBool());   // idempotent
}

PB_TEST(the_refusals_of_the_rules_come_back_as_keys_and_store_nothing) {
    Env e;
    PB_CHECK(e.sync->addPair(e.spec("Books", "/Books")).value("ok").toBool());
    PB_CHECK_EQ(key(e.sync->addPair(e.spec("Books/Sub", "/Else"))), std::string("pair_overlap"));
    PB_CHECK_EQ(key(e.sync->addPair(e.spec("Other", "/Books/Sub"))), std::string("pair_overlap"));
    PB_CHECK_EQ(key(e.sync->addPair(e.spec("PBCloud", "/Else"))), std::string("pair_folder_forbidden"));
    PB_CHECK_EQ(key(e.sync->addPair(e.spec("system", "/Else"))), std::string("pair_folder_forbidden"));
    QVariantMap outside;
    outside["readerFolder"] = "/etc";
    outside["remoteFolder"] = "/Else";
    PB_CHECK_EQ(key(e.sync->addPair(outside)), std::string("source_not_allowed"));
    QVariantMap limits = e.spec("Other", "/Other");
    limits["maxFiles"] = 99999;
    PB_CHECK_EQ(key(e.sync->addPair(limits)), std::string("pair_limit_invalid"));
    limits["maxFiles"] = 10;
    limits["maxBytes"] = 5;
    PB_CHECK_EQ(key(e.sync->addPair(limits)), std::string("pair_limit_invalid"));
    QVariantMap bad_dir = e.spec("Other", "/Other", "sideways");
    PB_CHECK_EQ(key(e.sync->addPair(bad_dir)), std::string("protocol_error"));
    PB_CHECK_EQ(e.sync->pairCount(), 1);
    PB_CHECK_EQ(e.requests(), size_t(0));
}

PB_TEST(a_ninth_pair_is_refused) {
    Env e;
    for (int i = 0; i < 8; ++i) {
        mkdirs(e.root + "/F" + std::to_string(i));
        PB_CHECK(e.sync->addPair(e.spec("F" + std::to_string(i), "/F" + std::to_string(i))).value("ok").toBool());
    }
    PB_CHECK_EQ(e.sync->pairLimit(), 8);
    PB_CHECK_EQ(key(e.sync->addPair(e.spec("Other", "/Other"))), std::string("pair_limit"));
    PB_CHECK_EQ(e.sync->pairCount(), 8);
}

PB_TEST(the_pair_list_carries_what_the_page_shows_with_names_made_safe) {
    Env e;
    QVariantMap s = e.spec("Books", "/Bo\xE2\x80\xAE" "oks");   // a right-to-left override in the server's folder name
    PB_CHECK(e.sync->addPair(s).value("ok").toBool());
    const QVariantMap p = e.pair();
    for (const char* k : {"id", "remoteFolder", "readerFolder", "readerPath", "remotePath", "direction", "directionText", "state", "stateText", "problemKey", "lastText",
                          "heldFiles", "heldBytes", "maxFiles", "maxBytes", "usageText", "planReady", "planOverLimit", "planCounts", "startupSync", "autoSync", "usable"})
        PB_CHECK(p.contains(k));
    PB_CHECK(!p.value("remoteFolder").toString().contains(QChar(0x202e)));   // displaySafe
    PB_CHECK(p.value("remotePath").toString().contains(QChar(0x202e)));      // the raw path is kept for editing
    PB_CHECK(p.value("usable").toBool());
    PB_CHECK_EQ(p.value("directionText").toString().toStdString(), std::string("Download only"));
    PB_CHECK_EQ(p.value("stateText").toString().toStdString(), std::string("Never synced"));
    PB_CHECK_EQ(p.value("lastText").toString().toStdString(), std::string(""));
    PB_CHECK(p.value("usageText").toString().contains("500"));
    PB_CHECK(!p.value("planReady").toBool());
    PB_CHECK(!p.value("planOverLimit").toBool());
    PB_CHECK_EQ(p.value("readerPath").toString().toStdString(), e.root + "/Books");
}

PB_TEST(describe_direction_gives_the_three_plain_sentences) {
    Env e;
    PB_CHECK_EQ(e.sync->describeDirection("down").toStdString(), std::string("Download only: files from the server are copied to the reader. Nothing on the reader is sent."));
    PB_CHECK_EQ(e.sync->describeDirection("up").toStdString(), std::string("Upload only: new files from the reader are sent to the server. Nothing from the server is copied."));
    PB_CHECK_EQ(e.sync->describeDirection("both").toStdString(), std::string("Two-way: new and changed files are copied in both directions. Nothing is ever deleted automatically."));
    PB_CHECK_EQ(e.sync->describeDirection("nonsense").toStdString(), std::string(""));
}

PB_TEST(a_pair_of_another_account_is_shown_as_such_and_is_not_usable) {
    Env e;
    PB_CHECK(e.sync->addPair(e.spec("Books", "/Books")).value("ok").toBool());
    PB_CHECK(e.pair().value("usable").toBool());
    e.account("someone");   // the app is connected to another account now
    e.sync->refresh();
    const QVariantMap p = e.pair();
    PB_CHECK(!p.value("usable").toBool());
    PB_CHECK_EQ(p.value("state").toString().toStdString(), std::string("account_changed"));
    PB_CHECK_EQ(p.value("stateText").toString().toStdString(), std::string("Other account"));
    PB_CHECK_EQ(p.value("problemKey").toString().toStdString(), std::string("account_changed"));
    PB_CHECK_EQ(e.sync->pairCount(), 1);   // never deleted
    PB_CHECK_EQ(e.requests(), size_t(0));
    // without any account
    PB_CHECK(e.store->clear_account().ok());
    e.sync->refresh();
    PB_CHECK(!e.pair().value("usable").toBool());
}

PB_TEST(the_offline_flag_follows_the_wifi_and_reading_it_makes_no_request) {
    Env e;
    PB_CHECK(!e.sync->offline());
    e.wifi = false;
    PB_CHECK(e.sync->offline());
    e.sync->refresh();
    PB_CHECK_EQ(e.requests(), size_t(0));
}

PB_TEST(adding_a_pair_without_an_account_is_refused) {
    Env e;
    PB_CHECK(e.store->clear_account().ok());
    const QVariantMap r = e.sync->addPair(e.spec("Books", "/Books"));
    PB_CHECK(!r.value("ok").toBool());
    PB_CHECK_EQ(key(r), std::string("auth_failed"));
    PB_CHECK_EQ(e.store->sync_pairs().size(), size_t(0));
}

// ------------------------------------------------------------------------------------------------------------------------------ review (US2)

namespace {
// the reader folder of the pair starts with a file that is not on the server
void fill(Env& e) {
    e.server.add_file("/Books/one.epub", "first book");
    e.server.add_file("/Books/two.epub", "second book, longer");
    e.server.add_file("/Books/Novels/three.epub", "third");
}
bool on_card(const Env& e, const std::string& rel) {
    struct stat st;
    return ::stat((e.root + "/Books/" + rel).c_str(), &st) == 0;
}
}  // namespace

PB_TEST(compare_ensures_the_network_runs_on_the_worker_and_leaves_a_plan_that_waits) {
    Env e;
    fill(e);
    const int64_t id = e.books_pair();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    PB_CHECK(e.sync->busy());   // started, not finished: the work is on the worker
    e.settle();
    PB_CHECK_EQ(ready.count(), 1);
    PB_CHECK_EQ(ready.at(0).at(0).toLongLong(), static_cast<qlonglong>(id));
    const qlonglong plan_id = ready.at(0).at(1).toLongLong();
    PB_CHECK(plan_id > 0);
    PB_CHECK_EQ(e.ensure_calls, 1);
    PB_CHECK_EQ(e.pair().value("state").toString().toStdString(), std::string("plan_ready"));
    PB_CHECK(e.pair().value("planReady").toBool());
    PB_CHECK_EQ(e.sync->readyCount(), 1);
    PB_CHECK_EQ(e.server.count("GET"), size_t(0));   // nothing was downloaded by looking
    PB_CHECK(!on_card(e, "one.epub"));
    for (const char* m : {"PUT", "MKCOL", "MOVE", "COPY", "DELETE"}) PB_CHECK_EQ(e.server.count(m), size_t(0));
    PB_CHECK_EQ(e.sync->stage().toStdString(), std::string("idle"));
}

PB_TEST(without_a_connection_nothing_is_compared_and_the_bridge_says_so) {
    Env e;
    fill(e);
    const int64_t id = e.books_pair();
    e.net_ok = false;
    QSignalSpy failed(e.sync.get(), &SyncBridge::comparisonFailed);
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    e.settle();
    PB_CHECK_EQ(failed.count(), 1);
    PB_CHECK_EQ(failed.at(0).at(1).toString().toStdString(), std::string("no_network"));
    PB_CHECK_EQ(ready.count(), 0);
    PB_CHECK_EQ(e.requests(), size_t(0));
    PB_CHECK_EQ(e.pair().value("state").toString().toStdString(), std::string("never"));
}

PB_TEST(the_plan_header_has_the_totals_the_sizes_and_whether_it_can_be_approved) {
    Env e;
    fill(e);
    const int64_t id = e.books_pair();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    e.settle();
    const qlonglong plan_id = ready.at(0).at(1).toLongLong();
    const QVariantMap h = e.sync->plan(plan_id);
    PB_CHECK_EQ(h.value("id").toLongLong(), plan_id);
    PB_CHECK_EQ(h.value("pairId").toLongLong(), static_cast<qlonglong>(id));
    PB_CHECK_EQ(h.value("state").toString().toStdString(), std::string("ready"));
    PB_CHECK_EQ(h.value("trigger").toString().toStdString(), std::string("tap"));
    const QVariantMap totals = h.value("totals").toMap();
    PB_CHECK_EQ(totals.value("downloads").toInt(), 3);
    PB_CHECK_EQ(totals.value("uploads").toInt(), 0);
    PB_CHECK_EQ(totals.value("skipped").toInt(), 0);
    PB_CHECK_EQ(totals.value("foldersNew").toInt(), 1);
    const QVariantMap sizes = h.value("sizes").toMap();
    PB_CHECK_EQ(sizes.value("downloadText").toString().toStdString(), std::string("34 B"));   // 10 + 19 + 5 bytes
    PB_CHECK(!h.value("freeText").toString().isEmpty());
    PB_CHECK(!h.value("reserveText").toString().isEmpty());
    PB_CHECK(h.value("approvable").toBool());
    PB_CHECK_EQ(h.value("approveErrorKey").toString().toStdString(), std::string(""));
    PB_CHECK(!h.value("stale").toBool());
    PB_CHECK(!h.value("incomplete").toBool());
    PB_CHECK(!h.value("truncated").toBool());
    PB_CHECK_EQ(h.value("ageKind").toString().toStdString(), std::string("just_now"));
    // an hour later it is stale and cannot be approved
    e.clock.t += pbc::kSyncPlanMaxAgeS + 60;
    const QVariantMap old = e.sync->plan(plan_id);
    PB_CHECK(old.value("stale").toBool());
    PB_CHECK(!old.value("approvable").toBool());
    PB_CHECK_EQ(old.value("approveErrorKey").toString().toStdString(), std::string("plan_stale"));
    PB_CHECK_EQ(old.value("ageKind").toString().toStdString(), std::string("hours"));
    PB_CHECK(e.sync->plan(plan_id + 999).isEmpty());
}

PB_TEST(plan_pages_hold_the_rows_of_a_section_with_the_right_counts) {
    Env e;
    for (int i = 0; i < 11; ++i) e.server.add_file("/Books/b" + std::to_string(10 + i) + ".epub", std::string(10 + static_cast<size_t>(i), 'x'));
    e.server.add_file("/Books/secret.epub", "x");
    e.server.set_unavailable("/Books/secret.epub", "no_permission");
    const int64_t id = e.books_pair();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    e.settle();
    const qlonglong plan_id = ready.at(0).at(1).toLongLong();
    e.sync->setPlanPageSize(5);
    PB_CHECK_EQ(e.sync->planPageSize(), 5);
    PB_CHECK_EQ(e.sync->planPageCount(plan_id, "transfers"), 3);   // 11 downloads in pages of 5
    PB_CHECK_EQ(e.sync->planPage(plan_id, "transfers", 0).size(), 5);
    PB_CHECK_EQ(e.sync->planPage(plan_id, "transfers", 2).size(), 1);
    PB_CHECK_EQ(e.sync->planPage(plan_id, "transfers", 3).size(), 0);
    PB_CHECK_EQ(e.sync->planPageCount(plan_id, "skips"), 1);
    PB_CHECK_EQ(e.sync->planPageCount(plan_id, "proposals"), 1);   // an empty section still has one (empty) page
    const QVariantMap row = e.sync->planPage(plan_id, "transfers", 0).at(0).toMap();
    for (const char* k : {"seq", "nameText", "localNameText", "action", "sizeText", "reason", "unapprovable", "removed", "allowed", "choice"}) PB_CHECK(row.contains(k));
    PB_CHECK_EQ(row.value("action").toString().toStdString(), std::string("download"));
    PB_CHECK_EQ(row.value("nameText").toString().toStdString(), std::string("b10.epub"));
    PB_CHECK_EQ(row.value("sizeText").toString().toStdString(), std::string("10 B"));
    const QVariantMap skip = e.sync->planPage(plan_id, "skips", 0).at(0).toMap();
    PB_CHECK_EQ(skip.value("nameText").toString().toStdString(), std::string("secret.epub"));
    PB_CHECK_EQ(skip.value("reason").toString().toStdString(), std::string("unavailable"));
    PB_CHECK_EQ(e.sync->planPageCount(plan_id, "nonsense"), 1);
    PB_CHECK(e.sync->planPage(plan_id, "nonsense", 0).isEmpty());
}

PB_TEST(removing_an_entry_changes_the_totals_and_only_a_ready_plan_allows_it) {
    Env e;
    fill(e);
    const int64_t id = e.books_pair();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    e.settle();
    const qlonglong plan_id = ready.at(0).at(1).toLongLong();
    const QVariantMap r = e.sync->removeEntry(plan_id, 1, true);
    PB_CHECK(r.value("ok").toBool());
    PB_CHECK_EQ(e.sync->plan(plan_id).value("totals").toMap().value("downloads").toInt(), 2);
    PB_CHECK(e.sync->planPage(plan_id, "transfers", 0).at(0).toMap().value("removed").toBool());
    PB_CHECK(e.sync->removeEntry(plan_id, 1, false).value("ok").toBool());
    PB_CHECK_EQ(e.sync->plan(plan_id).value("totals").toMap().value("downloads").toInt(), 3);
    PB_CHECK(!e.sync->removeEntry(plan_id, 99, true).value("ok").toBool());
    PB_CHECK(e.sync->cancelPlan(plan_id).value("ok").toBool());
    PB_CHECK_EQ(key(e.sync->removeEntry(plan_id, 1, true)), std::string("plan_not_approvable"));
}

PB_TEST(approve_is_the_only_call_that_starts_a_transfer_and_the_result_is_kept) {
    Env e;
    fill(e);
    const int64_t id = e.books_pair();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    QSignalSpy finished(e.sync.get(), &SyncBridge::runFinished);
    e.sync->compare(id);
    e.settle();
    const qlonglong plan_id = ready.at(0).at(1).toLongLong();
    e.sync->removeEntry(plan_id, 3, true);   // two.epub: the plan is in name order (Novels/three.epub, one.epub, two.epub)
    e.sync->refresh();
    e.sync->planPage(plan_id, "transfers", 0);
    e.sync->plan(plan_id);
    e.sync->resultFor(id);
    PB_CHECK_EQ(e.server.count("GET"), size_t(0));   // no call but approve has fetched a byte
    const QVariantMap a = e.sync->approve(plan_id);
    PB_CHECK(a.value("ok").toBool());
    PB_CHECK(e.sync->busy());
    e.settle();
    PB_CHECK_EQ(finished.count(), 1);
    const QVariantMap res = finished.at(0).at(0).toMap();
    PB_CHECK_EQ(res.value("pairId").toLongLong(), static_cast<qlonglong>(id));
    PB_CHECK_EQ(res.value("downloaded").toInt(), 2);
    PB_CHECK_EQ(res.value("skipped").toInt(), 1);   // the one removed from the plan
    PB_CHECK_EQ(res.value("failed").toInt(), 0);
    PB_CHECK(on_card(e, "one.epub") && on_card(e, "Novels/three.epub") && !on_card(e, "two.epub"));
    PB_CHECK_EQ(e.sync->lastResult().value("downloaded").toInt(), 2);
    PB_CHECK_EQ(e.sync->stage().toStdString(), std::string("idle"));
    PB_CHECK_EQ(e.pair().value("state").toString().toStdString(), std::string("in_sync"));
    PB_CHECK_EQ(e.sync->readyCount(), 0);
    const QVariantMap again = e.sync->resultFor(id);   // the last result stays available
    PB_CHECK_EQ(again.value("downloaded").toInt(), 2);
    PB_CHECK_EQ(again.value("skipped").toInt(), 1);
    PB_CHECK(!again.value("atText").toString().isEmpty());
    PB_CHECK(e.pair().value("lastText").toString().startsWith("Last sync"));
}

PB_TEST(approve_refuses_what_the_core_refuses_and_starts_nothing) {
    Env e;
    fill(e);
    const int64_t id = e.books_pair();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    e.settle();
    const qlonglong plan_id = ready.at(0).at(1).toLongLong();
    e.clock.t += pbc::kSyncPlanMaxAgeS + 10;
    QVariantMap a = e.sync->approve(plan_id);
    PB_CHECK(!a.value("ok").toBool());
    PB_CHECK_EQ(key(a), std::string("plan_stale"));
    PB_CHECK(!e.sync->busy());
    PB_CHECK_EQ(e.server.count("GET"), size_t(0));
    PB_CHECK_EQ(key(e.sync->approve(plan_id + 99)), std::string("not_found"));
    // no connection: approving needs one
    e.clock.t = e.server.server_time();
    e.net_ok = false;
    a = e.sync->approve(plan_id);
    e.settle();
    PB_CHECK(!a.value("ok").toBool() || e.server.count("GET") == 0);
    PB_CHECK_EQ(e.server.count("GET"), size_t(0));
    PB_CHECK(!on_card(e, "one.epub"));
}

PB_TEST(a_second_comparison_or_approval_for_a_busy_pair_answers_sync_busy) {
    Env e;
    fill(e);
    const int64_t id = e.books_pair();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    e.settle();
    const qlonglong plan_id = ready.at(0).at(1).toLongLong();
    QSignalSpy failed(e.sync.get(), &SyncBridge::comparisonFailed);
    bool second_compare = false, second_approve = false;
    e.on_step = [&](const char* step) {
        if (std::string(step) != "before_download" || second_approve) return;
        second_approve = true;
        QMetaObject::invokeMethod(e.sync.get(), [&] {
            e.sync->compare(id);   // while the pair is running
            PB_CHECK_EQ(key(e.sync->approve(plan_id)), std::string("sync_busy"));
            second_compare = true;
        }, Qt::BlockingQueuedConnection);
    };
    const QVariantMap a = e.sync->approve(plan_id);
    PB_CHECK(a.value("ok").toBool());
    e.settle();
    PB_CHECK(second_compare);
    PB_CHECK(failed.count() >= 1);
    PB_CHECK_EQ(failed.at(0).at(1).toString().toStdString(), std::string("sync_busy"));
    PB_CHECK(on_card(e, "one.epub"));
}

PB_TEST(choices_for_proposals_are_checked_confirmed_first_and_applied_only_on_the_confirmation) {
    Env e;
    pbc::ServerCaps caps;
    caps.trash = true;
    PB_CHECK(e.store->save_server_caps(caps).ok());
    ::unlink((e.root + "/Books/a.epub").c_str());
    e.server.remove("/Books/a.epub");
    e.server.add_file("/Books/x.epub", "xxxx");
    e.server.add_file("/Books/y.epub", "yyyy");
    QVariantMap spec = e.spec("Books", "/Books", "both");
    spec["confirmed"] = true;
    const int64_t id = e.sync->addPair(spec).value("id").toLongLong();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    e.settle();
    PB_CHECK(e.sync->approve(ready.at(0).at(1).toLongLong()).value("ok").toBool());
    e.settle();
    PB_CHECK(on_card(e, "x.epub") && on_card(e, "y.epub"));
    // x is removed on the server, y on the reader: two proposals
    e.server.remove("/Books/x.epub");
    ::unlink((e.root + "/Books/y.epub").c_str());
    e.sync->compare(id);
    e.settle();
    const qlonglong plan = ready.at(1).at(1).toLongLong();
    const QVariantList rows = e.sync->planPage(plan, "proposals", 0);
    PB_CHECK_EQ(rows.size(), 2);
    int seq_x = 0, seq_y = 0;
    for (const QVariant& v : rows) {
        const QVariantMap r = v.toMap();
        if (r.value("nameText").toString() == "x.epub") seq_x = r.value("seq").toInt();
        if (r.value("nameText").toString() == "y.epub") seq_y = r.value("seq").toInt();
    }
    PB_CHECK(seq_x > 0 && seq_y > 0);
    // only an allowed choice is accepted
    PB_CHECK_EQ(key(e.sync->setChoice(plan, seq_x, "bogus")), std::string("not_allowed"));
    PB_CHECK(e.sync->setChoice(plan, seq_x, "delete").value("ok").toBool());
    PB_CHECK(e.sync->setChoice(plan, seq_y, "delete").value("ok").toBool());
    // approving the plan applies nothing
    const QVariantMap approved = e.sync->approve(plan);
    PB_CHECK(approved.value("ok").toBool());
    e.settle();
    PB_CHECK(!e.server.has("/Books/y.epub") == false);   // y is still on the server
    PB_CHECK(on_card(e, "x.epub"));                       // x is still on the card
    // without `confirmed` the call only describes what would happen
    const QVariantMap ask = e.sync->applyChoices(plan, false);
    PB_CHECK(ask.value("ok").toBool());
    PB_CHECK_EQ(ask.value("deletes").toList().size(), 2);
    bool server_trash = false, reader = false;
    for (const QVariant& v : ask.value("deletes").toList()) {
        const QVariantMap d = v.toMap();
        if (d.value("side").toString() == "server") server_trash = d.value("trash").toBool();
        if (d.value("side").toString() == "reader") reader = !d.value("trash").toBool();
    }
    PB_CHECK(server_trash && reader);
    PB_CHECK(on_card(e, "x.epub") && e.server.has("/Books/y.epub"));
    // with it the choices are applied
    QSignalSpy finished(e.sync.get(), &SyncBridge::runFinished);
    PB_CHECK(e.sync->applyChoices(plan, true).value("ok").toBool());
    e.settle();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK_EQ(finished.at(0).at(0).toMap().value("deleted").toInt(), 2);
    PB_CHECK(!on_card(e, "x.epub"));
    PB_CHECK(e.server.trash_has("/Books/y.epub"));
}

PB_TEST(applying_choices_needs_a_plan_and_a_choice) {
    Env e;
    PB_CHECK_EQ(key(e.sync->applyChoices(0, true)), std::string("plan_not_approvable"));
    PB_CHECK_EQ(key(e.sync->setChoice(0, 1, "leave")), std::string("plan_not_approvable"));
    PB_CHECK_EQ(key(e.sync->applyChoices(999, false)), std::string("not_found"));
}

PB_TEST(a_plan_over_the_limit_says_by_how_much_and_cannot_be_approved) {
    Env e;
    for (int i = 0; i < 4; ++i) e.server.add_file("/Books/f" + std::to_string(i) + ".epub", std::string(100, 'x'));
    const int64_t id = e.books_pair();
    QVariantMap spec = e.spec("Books", "/Books");
    spec["maxFiles"] = 2;
    PB_CHECK(e.sync->updatePair(id, spec).value("ok").toBool());
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    e.settle();
    const QVariantMap plan = e.sync->plan(ready.at(0).at(1).toLongLong());
    PB_CHECK_EQ(plan.value("overLimit").toString().toStdString(), std::string("files"));
    PB_CHECK(plan.value("overLimitText").toString().contains("Over the file limit by 2"));
    PB_CHECK(!plan.value("approvable").toBool());
    PB_CHECK_EQ(key(e.sync->approve(ready.at(0).at(1).toLongLong())), std::string("over_limit"));
    PB_CHECK(e.pair().value("planOverLimit").toBool());
}

PB_TEST(cancel_run_stops_after_the_item_in_flight) {
    Env e;
    for (int i = 0; i < 6; ++i) e.server.add_file("/Books/f" + std::to_string(i) + ".epub", std::string(100, 'x'));
    const int64_t id = e.books_pair();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    QSignalSpy finished(e.sync.get(), &SyncBridge::runFinished);
    e.sync->compare(id);
    e.settle();
    int downloaded = 0;
    e.on_step = [&](const char* step) {
        if (std::string(step) == "downloaded" && ++downloaded == 2) e.sync->cancelRun();
    };
    PB_CHECK(e.sync->approve(ready.at(0).at(1).toLongLong()).value("ok").toBool());
    e.settle();
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK_EQ(finished.at(0).at(0).toMap().value("downloaded").toInt(), 2);
    PB_CHECK_EQ(finished.at(0).at(0).toMap().value("stoppedBy").toString().toStdString(), std::string("cancelled"));
    PB_CHECK_EQ(e.pair().value("state").toString().toStdString(), std::string("interrupted"));
    PB_CHECK_EQ(e.pair().value("stateText").toString().toStdString(), std::string("Interrupted: 2 of 6 done"));   // what the run had done when it stopped
}

PB_TEST(cancelling_a_plan_leaves_the_pair_idle_and_everything_else_alone) {
    Env e;
    fill(e);
    const int64_t id = e.books_pair();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    e.settle();
    e.server.clear_log();
    PB_CHECK(e.sync->cancelPlan(ready.at(0).at(1).toLongLong()).value("ok").toBool());
    PB_CHECK_EQ(e.pair().value("state").toString().toStdString(), std::string("never"));
    PB_CHECK_EQ(e.sync->readyCount(), 0);
    PB_CHECK_EQ(e.requests(), size_t(0));
    PB_CHECK(!on_card(e, "one.epub"));
}

PB_TEST(a_pair_with_nothing_to_do_says_it_is_in_sync_and_has_no_plan_to_approve) {
    Env e;
    fill(e);
    const int64_t id = e.books_pair();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    e.settle();
    PB_CHECK(e.sync->approve(ready.at(0).at(1).toLongLong()).value("ok").toBool());
    e.settle();
    ready.clear();
    e.sync->compare(id);
    e.settle();
    PB_CHECK_EQ(e.pair().value("state").toString().toStdString(), std::string("in_sync"));
    PB_CHECK_EQ(e.sync->readyCount(), 0);
    PB_CHECK_EQ(ready.count(), 1);   // the plan is shown anyway, and says that nothing is to be done
    const QVariantMap h = e.sync->plan(ready.at(0).at(1).toLongLong());
    PB_CHECK_EQ(h.value("totals").toMap().value("downloads").toInt(), 0);
    PB_CHECK_EQ(h.value("totals").toMap().value("inSync").toInt(), 3);
}

PB_TEST(the_offline_preview_uses_stored_listings_only_and_is_flagged_and_not_approvable) {
    Env e;
    fill(e);
    const int64_t id = e.books_pair();
    {
        pbc::FolderService fs(*e.store, e.server, e.clock);
        PB_CHECK(fs.load("/Books", pbc::Fetch::Always).error.ok());
    }
    e.server.clear_log();
    e.wifi = false;
    QSignalSpy previewed(e.sync.get(), &SyncBridge::previewReady);
    e.sync->previewOffline(id);
    e.settle();
    PB_CHECK_EQ(previewed.count(), 1);
    PB_CHECK_EQ(e.requests(), size_t(0));
    const QVariantMap h = e.sync->plan(0);
    PB_CHECK_EQ(h.value("state").toString().toStdString(), std::string("preview"));
    PB_CHECK(h.value("stale").toBool());
    PB_CHECK(h.value("incomplete").toBool());
    PB_CHECK(!h.value("approvable").toBool());
    PB_CHECK_EQ(h.value("totals").toMap().value("downloads").toInt(), 2);   // /Books/Novels was never listed
    PB_CHECK_EQ(h.value("incompleteFolders").toInt(), 1);
    PB_CHECK_EQ(e.sync->planPage(0, "transfers", 0).size(), 2);
    PB_CHECK_EQ(key(e.sync->approve(0)), std::string("plan_not_approvable"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM sync_plans"), std::string("0"));
}

PB_TEST(a_pair_of_another_account_is_not_compared_or_approved) {
    Env e;
    fill(e);
    const int64_t id = e.books_pair();
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(id);
    e.settle();
    const qlonglong plan_id = ready.at(0).at(1).toLongLong();
    e.account("someone");
    e.sync->refresh();
    e.server.clear_log();
    QSignalSpy failed(e.sync.get(), &SyncBridge::comparisonFailed);
    e.sync->compare(id);
    e.settle();
    PB_CHECK_EQ(failed.count(), 1);
    PB_CHECK_EQ(failed.at(0).at(1).toString().toStdString(), std::string("account_changed"));
    PB_CHECK_EQ(key(e.sync->approve(plan_id)), std::string("account_changed"));
    PB_CHECK_EQ(e.requests(), size_t(0));
}

// ------------------------------------------------------------------------------------------------------------------------------ triggers (US5)

namespace {
// Books (start-up on, Auto Sync on) and Other (both off), each with something to download
struct Two {
    int64_t books = 0, other = 0;
};
Two two_pairs(Env& e, bool books_startup, bool books_auto, bool other_startup = false, bool other_auto = false) {
    e.server.add_file("/Books/one.epub", "first book");
    e.server.add_file("/Other/o.epub", "other book");
    ::unlink((e.root + "/Books/a.epub").c_str());
    e.server.remove("/Books/a.epub");
    Two t;
    // saving a pair through the bridge arms its own Auto Sync (tested on its own), so the switch is set in the store afterwards
    QVariantMap a = e.spec("Books", "/Books");
    a["startupSync"] = books_startup;
    t.books = e.sync->addPair(a).value("id").toLongLong();
    QVariantMap b = e.spec("Other", "/Other");
    b["startupSync"] = other_startup;
    t.other = e.sync->addPair(b).value("id").toLongLong();
    for (const auto& [id, on] : {std::pair<int64_t, bool>{t.books, books_auto}, std::pair<int64_t, bool>{t.other, other_auto}}) {
        auto pair = e.store->sync_pair(id);
        pair->auto_sync = on;
        PB_CHECK(e.store->sync_pair_update(*pair).ok());
    }
    e.server.clear_log();
    return t;
}
QVariantMap pair_of(Env& e, int64_t id) {
    for (const QVariant& v : e.sync->pairs())
        if (v.toMap().value("id").toLongLong() == id) return v.toMap();
    return QVariantMap();
}
void wait_ms(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QTest::qWait(10);
}
}  // namespace

PB_TEST(nothing_starts_by_itself_not_at_construction_not_while_idle_not_with_the_wifi_changing) {
    Env e;
    e.quiet_ms = 50;
    e.build();
    two_pairs(e, true, true, true, true);
    wait_ms(300);
    e.wifi = false;
    e.sync->refresh();
    wait_ms(100);
    e.wifi = true;
    e.sync->refresh();
    wait_ms(300);
    PB_CHECK_EQ(e.requests(), size_t(0));
    PB_CHECK_EQ(e.sync->readyCount(), 0);
    PB_CHECK(!e.sync->busy());
}

PB_TEST(startup_compare_compares_the_pairs_with_the_switch_on_one_after_the_other_and_only_stores_plans) {
    Env e;
    const Two t = two_pairs(e, true, false, false, false);
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->startupCompare();
    e.settle();
    PB_CHECK_EQ(ready.count(), 0);   // an unattended comparison opens nothing by itself
    PB_CHECK_EQ(e.ensure_calls, 0);   // and never asks for the network: Wi-Fi was already on
    PB_CHECK_EQ(pair_of(e, t.books).value("state").toString().toStdString(), std::string("plan_ready"));
    PB_CHECK_EQ(pair_of(e, t.other).value("state").toString().toStdString(), std::string("never"));   // its switch is off
    PB_CHECK_EQ(e.sync->readyCount(), 1);
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(1));          // one folder, one listing
    PB_CHECK_EQ(e.server.count("GET"), size_t(0));
    for (const char* m : {"PUT", "MKCOL", "MOVE", "COPY", "DELETE"}) PB_CHECK_EQ(e.server.count(m), size_t(0));
    PB_CHECK(!on_card(e, "one.epub"));   // nothing was transferred
    const int64_t plan = e.store->sync_ready_plan(t.books).value_or(0);
    PB_CHECK(plan > 0);
    PB_CHECK_EQ(e.store->sync_plan(plan)->trigger, std::string("startup"));
    PB_CHECK_EQ(e.store->sync_plan(plan)->state, std::string("ready"));   // it waits for the user
    // both on: one after the other
    Env f;
    const Two u = two_pairs(f, true, false, true, false);
    f.sync->startupCompare();
    f.settle();
    PB_CHECK_EQ(f.sync->readyCount(), 2);
    PB_CHECK_EQ(f.server.count("PROPFIND"), size_t(2));
    PB_CHECK(u.books != u.other);
}

PB_TEST(startup_compare_with_the_wifi_off_requests_nothing_and_nothing_is_retried) {
    Env e;
    two_pairs(e, true, false, true, false);
    e.wifi = false;
    e.sync->startupCompare();
    e.settle();
    wait_ms(100);
    e.wifi = true;   // coming up later arms nothing
    e.sync->refresh();
    wait_ms(200);
    PB_CHECK_EQ(e.requests(), size_t(0));
    PB_CHECK_EQ(e.sync->readyCount(), 0);
}

PB_TEST(startup_compare_without_any_start_switch_does_nothing) {
    Env e;
    two_pairs(e, false, true, false, true);
    e.sync->startupCompare();
    e.settle();
    PB_CHECK_EQ(e.requests(), size_t(0));
}

PB_TEST(an_action_below_a_pairs_server_folder_compares_that_pair_only_after_the_quiet_period) {
    Env e;
    e.quiet_ms = 150;
    e.build();
    const Two t = two_pairs(e, false, true, false, true);   // both pairs have Auto Sync on
    e.sync->noteAction("/Books/Novels");
    wait_ms(60);
    PB_CHECK_EQ(e.requests(), size_t(0));   // the quiet period is not over
    e.sync->noteAction("/Books");           // a further action restarts it
    wait_ms(100);
    PB_CHECK_EQ(e.requests(), size_t(0));
    wait_ms(400);
    e.settle();
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(1));   // one comparison for both actions, of this pair only
    PB_CHECK_EQ(pair_of(e, t.books).value("state").toString().toStdString(), std::string("plan_ready"));
    PB_CHECK_EQ(pair_of(e, t.other).value("state").toString().toStdString(), std::string("never"));
    PB_CHECK_EQ(e.ensure_calls, 0);   // Auto Sync never switches Wi-Fi on
    PB_CHECK_EQ(e.store->sync_plan(e.store->sync_ready_plan(t.books).value())->trigger, std::string("auto"));
    PB_CHECK_EQ(e.server.count("GET"), size_t(0));
    PB_CHECK(!on_card(e, "one.epub"));
    // it fires once: nothing more without a new action
    wait_ms(300);
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(1));
}

PB_TEST(an_action_below_a_pairs_reader_folder_arms_it_too) {
    Env e;
    e.quiet_ms = 80;
    e.build();
    const Two t = two_pairs(e, false, true, false, false);
    e.sync->noteLocalAction(QString::fromStdString(e.root + "/Books/new-file.epub"));
    wait_ms(400);
    e.settle();
    PB_CHECK_EQ(pair_of(e, t.books).value("state").toString().toStdString(), std::string("plan_ready"));
    // a path that no pair holds, and a pair without the switch, arm nothing
    e.server.clear_log();
    e.sync->noteLocalAction(QString::fromStdString(e.root + "/Elsewhere/x"));
    e.sync->noteLocalAction(QString::fromStdString(e.root + "/Other/y"));   // the pair for Other has Auto Sync off
    e.sync->noteAction("/Nothing/here");
    e.sync->noteAction("/Other/z");
    wait_ms(300);
    e.settle();
    PB_CHECK_EQ(e.requests(), size_t(0));
}

PB_TEST(with_the_global_auto_sync_off_nothing_is_armed) {
    Env e;
    e.quiet_ms = 50;
    e.build();
    two_pairs(e, false, true, false, true);
    PB_CHECK(e.store->set_setting("auto_sync", "0").ok());
    e.sync->noteAction("/Books/x");
    e.sync->noteLocalAction(QString::fromStdString(e.root + "/Other/y"));
    wait_ms(300);
    e.settle();
    PB_CHECK_EQ(e.requests(), size_t(0));
}

PB_TEST(a_quiet_period_that_ends_without_wifi_leaves_the_pair_waiting_and_retries_nothing) {
    Env e;
    e.quiet_ms = 80;
    e.build();
    const Two t = two_pairs(e, false, true);
    e.sync->noteAction("/Books/x");
    e.wifi = false;
    wait_ms(400);
    e.settle();
    PB_CHECK_EQ(e.requests(), size_t(0));
    PB_CHECK_EQ(pair_of(e, t.books).value("state").toString().toStdString(), std::string("waiting"));
    PB_CHECK_EQ(e.sync->waitingNotice().toStdString(), std::string("offline"));
    e.wifi = true;
    e.sync->refresh();
    wait_ms(300);
    PB_CHECK_EQ(e.requests(), size_t(0));   // nothing retries by itself
    // a tap compares it and the pair is no longer waiting
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(t.books);
    e.settle();
    PB_CHECK_EQ(ready.count(), 1);
    PB_CHECK(e.sync->waitingNotice().isEmpty());
}

PB_TEST(saving_a_pair_is_a_change_of_the_users_that_arms_its_own_auto_sync) {
    Env e;
    e.quiet_ms = 80;
    e.build();
    e.server.add_file("/Books/one.epub", "first book");
    ::unlink((e.root + "/Books/a.epub").c_str());
    e.server.remove("/Books/a.epub");
    QVariantMap a = e.spec("Books", "/Books");
    a["autoSync"] = true;
    e.server.clear_log();
    const int64_t id = e.sync->addPair(a).value("id").toLongLong();
    wait_ms(400);
    e.settle();
    PB_CHECK_EQ(pair_of(e, id).value("state").toString().toStdString(), std::string("plan_ready"));
    PB_CHECK_EQ(e.server.count("GET"), size_t(0));
}

PB_TEST(a_run_that_is_going_holds_the_auto_comparison_until_it_ends_and_there_is_one_follow_up) {
    Env e;
    e.quiet_ms = 60;
    e.build();
    const Two t = two_pairs(e, false, true);
    QSignalSpy ready(e.sync.get(), &SyncBridge::planReady);
    e.sync->compare(t.books);
    e.settle();
    const qlonglong plan_id = ready.at(0).at(1).toLongLong();
    e.server.clear_log();
    bool noted = false;
    e.on_step = [&](const char* step) {
        if (std::string(step) != "before_download" || noted) return;
        noted = true;
        QMetaObject::invokeMethod(e.sync.get(), [&] { e.sync->noteAction("/Books/x"); }, Qt::BlockingQueuedConnection);
        QThread::msleep(200);   // the quiet period ends while the run is still going
    };
    PB_CHECK(e.sync->approve(plan_id).value("ok").toBool());
    e.settle();
    wait_ms(300);
    e.settle();
    PB_CHECK_EQ(e.server.count("PROPFIND"), size_t(1));   // the follow-up comparison, after the run, once
    PB_CHECK_EQ(e.server.count("GET"), size_t(1));        // only the one download of the approved plan
}

PB_TEST_MAIN()
