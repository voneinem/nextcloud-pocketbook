// The PickerBridge (contracts/bridge-qml.md): the reader-side file browser of the Upload action. Real core, real temporary folders. It reads no
// file content, sends no request, and can never select the app's own data, a link, an empty file or a file the storage cannot hold.
#include <QCoreApplication>
#include <QTest>
#include <QVariantMap>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pbc/store.h"
#include "picker_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_picker_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

pbc::SystemClock g_clock;

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
    std::string root;
    std::unique_ptr<pbc::Store> store;
    std::unique_ptr<PickerBridge> picker;
    Env(bool with_account = true) {
        root = dir.file("root");
        for (const char* d : {"Audio Books", "Downloads", "Notes", "PBCloud", "screens", "Other", "system/pbcloud"}) mkdirs(root + "/" + d);
        touch(root + "/Autorun.inf", "x");
        touch(root + "/install.md", "x");
        touch(root + "/Downloads/book.epub", "bookbook");
        touch(root + "/Downloads/.hidden", "h");
        mkdirs(root + "/Downloads/Series");
        touch(root + "/Downloads/Series/one.epub", "1");
        touch(root + "/system/pbcloud/credentials", "app-secret");
        auto r = pbc::Store::open(dir.file("data"), g_clock);
        if (!r.ok()) PB_FAIL_AT("cannot open the store: " + r.error.key);
        store = std::move(r.value);
        if (with_account) {
            pbc::AccountRow a;
            a.base_url = "https://cloud.example.org";
            a.login_name = "demo";
            a.user_id = "demo";
            PB_CHECK(store->save_account(a).ok());
        }
        PickerBridge::Config c;
        c.store = store.get();
        c.rules.root = root;
        c.rules.deny = {root + "/system/pbcloud"};
        c.topFolders = {"Audio Books", "Downloads", "Music", "My pictures", "Notes", "Photo", "screens", "PBCloud"};
        c.downloadRoot = root + "/PBCloud";
        picker = std::make_unique<PickerBridge>(c);
    }
    QStringList names(const QString& path) {
        QStringList n;
        for (const QVariant& v : picker->listDirectory(path)) n << v.toMap().value("name").toString();
        return n;
    }
    QString p(const std::string& rel) const { return QString::fromStdString(root + "/" + rel); }
};

}  // namespace

PB_TEST(the_root_lists_only_the_existing_top_folders_and_no_files) {
    Env e;
    PB_CHECK_EQ(e.picker->rootDirectory().toStdString(), e.root);
    PB_CHECK_EQ(e.names(e.picker->rootDirectory()).join(",").toStdString(), std::string("Audio Books,Downloads,Notes,PBCloud,screens"));
    for (const QVariant& v : e.picker->listDirectory(e.picker->rootDirectory())) PB_CHECK(v.toMap().value("dir").toBool());
    PB_CHECK_EQ(e.picker->topFolders().size(), 8);
}

PB_TEST(below_the_root_everything_is_listed_folders_first_without_hidden_entries) {
    Env e;
    PB_CHECK_EQ(e.names(e.p("Downloads")).join(",").toStdString(), std::string("Series,book.epub"));
    const QVariantMap file = e.picker->listDirectory(e.p("Downloads")).at(1).toMap();
    PB_CHECK_EQ(file.value("size").toLongLong(), 8LL);
    PB_CHECK_EQ(file.value("sizeText").toString().toStdString(), std::string("8 B"));
    PB_CHECK_EQ(file.value("path").toString().toStdString(), e.root + "/Downloads/book.epub");
    PB_CHECK(!file.value("dir").toBool());
    PB_CHECK(!file.value("selected").toBool());
}

PB_TEST(a_path_that_is_not_readable_or_not_allowed_lists_as_empty) {
    Env e;
    PB_CHECK(e.picker->listDirectory(e.p("missing")).isEmpty());
    PB_CHECK(e.picker->listDirectory(e.p("system/pbcloud")).isEmpty());      // the app's own data folder
    PB_CHECK(e.picker->listDirectory(QString()).isEmpty());
    PB_CHECK(e.picker->listDirectory(QStringLiteral("/etc")).isEmpty());      // outside the root
}

PB_TEST(the_layout_is_remembered_and_works_without_an_account) {
    Env e(false);
    PB_CHECK_EQ(e.picker->view().toStdString(), std::string("list"));
    e.picker->setView("tile");
    PB_CHECK_EQ(e.picker->view().toStdString(), std::string("tile"));
    PB_CHECK_EQ(e.store->setting("picker_view"), std::string("tile"));
    e.picker->setView("grid-of-doom");   // not a layout: ignored
    PB_CHECK_EQ(e.picker->view().toStdString(), std::string("tile"));
    PB_CHECK_EQ(e.store->setting("picker_view"), std::string("tile"));
    // a second bridge on the same store starts in the stored layout (after a restart)
    PickerBridge::Config c;
    c.store = e.store.get();
    c.rules.root = e.root;
    PickerBridge again(c);
    PB_CHECK_EQ(again.view().toStdString(), std::string("tile"));
    e.picker->setView("list");
    PB_CHECK_EQ(e.picker->view().toStdString(), std::string("list"));
}

PB_TEST(toggling_a_file_selects_and_unselects_it_and_the_listing_shows_it) {
    Env e;
    const QString book = e.p("Downloads/book.epub");
    PB_CHECK_EQ(e.picker->toggle(book).toStdString(), std::string());
    PB_CHECK_EQ(e.picker->fileCount(), 1);
    PB_CHECK_EQ(e.picker->folderCount(), 0);
    PB_CHECK_EQ(e.picker->byteCount(), 8LL);
    PB_CHECK_EQ(e.picker->selectedFiles().size(), 1);
    PB_CHECK(e.picker->listDirectory(e.p("Downloads")).at(1).toMap().value("selected").toBool());
    PB_CHECK_EQ(e.picker->toggle(book).toStdString(), std::string());   // a second tap unselects
    PB_CHECK_EQ(e.picker->fileCount(), 0);
    PB_CHECK(e.picker->selectedFiles().isEmpty());
    PB_CHECK(!e.picker->listDirectory(e.p("Downloads")).at(1).toMap().value("selected").toBool());
}

PB_TEST(a_file_that_cannot_be_sent_is_refused_with_its_reason_and_selects_nothing) {
    Env e;
    touch(e.root + "/Downloads/empty.txt", "");
    touch(e.root + "/Downloads/what?.txt", "x");
    PB_CHECK(::symlink((e.root + "/system/pbcloud/credentials").c_str(), (e.root + "/Downloads/link.txt").c_str()) == 0);
    {
        int fd = ::open((e.root + "/Downloads/huge.bin").c_str(), O_CREAT | O_WRONLY, 0600);
        PB_CHECK(fd >= 0);
        PB_CHECK(::ftruncate(fd, 4294967296ll) == 0);
        ::close(fd);
    }
    pbc::ServerCaps caps;
    caps.forbidden_chars = "?";
    PB_CHECK(e.store->save_server_caps(caps).ok());
    PB_CHECK_EQ(e.picker->toggle(e.p("Downloads/empty.txt")).toStdString(), std::string("source_unreadable"));
    PB_CHECK_EQ(e.picker->toggle(e.p("Downloads/huge.bin")).toStdString(), std::string("too_large_for_storage"));
    PB_CHECK_EQ(e.picker->toggle(e.p("Downloads/what?.txt")).toStdString(), std::string("name_forbidden"));
    PB_CHECK_EQ(e.picker->toggle(e.p("Downloads/link.txt")).toStdString(), std::string("source_not_allowed"));
    PB_CHECK_EQ(e.picker->toggle(e.p("system/pbcloud/credentials")).toStdString(), std::string("source_not_allowed"));
    PB_CHECK_EQ(e.picker->toggle(QStringLiteral("/etc/hostname")).toStdString(), std::string("source_not_allowed"));
    PB_CHECK_EQ(e.picker->fileCount(), 0);
}

PB_TEST(adding_a_folder_counts_what_is_in_it_and_a_second_add_unselects_it) {
    Env e;
    touch(e.root + "/Notes/trip.txt", "abc");
    mkdirs(e.root + "/Notes/photos");
    touch(e.root + "/Notes/photos/a.jpg", "aaaa");
    const QVariantMap r = e.picker->addFolder(e.p("Notes"));
    PB_CHECK(r.value("ok").toBool());
    PB_CHECK_EQ(r.value("folders").toInt(), 2);   // Notes and photos
    PB_CHECK_EQ(r.value("files").toInt(), 2);
    PB_CHECK_EQ(r.value("bytes").toLongLong(), 7LL);
    PB_CHECK_EQ(e.picker->folderCount(), 2);
    PB_CHECK_EQ(e.picker->fileCount(), 2);
    PB_CHECK_EQ(e.picker->selectedFolders().size(), 1);
    PB_CHECK(e.picker->listDirectory(e.picker->rootDirectory()).at(2).toMap().value("selected").toBool());   // the folder entry is marked
    const QVariantMap again = e.picker->addFolder(e.p("Notes"));
    PB_CHECK(again.value("ok").toBool());
    PB_CHECK_EQ(e.picker->folderCount(), 0);
    PB_CHECK(e.picker->selectedFolders().isEmpty());
}

PB_TEST(a_folder_over_the_walk_limit_or_not_allowed_is_refused) {
    Env e;
    pbc::SourceRules small;
    small.root = e.root;
    small.max_items = 5;
    PickerBridge::Config c;
    c.store = e.store.get();
    c.rules = small;
    PickerBridge limited(c);
    for (int i = 0; i < 12; ++i) touch(e.root + "/Notes/f" + std::to_string(100 + i), "x");
    const QVariantMap r = limited.addFolder(e.p("Notes"));
    PB_CHECK(!r.value("ok").toBool());
    PB_CHECK(r.value("overLimit").toBool());
    PB_CHECK_EQ(limited.folderCount(), 0);
    const QVariantMap denied = e.picker->addFolder(e.p("system/pbcloud"));
    PB_CHECK(!denied.value("ok").toBool());
    PB_CHECK_EQ(denied.value("errorKey").toString().toStdString(), std::string("source_not_allowed"));
    const QVariantMap file = e.picker->addFolder(e.p("Downloads/book.epub"));
    PB_CHECK(!file.value("ok").toBool());
    PB_CHECK_EQ(e.picker->folderCount(), 0);
}

PB_TEST(a_selection_that_does_not_fit_the_queue_is_refused) {
    Env e;
    for (int i = 0; i < pbc::kMaxQueueItems - 1; ++i) {
        pbc::QueueItem q;
        q.kind = pbc::ItemKind::Upload;
        q.server = "https://cloud.example.org";
        q.user_id = "demo";
        q.src_local = "/mnt/ext1/x" + std::to_string(i);
        q.dst_parent = "/Docs";
        q.dst_name = "n" + std::to_string(i);
        PB_CHECK(e.store->queue_add(q).ok());
    }
    PB_CHECK_EQ(e.picker->maxItems(), 1);
    PB_CHECK_EQ(e.picker->toggle(e.p("Downloads/book.epub")).toStdString(), std::string());
    touch(e.root + "/Downloads/two.txt", "2");
    PB_CHECK_EQ(e.picker->toggle(e.p("Downloads/two.txt")).toStdString(), std::string("queue_full"));
    PB_CHECK_EQ(e.picker->fileCount(), 1);
}

PB_TEST(clear_empties_the_selection) {
    Env e;
    PB_CHECK_EQ(e.picker->toggle(e.p("Downloads/book.epub")).toStdString(), std::string());
    PB_CHECK(e.picker->addFolder(e.p("Downloads/Series")).value("ok").toBool());
    PB_CHECK(e.picker->fileCount() > 0 && e.picker->folderCount() > 0);
    e.picker->clear();
    PB_CHECK_EQ(e.picker->fileCount(), 0);
    PB_CHECK_EQ(e.picker->folderCount(), 0);
    PB_CHECK_EQ(e.picker->byteCount(), 0LL);
}

// ------------------------------------------------------------------------------------------------ folder mode (specs/003-folder-sync)

PB_TEST(folder_mode_lists_folders_only_without_hidden_entries_or_links) {
    Env e;
    PB_CHECK(!e.picker->folderMode());
    e.picker->setFolderMode(true);
    PB_CHECK(e.picker->folderMode());
    PB_CHECK_EQ(e.names(e.p("Downloads")).join(",").toStdString(), std::string("Series"));   // book.epub and .hidden are not listed
    PB_CHECK(::symlink("/etc", (e.root + "/Downloads/LinkToEtc").c_str()) == 0);
    mkdirs(e.root + "/Downloads/.hiddenfolder");
    PB_CHECK_EQ(e.names(e.p("Downloads")).join(",").toStdString(), std::string("Series"));
    // the root lists every folder, not only the standard top folders, so that a folder the user made there can be found
    const QStringList top = e.names(e.picker->rootDirectory());
    PB_CHECK(top.contains("Other") && top.contains("Downloads") && top.contains("system"));
    for (const QVariant& v : e.picker->listDirectory(e.picker->rootDirectory())) PB_CHECK(v.toMap().value("dir").toBool());
    e.picker->setFolderMode(false);   // and back to the normal browser
    PB_CHECK_EQ(e.names(e.p("Downloads")).join(",").toStdString(), std::string("Series,book.epub"));
}

PB_TEST(folder_mode_shows_the_data_folder_and_the_download_folder_disabled_with_the_reason) {
    Env e;
    e.picker->setFolderMode(true);
    QVariantMap pbcloud, data;
    for (const QVariant& v : e.picker->listDirectory(e.picker->rootDirectory()))
        if (v.toMap().value("name").toString() == "PBCloud") pbcloud = v.toMap();
    for (const QVariant& v : e.picker->listDirectory(e.p("system")))
        if (v.toMap().value("name").toString() == "pbcloud") data = v.toMap();
    PB_CHECK(pbcloud.value("disabled").toBool());
    PB_CHECK_EQ(pbcloud.value("reason").toString().toStdString(), std::string("pair_folder_forbidden"));
    PB_CHECK(data.value("disabled").toBool());
    PB_CHECK_EQ(data.value("reason").toString().toStdString(), std::string("pair_folder_forbidden"));
    // an ordinary folder is not disabled
    for (const QVariant& v : e.picker->listDirectory(e.p("Downloads"))) PB_CHECK(!v.toMap().value("disabled").toBool());
    // what is inside them is not browsed
    PB_CHECK(e.picker->listDirectory(e.p("system/pbcloud")).isEmpty());
}

PB_TEST(folder_mode_refuses_to_select_a_file) {
    Env e;
    e.picker->setFolderMode(true);
    PB_CHECK_EQ(e.picker->toggle(e.p("Downloads/book.epub")).toStdString(), std::string("source_not_allowed"));
    PB_CHECK_EQ(e.picker->fileCount(), 0);
}

PB_TEST(choose_folder_answers_for_the_pair_page_without_touching_the_selection) {
    Env e;
    e.picker->setFolderMode(true);
    const QVariantMap ok = e.picker->chooseFolder(e.p("Downloads/Series"));
    PB_CHECK(ok.value("ok").toBool());
    PB_CHECK_EQ(ok.value("path").toString().toStdString(), e.root + "/Downloads/Series");
    PB_CHECK_EQ(ok.value("errorKey").toString().toStdString(), std::string());
    PB_CHECK_EQ(e.picker->fileCount() + e.picker->folderCount(), 0);
    PB_CHECK_EQ(e.picker->chooseFolder(e.picker->rootDirectory()).value("errorKey").toString().toStdString(), std::string("pair_folder_forbidden"));
    PB_CHECK_EQ(e.picker->chooseFolder(e.p("PBCloud")).value("errorKey").toString().toStdString(), std::string("pair_folder_forbidden"));
    PB_CHECK_EQ(e.picker->chooseFolder(e.p("system")).value("errorKey").toString().toStdString(), std::string("pair_folder_forbidden"));   // contains the data folder
    PB_CHECK_EQ(e.picker->chooseFolder(QStringLiteral("/etc")).value("errorKey").toString().toStdString(), std::string("source_not_allowed"));
    PB_CHECK_EQ(e.picker->chooseFolder(e.p("Downloads/book.epub")).value("errorKey").toString().toStdString(), std::string("source_not_allowed"));   // a file
    PB_CHECK_EQ(e.picker->chooseFolder(e.p("Nowhere")).value("errorKey").toString().toStdString(), std::string("reader_folder_missing"));
}

PB_TEST(new_folder_creates_one_folder_through_the_core_and_refuses_what_the_core_refuses) {
    Env e;
    e.picker->setFolderMode(true);
    const QVariantMap made = e.picker->createFolder(e.p("Downloads"), QStringLiteral("Outbox"));
    PB_CHECK(made.value("ok").toBool());
    PB_CHECK_EQ(made.value("path").toString().toStdString(), e.root + "/Downloads/Outbox");
    PB_CHECK_EQ(e.names(e.p("Downloads")).join(",").toStdString(), std::string("Outbox,Series"));
    struct stat st;
    PB_CHECK(::stat((e.root + "/Downloads/Outbox").c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    PB_CHECK_EQ(e.picker->createFolder(e.p("Downloads"), QStringLiteral("outbox")).value("errorKey").toString().toStdString(), std::string("name_exists"));
    for (const char* bad : {"..", "a/b", ".hidden", "", "CON"})
        PB_CHECK_EQ(e.picker->createFolder(e.p("Downloads"), QString::fromUtf8(bad)).value("errorKey").toString().toStdString(), std::string("name_forbidden"));
    PB_CHECK_EQ(e.picker->createFolder(QStringLiteral("/etc"), QStringLiteral("Outbox")).value("errorKey").toString().toStdString(), std::string("source_not_allowed"));
    PB_CHECK_EQ(e.picker->createFolder(e.p("system/pbcloud"), QStringLiteral("Outbox")).value("errorKey").toString().toStdString(), std::string("source_not_allowed"));
    PB_CHECK_EQ(e.names(e.p("Downloads")).join(",").toStdString(), std::string("Outbox,Series"));   // nothing else appeared
}

PB_TEST(browsing_in_folder_mode_changes_nothing_on_the_card) {
    Env e;
    e.picker->setFolderMode(true);
    auto count = [&]() {
        int n = 0;
        for (const char* d : {"", "Downloads", "Downloads/Series", "system", "PBCloud", "Notes"}) n += static_cast<int>(e.picker->listDirectory(e.p(d)).size());
        return n;
    };
    const int before = count();
    for (int i = 0; i < 3; ++i) PB_CHECK_EQ(count(), before);
    PB_CHECK(e.picker->chooseFolder(e.p("Downloads")).value("ok").toBool());
    PB_CHECK_EQ(count(), before);
}

PB_TEST_MAIN()
