// A fake Nextcloud server over real HTTPS on localhost for trying the host app by hand (scripts/run-host-demo.sh): the stateful fake of the
// tests behind a TLS server whose throw-away certificate authority is written to <work-dir>/ca.pem (the demo copies it to custom-ca.pem).
//
//   pbc_fake_nextcloud <work-dir>
//
// Prints "READY <https-url> <user> <app-password> <ca-file>" and then reads commands from stdin:
//   approve        the user "approved" the browser login that is waiting (Login Flow v2)
//   touch <path>   change a file on the server, so that the next Update finds it newer
//   lose-next-answer [METHOD [part]]   the next write is done but its answer is lost (feature 002)
//   drop-after <bytes>   cut the next upload body longer than that; quota <bytes>; lock <path>; no-trash; ignore-conditions
//   remove <path>  delete a file or a folder on the server (feature 003: a deletion the sync must only propose)
//   partial <folder> [mode]   from now on the listing of that folder fails: partial (default), truncated, timeout, error; "partial <folder> off" clears
//   ls-etag [path]  print the ETag of a path (default: the root) and of its children
//   drop           the next download loses its connection after half of the file
//   offline|online switch the server off and on
//   quit
// NOT a real server: everything it does is what the tests assume (docs/live-server-checklist.md).

#include <atomic>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include "fake_nextcloud_server.h"
#include "tls_test_server.h"

namespace {

std::string base64_decode(const std::string& in) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    uint32_t val = 0;   // at most 24 bits are ever kept
    int bits = 0;
    for (unsigned char c : in) {
        const size_t pos = chars.find(static_cast<char>(c));
        if (pos == std::string::npos) break;   // '=' or anything else ends the data
        val = ((val << 6) | static_cast<uint32_t>(pos)) & 0xFFFFFFu;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
        }
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// the raw bytes of a request -> a Request for the fake
pbc::Request parse_request(const std::string& raw, const std::string& base_url) {
    pbc::Request r;
    const size_t head_end = raw.find("\r\n\r\n");
    std::istringstream head(raw.substr(0, head_end));
    std::string line, version;
    std::getline(head, line);
    std::istringstream first(line);
    std::string target;
    first >> r.method >> target >> version;
    r.url = base_url + target;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(0, 1);
        r.headers.push_back({line.substr(0, colon), value});
    }
    r.body = head_end == std::string::npos ? std::string() : raw.substr(head_end + 4);
    const std::string auth = pbc::header_value(r.headers, "Authorization");
    if (lower(auth).compare(0, 6, "basic ") == 0) {
        const std::string up = base64_decode(auth.substr(6));
        const size_t colon = up.find(':');
        if (colon != std::string::npos) {
            r.username = up.substr(0, colon);
            r.password = up.substr(colon + 1);
        }
    }
    return r;
}

void fill_tree(pbtest::FakeNextcloud& s) {
    s.add_synthetic_file("/Books/Moby Dick.epub", 800 * 1024, 3, 1790000000);
    s.add_synthetic_file("/Books/Emma.epub", 300 * 1024, 5, 1789000000);
    s.add_file("/Books/Notes.txt", "Notes on the books.\n", 1788000000);
    s.add_file("/Books/Novels/Dracula.txt", "Dracula, a short sample.\n", 1787000000);
    s.add_file("/Books/Novels/Persuasion.txt", "Persuasion, a short sample.\n", 1787000100);
    s.add_synthetic_file("/Documents/Report 2026.pdf", 2 * 1024 * 1024, 9, 1790100000);
    s.add_file("/Documents/notes.txt", "Meeting notes.\n", 1790200000);
    s.add_file("/readme.txt", "Welcome to the demo server.\n", 1780000000);
    for (int i = 1; i <= 1500; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "/Many/file%04d.txt", i);
        s.add_file(name, "x", 1780000000 + i);
    }
    s.add_synthetic_file("/Big/large.bin", 120ll * 1024 * 1024, 11, 1785000000);   // above the 100 MiB warning
    s.add_folder("/Vault");
    s.set_encrypted("/Vault", true);
    s.add_folder("/Restricted");
    s.set_permissions("/Restricted", "RDNVCK");   // no read permission
    s.set_favorite("/Books/Moby Dick.epub", true);
    s.set_favorite("/Documents/notes.txt", true);
    s.set_quota(1500ll * 1024 * 1024, 8ll * 1024 * 1024 * 1024);
    // share links (feature 004): a folder with a link, a user, a group and a share of another kind, and a link made by someone else
    pbtest::FakeNextcloud::Share link;
    link.path = "/Books";
    link.permissions = 1;
    link.label = "Reading list";
    s.add_share(link);
    pbtest::FakeNextcloud::Share user;
    user.type = 0;
    user.path = "/Books";
    user.with = "bob";
    user.with_display = "Bob Example";
    s.add_share(user);
    pbtest::FakeNextcloud::Share group = user;
    group.type = 1;
    group.with = "team";
    group.with_display = "Team Example";
    s.add_share(group);
    pbtest::FakeNextcloud::Share mail = user;
    mail.type = 4;
    mail.with = "guest@example.org";
    mail.with_display = "guest@example.org";
    s.add_share(mail);
    pbtest::FakeNextcloud::Share foreign = link;
    foreign.owner = "carol";
    foreign.label = "Carol's link";
    s.add_share(foreign);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: pbc_fake_nextcloud <work-dir>\n";
        return 2;
    }
    const std::string work = argv[1];
    // the throw-away authority and a certificate for localhost, in <work-dir>
    pbtest::TmpDir pki_dir;
    pbtest::TestPki pki(pki_dir, "democa");
    if (!pki.available()) {
        std::cerr << "the openssl command line tool is needed\n";
        return 2;
    }
    const auto leaf = pki.issue("localhost-cert", "DNS:localhost,IP:127.0.0.1");
    if (!leaf.ok) return 2;
    const std::string ca_out = work + "/ca.pem";
    if (!pbtest::run("cp " + pki.ca_cert() + " " + ca_out)) return 2;

    std::mutex mu;
    std::unique_ptr<pbtest::FakeNextcloud> fake;   // created when the port is known: its address is part of every answer
    std::atomic<bool> drop_next{false};
    std::string base_url;

    pbtest::TlsTestServer server(leaf.cert, leaf.key, pbtest::TlsTestServer::StreamHandler([&](const std::string& raw, const std::function<bool(const char*, size_t)>& write) {
        pbc::Response resp;
        {
            std::lock_guard<std::mutex> l(mu);
            resp = fake->perform(parse_request(raw, base_url));
        }
        if (resp.status == 0) return;   // unreachable: close without an answer
        std::string head = "HTTP/1.1 " + std::to_string(resp.status) + " X\r\n";
        for (const auto& h : resp.headers) {
            const std::string k = lower(h.first);
            if (k == "content-length" || k == "connection" || k == "transfer-encoding") continue;
            head += h.first + ": " + h.second + "\r\n";
        }
        head += "Content-Length: " + std::to_string(resp.body.size()) + "\r\nConnection: close\r\n\r\n";
        if (!write(head.data(), head.size())) return;
        size_t limit = resp.body.size();
        if (raw.compare(0, 4, "GET ") == 0 && resp.body.size() > 64 * 1024 && drop_next.exchange(false)) limit = resp.body.size() / 2;   // the connection "drops"
        write(resp.body.data(), limit);
    }));
    if (!server.running()) {
        std::cerr << "the server could not start\n";
        return 2;
    }
    base_url = "https://localhost:" + std::to_string(server.port());
    fake = std::make_unique<pbtest::FakeNextcloud>("localhost:" + std::to_string(server.port()));
    fake->set_server_time(static_cast<int64_t>(std::time(nullptr)));
    fill_tree(*fake);
    std::cout << "READY " << base_url << " " << fake->login_name() << " " << fake->app_password() << " " << ca_out << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        std::lock_guard<std::mutex> l(mu);
        std::istringstream in(line);
        std::string cmd, arg;
        in >> cmd;
        std::getline(in, arg);
        while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
        if (cmd == "quit") break;
        if (cmd == "approve") {
            fake->approve_login(fake->login_name(), fake->app_password());
            std::cout << "approved: the browser login can finish now" << std::endl;
        } else if (cmd == "touch" && !arg.empty()) {
            if (fake->has(arg) && !fake->node(arg).folder) {
                fake->set_file(arg, fake->node(arg).content + "\nchanged on the server\n", static_cast<int64_t>(std::time(nullptr)));
                std::cout << "changed " << arg << std::endl;
            } else {
                std::cout << "no such file: " << arg << std::endl;
            }
        } else if (cmd == "remove" && !arg.empty()) {
            if (fake->has(arg)) {
                fake->remove(arg);
                std::cout << "removed " << arg << std::endl;
            } else {
                std::cout << "no such path: " << arg << std::endl;
            }
        } else if (cmd == "partial" && !arg.empty()) {
            std::istringstream a(arg);
            std::string folder, mode = "partial";
            a >> folder >> mode;
            fake->set_listing_failure(folder, mode == "off" ? std::string() : mode);
            std::cout << "the listing of " << folder << (mode == "off" ? " works again" : " now fails: " + mode) << std::endl;
        } else if (cmd == "ls-etag") {
            const std::string path = arg.empty() ? std::string("/") : arg;
            if (!fake->has(path)) {
                std::cout << "no such path: " << path << std::endl;
            } else {
                std::cout << path << " " << fake->etag_of(path) << std::endl;
                for (const std::string& c : fake->children(path)) std::cout << "  " << c << " " << fake->etag_of(c) << std::endl;
            }
        } else if (cmd == "drop") {
            drop_next = true;
            std::cout << "the next download will lose its connection half way" << std::endl;
        } else if (cmd == "offline" || cmd == "online") {
            fake->set_offline(cmd == "offline");
            std::cout << "server is " << cmd << std::endl;
        } else if (cmd == "lose-next-answer") {   // the next write is done, but the answer never arrives: the app must call it an unknown outcome
            std::istringstream a(arg);
            std::string method = "PUT", part;
            a >> method >> part;
            fake->lose_next_answer(method, part);
            std::cout << "the answer of the next " << method << " is lost after the server did it" << std::endl;
        } else if (cmd == "drop-after" && !arg.empty()) {   // the next request body longer than this many bytes is cut: nothing is stored
            fake->drop_after_bytes(std::atoll(arg.c_str()));
            std::cout << "the next upload body longer than " << arg << " bytes is cut" << std::endl;
        } else if (cmd == "quota" && !arg.empty()) {
            fake->set_quota_limit(std::atoll(arg.c_str()));
            std::cout << "the quota is " << arg << " bytes" << std::endl;
        } else if (cmd == "lock" && !arg.empty()) {
            if (fake->has(arg)) {
                fake->lock(arg);
                std::cout << "locked " << arg << std::endl;
            } else {
                std::cout << "no such path: " << arg << std::endl;
            }
        } else if (cmd == "no-trash") {
            fake->set_trash(false);
            std::cout << "the server has no trash now (deleting would be permanent)" << std::endl;
        } else if (cmd == "trash") {
            fake->set_trash(true);
            std::cout << "the server has a trash again" << std::endl;
        } else if (cmd == "share-rules") {   // share-rules <sharing|public|password|expiry|upload> <on|off|N>
            std::istringstream a(arg);
            std::string what, value;
            a >> what >> value;
            const bool on = value != "off";
            if (what == "sharing") fake->set_sharing_enabled(on);
            else if (what == "public") fake->set_public_links_enabled(on);
            else if (what == "password") fake->set_password_enforced(on);
            else if (what == "upload") fake->set_public_upload(on);
            else if (what == "expiry") fake->set_expiry(on, on, std::atoi(value.c_str()) > 0 ? std::atoi(value.c_str()) : 7);
            else {
                std::cout << "usage: share-rules <sharing|public|password|upload> <on|off>   or   share-rules expiry <days|off>" << std::endl;
                continue;
            }
            std::cout << "share rule " << what << " is now " << (value.empty() ? "on" : value) << std::endl;
        } else if (cmd == "share-list") {   // the shares of a path (ids, kinds, permissions; no addresses)
            const std::string path = arg.empty() ? std::string("/Books") : arg;
            int n = 0;
            for (int64_t id = 1000; id < 1000 + 5000; ++id) {
                const auto* sh = fake->find_share(id);
                if (!sh || sh->path != path) continue;
                std::cout << "  #" << sh->id << " type " << sh->type << " perms " << sh->permissions << " owner " << sh->owner
                          << (sh->password.empty() ? "" : " password") << (sh->expiration.empty() ? "" : " expires " + sh->expiration) << std::endl;
                ++n;
            }
            std::cout << n << " shares on " << path << std::endl;
        } else if (cmd == "unshare" && !arg.empty()) {   // removes a share behind the app's back
            fake->remove_share(std::atoll(arg.c_str()));
            std::cout << "removed share " << arg << std::endl;
        } else if (cmd == "ignore-conditions") {
            fake->ignore_conditions_on_assemble(true);
            std::cout << "the final MOVE of a chunked upload ignores its conditions" << std::endl;
        } else if (!cmd.empty()) {
            std::cout << "commands: approve, touch <path>, remove <path>, partial <folder> [mode], ls-etag [path], drop, offline, online, lose-next-answer [METHOD [part]], drop-after <bytes>, quota <bytes>, "
                         "lock <path>, no-trash, trash, ignore-conditions, share-rules <what> <value>, share-list [path], unshare <id>, quit" << std::endl;
        }
    }
    return 0;
}
