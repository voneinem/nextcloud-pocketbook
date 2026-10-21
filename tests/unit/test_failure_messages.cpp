// Every cause key the app can show has a plain message and a "what to do" line, in English in qml/Problems.qml and in German in qml/strings_de.tsv with
// the same placeholders, and none of them can carry an address, a user name or a name from the server (specs/002-manage-server-files US7, FR-044, SC-010).
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "pbc/error_keys.h"
#include "test_support.h"

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// "case "a": case "b": return qsTr("text")" inside the function that starts at `marker`, up to the next function.
std::map<std::string, std::string> messages_of(const std::string& qml, const std::string& marker) {
    std::map<std::string, std::string> out;
    const size_t start = qml.find(marker);
    if (start == std::string::npos) return out;
    size_t end = qml.find("function ", start + marker.size());
    if (end == std::string::npos) end = qml.size();
    const std::string body = qml.substr(start, end - start);
    std::vector<std::string> pending;
    std::istringstream lines(body);
    std::string line;
    const std::regex key_re("case \"([a-z_0-9]+)\":");
    const std::regex text_re("return qsTr\\(\"((?:[^\"\\\\]|\\\\.)*)\"\\)");
    while (std::getline(lines, line)) {
        for (std::sregex_iterator it(line.begin(), line.end(), key_re), e; it != e; ++it) pending.push_back((*it)[1]);
        std::smatch m;
        if (std::regex_search(line, m, text_re)) {
            for (const std::string& k : pending) out[k] = m[1];
            pending.clear();
        }
    }
    return out;
}

std::set<std::string> placeholders(const std::string& text) {
    std::set<std::string> out;
    for (size_t i = 0; i + 1 < text.size(); ++i)
        if (text[i] == '%' && (text[i + 1] == 'n' || (text[i + 1] >= '1' && text[i + 1] <= '9'))) out.insert(text.substr(i, 2));
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

// the German table: English text -> German text (the "||" of a plural is kept inside)
std::map<std::string, std::string> german_table() {
    std::map<std::string, std::string> out;
    std::istringstream in(read_file(std::string(PBC_SOURCE_DIR) + "/qml/strings_de.tsv"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        out[unescape(line.substr(0, tab))] = unescape(line.substr(tab + 1));
    }
    return out;
}

// keys that are internal and never shown to the user as a message of their own
bool internal_key(const std::string& k) { return k == "cancelled" || k == "write_failed" || k == "starttls_unavailable"; }

}  // namespace

PB_TEST(every_cause_key_has_a_message_and_a_what_to_do_line) {
    const std::string qml = read_file(std::string(PBC_SOURCE_DIR) + "/qml/Problems.qml");
    const auto titles = messages_of(qml, "function title(");
    const auto actions = messages_of(qml, "function action(");
    PB_CHECK(!titles.empty() && !actions.empty());
    std::string missing;
    for (const std::string& key : pbc::all_error_keys()) {
        if (internal_key(key)) continue;
        if (!titles.count(key)) missing += " title:" + key;
        if (!actions.count(key)) missing += " action:" + key;
    }
    if (!missing.empty()) PB_FAIL_AT("keys without a message or a \"what to do\" line:" + missing);
}

PB_TEST(every_message_has_a_german_text_with_the_same_placeholders) {
    const std::string qml = read_file(std::string(PBC_SOURCE_DIR) + "/qml/Problems.qml");
    const auto table = german_table();
    for (const char* marker : {"function title(", "function action("}) {
        for (const auto& kv : messages_of(qml, marker)) {
            const std::string english = unescape(kv.second);
            const auto de = table.find(english);
            if (de == table.end()) PB_FAIL_AT("no German text for the message of " + kv.first);
            std::set<std::string> german;
            std::string rest = de->second;
            for (size_t cut; (cut = rest.find("||")) != std::string::npos; rest = rest.substr(cut + 2)) {
                for (const auto& p : placeholders(rest.substr(0, cut))) german.insert(p);
            }
            for (const auto& p : placeholders(rest)) german.insert(p);
            if (placeholders(english) != german) PB_FAIL_AT("the placeholders of the German text differ for " + kv.first);
        }
    }
}

PB_TEST(no_message_carries_an_address_a_user_name_or_a_server_name) {
    const std::string qml = read_file(std::string(PBC_SOURCE_DIR) + "/qml/Problems.qml");
    for (const char* marker : {"function title(", "function action("}) {
        for (const auto& kv : messages_of(qml, marker)) {
            std::string text = kv.second;
            for (size_t at; (at = text.find("https://")) != std::string::npos;) text.erase(at, 8);   // the word "https://" in an instruction is no address
            PB_CHECK(text.find("://") == std::string::npos);
            PB_CHECK(kv.second.find('@') == std::string::npos);
            PB_CHECK(kv.second.find("%1") == std::string::npos);   // a message is a fixed sentence: nothing is filled in from the server
            PB_CHECK(kv.second.find("%2") == std::string::npos);
        }
    }
}

PB_TEST_MAIN()
