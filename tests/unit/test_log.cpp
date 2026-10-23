// T019: the logger accepts only event codes, HTTP status, counts and file paths; sensitive
// field names are redacted (constitution Principle III).
#include <dirent.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../../core/src/log_codes.h"
#include "pbc/log.h"
#include "test_support.h"

namespace log = pbc::log;

namespace {
struct Capture {
    std::vector<std::string> lines;
    Capture() {
        log::set_sink([this](const std::string& line) { lines.push_back(line); });
    }
    ~Capture() { log::reset_sink(); }
    std::string all() const {
        std::string s;
        for (const auto& l : lines) s += l + "\n";
        return s;
    }
};
bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
}  // namespace

PB_TEST(logs_event_status_counts_and_path) {
    Capture c;
    log::event("sync.finished", {log::status(200), log::count("pushed", 3), log::path("/mnt/ext1/system/pbcloud/pbcloud.db")});
    PB_CHECK_EQ(c.lines.size(), static_cast<size_t>(1));
    PB_CHECK(contains(c.all(), "sync.finished"));
    PB_CHECK(contains(c.all(), "status=200"));
    PB_CHECK(contains(c.all(), "pushed=3"));
    PB_CHECK(contains(c.all(), "path=/mnt/ext1/system/pbcloud/pbcloud.db"));
}

PB_TEST(sensitive_field_names_are_redacted) {
    Capture c;
    log::event("account.saved", {log::text("password", "hunter2"), log::text("USERNAME", "alice"),
                                 log::text("content", "my secret diary"), log::text("title", "My diary"),
                                 log::text("Authorization", "Basic YWxpY2U6aHVudGVyMg=="),
                                 log::text("token", "abc123"), log::text("url", "https://alice:pw@host/")});
    const std::string out = c.all();
    for (const char* leaked : {"hunter2", "alice", "diary", "YWxpY2U6", "abc123", "pw@host"})
        PB_CHECK(!contains(out, leaked));
    PB_CHECK(contains(out, "password=[redacted]"));
}

PB_TEST(server_data_and_user_input_of_the_files_client_are_redacted) {
    Capture c;
    log::event("listing.refresh", {log::text("href", "/remote.php/dav/files/demo/Books/secret.epub"),
                                   log::text("term", "tax return 2025"), log::text("login", "demo"),
                                   log::text("query", "q=1"), log::text("search", "private plans"),
                                   log::text("link", "https://cloud.example.org/s/abc"), log::text("remote_name", "diary.txt")});
    const std::string out = c.all();
    for (const char* leaked : {"secret.epub", "tax return", "private plans", "cloud.example.org", "diary.txt", "q=1"})
        PB_CHECK(!contains(out, leaked));
    PB_CHECK(contains(out, "href=[redacted]"));
    PB_CHECK(contains(out, "term=[redacted]"));
    PB_CHECK(contains(out, "search=[redacted]"));
    PB_CHECK(contains(out, "link=[redacted]"));
}

PB_TEST(versions_checksums_and_upload_ids_of_the_write_side_are_redacted_and_the_codes_stay_plain) {
    Capture c;
    log::event("upload.done", {log::text("etag", "\"5f3c0a\""), log::text("src_checksum", "ab12cd34"), log::text("upload_id", "pbc-0123456789abcdef"),
                               log::text("dst_etag", "\"zz\""), log::count("items", 3), log::text("outcome", "success")});
    const std::string out = c.all();
    for (const char* leaked : {"5f3c0a", "ab12cd34", "0123456789abcdef", "zz"}) PB_CHECK(!contains(out, leaked));
    PB_CHECK(contains(out, "etag=[redacted]"));
    PB_CHECK(contains(out, "upload_id=[redacted]"));
    PB_CHECK(contains(out, "items=3"));
    PB_CHECK(contains(out, "outcome=success"));
    // no event code of the app can itself carry data: dotted lower-case words only
    for (const char* code : pbc::log::kAllowedCodes) {
        const std::string c2 = code;
        PB_CHECK(std::regex_match(c2, std::regex("[a-z]+(\\.[a-z_]+)+")));
    }
}

PB_TEST(the_sync_events_log_counts_ids_and_reasons_only) {
    Capture c;
    // what the sync modules may say: a pair id, a plan id, an entry sequence number, counts and an error key (as `reason`)
    log::event("sync.recover", {log::count("pairs", 2), log::count("plans", 1)});
    log::event("sync.recover", {log::count("pair", 7), log::count("plan", 42), log::count("seq", 3), log::text("reason", "storage_full"), log::count("downloaded", 5)});
    const std::string kept = c.all();
    for (const char* ok : {"pairs=2", "plans=1", "pair=7", "plan=42", "seq=3", "reason=storage_full", "downloaded=5"}) PB_CHECK(contains(kept, ok));
    // and whatever would name a file, a folder, a server or a version is redacted even if a careless call tried
    Capture d;
    log::event("sync.recover", {log::text("rel", "Books/secret.epub"), log::text("local_rel", "Books/secret (2).epub"), log::text("etag", "\"abc123\""),
                                log::text("checksum", "SHA256:deadbeef"), log::text("name", "secret.epub"), log::text("remote_folder", "/Private"),
                                log::text("local_folder", "/mnt/ext1/Private"), log::text("server", "https://cloud.example.org"),
                                log::text("path", "/mnt/ext1/Private/secret.epub"), log::text("address", "cloud.example.org")});
    const std::string out = d.all();
    for (const char* leaked : {"secret", "abc123", "deadbeef", "Private", "cloud.example.org"}) PB_CHECK(!contains(out, leaked));
    PB_CHECK(contains(out, "rel=[redacted]"));
    PB_CHECK(contains(out, "local_rel=[redacted]"));
    PB_CHECK(contains(out, "server=[redacted]"));
    PB_CHECK(contains(out, "remote_folder=[redacted]"));
}

PB_TEST(the_share_events_log_a_status_and_counts_only_and_a_careless_call_cannot_leak_a_secret) {
    Capture c;
    log::event("share.open", {log::status(200), log::count("rows", 3), log::count("skipped", 1), log::count("inherited", 0)});
    log::event("share.fail", {log::status(403), log::count("op", 1)});
    const std::string kept = c.all();
    for (const char* ok : {"share.open", "status=200", "rows=3", "skipped=1", "inherited=0", "share.fail", "status=403", "op=1"}) PB_CHECK(contains(kept, ok));
    // N8 forbids these calls in the share modules; if one slipped in anyway the logger redacts what is named like a secret
    Capture d;
    log::event("share.create", {log::text("link", "https://cloud.example.org/s/TOKEN123"), log::text("token", "TOKEN123"), log::text("password", "Zx9-secret-pw"),
                                log::text("url", "https://cloud.example.org/s/TOKEN123"), log::text("share_link", "https://cloud.example.org/s/TOKEN123"),
                                log::text("new_password", "Zx9-secret-pw")});
    const std::string out = d.all();
    for (const char* leaked : {"TOKEN123", "Zx9-secret-pw", "cloud.example.org"}) PB_CHECK(!contains(out, leaked));
}

PB_TEST(harmless_text_fields_are_kept) {
    Capture c;
    log::event("sync.finished", {log::text("outcome", "success"), log::text("error_class", "none")});
    PB_CHECK(contains(c.all(), "outcome=success"));
}

PB_TEST(newlines_in_values_cannot_forge_log_lines) {
    Capture c;
    log::event("x", {log::text("outcome", "ok\nFAKE LINE")});
    PB_CHECK_EQ(c.lines.size(), static_cast<size_t>(1));
    PB_CHECK(!contains(c.lines[0], "\n"));
}

PB_TEST(mail_and_calendar_words_are_redacted_as_whole_words) {
    Capture c;
    log::event("x", {log::text("host", "mail.example.org"), log::text("folder", "Projects/Secret"),
                     log::text("subject", "Layoffs"), log::text("from", "boss@example.org"),
                     log::text("to", "me@example.org"), log::text("sent_to", "you@example.org"),
                     log::text("recipient", "carol"), log::text("summary", "Dentist"),
                     log::text("attendee", "dave"), log::text("location", "Room 7"),
                     log::text("event.title", "Budget")});
    const std::string out = c.all();
    for (const char* leaked : {"example.org", "Secret", "Layoffs", "carol", "Dentist", "dave", "Room 7", "Budget"})
        PB_CHECK(!contains(out, leaked));
    PB_CHECK(contains(out, "host=[redacted]"));
    PB_CHECK(contains(out, "to=[redacted]"));
    PB_CHECK(contains(out, "sent_to=[redacted]"));
}

PB_TEST(words_inside_longer_names_and_typed_fields_are_unaffected) {
    Capture c;
    log::event("x", {log::text("total", "5"), log::text("tofu", "ok"), log::text("outcome", "success"),
                     log::text("fromage", "brie"), log::count("to_fetch", 7), log::count("host_count", 2),
                     log::count("folder", 3), log::status(404)});
    const std::string out = c.all();
    for (const char* kept : {"total=5", "tofu=ok", "outcome=success", "fromage=brie", "to_fetch=7", "host_count=2",
                             "folder=3", "status=404"})
        PB_CHECK(contains(out, kept));
}

// Every literal event code in the sources is listed in core/src/log_codes.h, and every listed code is used
// (a stale list would hide a new code that was never reviewed).
PB_TEST(event_codes_in_the_sources_are_exactly_the_listed_ones) {
    const std::regex call(R"re((?:log::event|log_failure)\(\s*"([A-Za-z0-9_.]+)")re");
    std::set<std::string> used;
    std::vector<std::string> dirs = {std::string(PBC_SOURCE_DIR) + "/core/src", std::string(PBC_SOURCE_DIR) + "/bridge/src",
                                     std::string(PBC_SOURCE_DIR) + "/app"};
    for (const std::string& dir : dirs) {
        DIR* d = opendir(dir.c_str());
        PB_CHECK(d != nullptr);
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() < 5 || name.compare(name.size() - 4, 4, ".cpp") != 0) continue;
            std::ifstream in(dir + "/" + name);
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string text = ss.str();
            for (std::sregex_iterator it(text.begin(), text.end(), call), end; it != end; ++it) used.insert((*it)[1]);
        }
        closedir(d);
    }
    std::set<std::string> listed(std::begin(pbc::log::kAllowedCodes), std::end(pbc::log::kAllowedCodes));
    for (const std::string& code : used)
        if (listed.count(code) != 1) PB_FAIL_AT("code not listed in log_codes.h: " + code);
    for (const std::string& code : listed)
        if (used.count(code) != 1) PB_FAIL_AT("listed code is unused: " + code);
    PB_CHECK(pbc::log::is_allowed_code("about.opened"));
    PB_CHECK(!pbc::log::is_allowed_code("password.leaked"));
}

PB_TEST_MAIN()
