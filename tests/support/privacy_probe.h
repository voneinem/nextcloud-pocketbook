#pragma once
// The harness of the share links privacy tests (specs/004-share-links, research R-9): it captures everything the app writes - the log sink,
// every file below the data folder (the database, its journal, settings, caches, error records) - and searches it for a needle. A needle
// that must not exist anywhere (a password) or that may exist only where the screen asked for it (a link). Test infrastructure.

#include <dirent.h>
#include <sys/stat.h>

#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "pbc/log.h"
#include "test_support.h"

namespace pbtest {

class PrivacyProbe {
public:
    PrivacyProbe() {
        pbc::log::set_sink([this](const std::string& line) { lines_.push_back(line); });
    }
    ~PrivacyProbe() { pbc::log::reset_sink(); }
    PrivacyProbe(const PrivacyProbe&) = delete;
    PrivacyProbe& operator=(const PrivacyProbe&) = delete;

    const std::vector<std::string>& log_lines() const { return lines_; }
    std::string log_text() const {
        std::string all;
        for (const std::string& l : lines_) all += l + "\n";
        return all;
    }
    // every file below `dir` (names and bytes), the way a person could read the card
    static std::string tree_text(const std::string& dir) {
        std::string all;
        collect(dir, &all);
        return all;
    }
    // Fails the test (with `what` and where it was found) when `needle` occurs in the log or in the data folder.
    void expect_absent(const std::string& needle, const std::string& data_dir, const std::string& what) const {
        if (needle.empty()) PB_FAIL_AT("expect_absent needs a needle: " + what);
        if (log_text().find(needle) != std::string::npos) PB_FAIL_AT(what + ": found in the log");
        if (tree_text(data_dir).find(needle) != std::string::npos) PB_FAIL_AT(what + ": found in a file or in the database below the data folder");
    }
    // The same for text the test collected elsewhere (messages, rows, the clipboard setter, exported text).
    static void expect_absent_in(const std::string& needle, const std::string& text, const std::string& what) {
        if (needle.empty()) PB_FAIL_AT("expect_absent_in needs a needle: " + what);
        if (text.find(needle) != std::string::npos) PB_FAIL_AT(what + ": found");
    }
    // The needle may be in `allowed_text` (where the screen asked for it) but nowhere in the log or the data folder.
    void expect_only_in(const std::string& needle, const std::string& allowed_text, const std::string& data_dir, const std::string& what) const {
        if (allowed_text.find(needle) == std::string::npos) PB_FAIL_AT(what + ": not found where it must be");
        expect_absent(needle, data_dir, what);
    }

private:
    static void collect(const std::string& dir, std::string* all) {
        DIR* d = ::opendir(dir.c_str());
        if (!d) return;
        while (dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string path = dir + "/" + name;
            struct stat st;
            if (::lstat(path.c_str(), &st) != 0) continue;
            *all += name + "\n";
            if (S_ISDIR(st.st_mode)) {
                collect(path, all);
            } else if (S_ISREG(st.st_mode)) {
                std::ifstream f(path, std::ios::binary);
                all->append(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
        }
        ::closedir(d);
    }
    std::vector<std::string> lines_;
};

}  // namespace pbtest
