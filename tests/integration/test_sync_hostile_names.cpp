// Names a server may hold inside a paired folder never reach a path outside the pair's reader folder and never replace another entry (SC-012,
// specs/003-folder-sync R-6): they become safe names, get a suffix when the card cannot tell them apart, or are skipped with their reason.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "download_env.h"
#include "pbc/sync_runner.h"
#include "pbc/sync_rules.h"
#include "pbc/sync_scan.h"
#include "pbc/sync_types.h"
#include "pbc/utf8.h"

PBC_FAKE_SERVER_TEST();

using pbtest::DlEnv;

namespace {

struct HostileEnv : DlEnv {
    std::string reader;
    int64_t pair = 0;
    std::unique_ptr<pbc::SyncRunner> runner;
    HostileEnv() {
        // the pair folder sits in a folder of its own, so that anything written beside it is noticed
        ::mkdir(dir.file("card").c_str(), 0700);
        reader = dir.file("card/Reader");
        ::mkdir(reader.c_str(), 0700);
        pbc::SourceRules rules;
        rules.root = dir.file("card");
        pbc::SyncPair p = pbc::default_pair(server.base_url(), "demo");
        p.remote_folder = "/Books";
        p.local_folder = reader;
        pair = store->sync_pair_add(p).value;
        runner = std::make_unique<pbc::SyncRunner>(*store, server, *dl, storage, clock, rules);
    }
    pbc::SyncResult sync() {
        pbtest::NoCancel c;
        auto plan = runner->compare(pair, "tap", c);
        if (!plan.ok()) PB_FAIL_AT("compare failed: " + plan.error.key);
        auto r = runner->run(plan.value, c);
        if (!r.ok()) PB_FAIL_AT("run failed: " + r.error.key);
        return r.value;
    }
    std::vector<std::string> below(const std::string& base) const {
        std::vector<std::string> out;
        pbtest::list_tree(base, "", &out);
        std::sort(out.begin(), out.end());
        return out;
    }
};

std::string fold(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

PB_TEST(hostile_names_never_leave_the_pair_folder_and_never_replace_another_entry) {
    HostileEnv e;
    const std::vector<std::string> names = {
        "plain.epub",
        "control\x01name.epub",                      // a control character
        "caf\xC3\xA9.epub",                           // NFC
        "cafe\xCC\x81.epub",                          // NFD: the same to the eye
        "Case.epub", "case.epub", "CASE.epub",        // the card cannot tell them apart
        std::string(300, 'n') + ".epub",              // longer than a FAT name
        "slash\xE2\x88\x95name.epub",                 // U+2215, no separator
        "CON.txt", "nul", "aux.epub",                 // names other systems reserve
        "trailing.dot.", "trailing space ",
        "a:b|c?d*e.epub",
        "\xE2\x80\xAE" "rtl-override.epub",
    };
    std::map<std::string, std::string> content;
    int seed = 1;
    for (const std::string& n : names) {
        content[n] = "content of " + std::to_string(seed++);
        e.server.add_file("/Books/" + n, content[n]);
    }
    e.server.add_file("/Books/Sub/" + std::string("..") + "x.epub", "dots in a name, not a segment");
    e.server.add_raw_member("/Books", "..");
    e.server.add_raw_member("/Books", "a%2Fb.epub");
    e.server.add_raw_member("/Books", "%2E%2E");
    const auto outside_before = e.below(e.dir.file("card"));

    e.sync();
    // nothing beside the pair folder
    std::vector<std::string> card;
    pbtest::list_tree(e.dir.file("card"), "", &card);
    for (const auto& f : card) PB_CHECK(f.compare(0, 7, "Reader/") == 0);
    PB_CHECK(outside_before.empty());
    // every recorded name is a plain name inside the folder, distinct from the others when the card folds them, and its bytes are its own
    std::set<std::string> folded;
    const auto records = e.store->sync_records(e.pair);
    PB_CHECK(!records.empty());
    for (const auto& r : records) {
        PB_CHECK(!pbc::join_path_checked(e.reader, r.local_rel).empty());
        PB_CHECK(r.local_rel.find("..") == std::string::npos || r.rel.find("..") != std::string::npos);   // dots only where the server's name had them
        PB_CHECK(r.local_rel.find('\x01') == std::string::npos);
        PB_CHECK(r.local_rel[0] != '.');
        PB_CHECK(folded.insert(pbc::case_fold(r.local_rel)).second);   // no two records claim one name
        std::ifstream f(e.reader + "/" + r.local_rel, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (content.count(r.rel)) PB_CHECK_EQ(got, content[r.rel]);   // each name kept its own content
    }
    // the three spellings of "case" and the two of "café" all arrived, each with its own bytes
    int cases = 0, cafes = 0;
    for (const auto& r : records) {
        if (fold(r.rel) == "case.epub") ++cases;
        if (r.rel.compare(0, 3, "caf") == 0) ++cafes;
    }
    PB_CHECK_EQ(cases, 3);
    PB_CHECK_EQ(cafes, 2);
    // nothing was put anywhere under the raw hostile names
    for (const auto& f : e.below(e.reader)) {
        PB_CHECK(f.find("a/b.epub") == std::string::npos);
        PB_CHECK(f.compare(0, 3, "../") != 0);
    }
    // a second sync is quiet: the same names map to the same files
    pbtest::NoCancel c;
    const auto plan = e.runner->compare(e.pair, "tap", c);
    PB_CHECK(plan.ok());
    PB_CHECK_EQ(e.store->sync_plan(plan.value)->totals.n_download, 0);
}

PB_TEST(a_name_the_card_cannot_hold_is_reported_with_its_reason_and_the_rest_goes_on) {
    HostileEnv e;
    e.server.add_file("/Books/ok.epub", "fine");
    e.server.add_file("/Books/Sub/inside.epub", "the card has a file where this folder would go");
    e.server.add_file("/Books/.DS_Store", "hidden on the server");
    e.server.add_file("/Books/Sub2/.hidden/x.epub", "inside a hidden folder");
    std::ofstream(e.reader + "/Sub") << "a file, not a folder";
    pbtest::NoCancel c;
    const auto plan = e.runner->compare(e.pair, "tap", c);
    PB_CHECK(plan.ok());
    std::map<std::string, std::string> reasons;
    for (const auto& en : e.store->sync_plan_page(plan.value, "skips", 0, 100).rows) reasons[en.rel] = en.reason;
    PB_CHECK_EQ(reasons["Sub/inside.epub"], std::string("path_unsafe"));   // a file where a folder is needed
    PB_CHECK_EQ(reasons[".DS_Store"], std::string("hidden_name"));
    PB_CHECK_EQ(reasons["Sub2/.hidden/x.epub"], std::string("hidden_name"));
    const auto result = e.runner->run(plan.value, c);
    PB_CHECK(result.ok());
    PB_CHECK_EQ(result.value.downloaded, 1);   // only ok.epub
    PB_CHECK_EQ(e.below(e.reader).size(), size_t(2));   // ok.epub and the user's own file "Sub"
}

PB_TEST_MAIN()
