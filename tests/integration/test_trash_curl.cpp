// The trash and versions requests through the real CurlTransport (specs/006-trash-and-versions, constitution I): libcurl talks TLS to a local server with its own CA; behind
// it sits the fake Nextcloud's trash and versions side. PROPFIND with a body, MOVE with its Destination and Overwrite headers, DELETE of one item, certificate verification
// on, and a redirect to another host that the transport does not follow. Needs `openssl`. Nothing here is verified against a real server.
#include <memory>
#include <mutex>
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/curl_transport.h"
#include "pbc/dav_common.h"
#include "pbc/trash_api.h"
#include "pbc/trash_rules.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

PBC_FAKE_SERVER_TEST();

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The raw request of the TLS server as a request of the fake Nextcloud, and the fake's answer as raw bytes.
std::string through_fake(pbtest::FakeNextcloud& fake, const std::string& raw, const std::string& redirect_part, const std::string& redirect_to, bool* saw_auth) {
    const size_t eol = raw.find("\r\n");
    const size_t sp1 = raw.find(' ');
    const size_t sp2 = raw.find(' ', sp1 + 1);
    pbc::Request req;
    req.method = raw.substr(0, sp1);
    const std::string path = raw.substr(sp1 + 1, sp2 - sp1 - 1);
    req.url = fake.base_url() + path;
    const size_t head_end = raw.find("\r\n\r\n");
    size_t pos = eol + 2;
    while (pos < head_end) {
        const size_t e = raw.find("\r\n", pos);
        const std::string line = raw.substr(pos, e - pos);
        pos = e + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = line.substr(0, colon), value = line.substr(colon + 2);
        const std::string l = lower(name);
        if (l == "authorization") {
            req.username = "demo";
            req.password = fake.app_password();
            if (saw_auth) *saw_auth = true;
        } else if (l == "destination") {   // the address the client used for the TLS server is the fake's own address here
            const size_t slash = value.find('/', value.find("://") + 3);
            req.headers.push_back({name, fake.base_url() + value.substr(slash)});
        } else if (l != "host" && l != "content-length" && l != "connection" && l != "accept") {
            req.headers.push_back({name, value});
        }
    }
    req.body = raw.substr(head_end + 4);
    if (!redirect_part.empty() && path.find(redirect_part) != std::string::npos && req.method == "GET")
        return "HTTP/1.1 307 Temporary Redirect\r\nLocation: " + redirect_to + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const pbc::Response r = fake.perform(req);
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " X\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n";
    for (const auto& h : r.headers) {
        if (lower(h.first) == "content-length" || lower(h.first) == "connection") continue;
        out += h.first + ": " + h.second + "\r\n";
    }
    return out + "\r\n" + r.body;
}

struct Over {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    pbtest::FakeNextcloud fake;
    pbc::SystemClock clock;
    std::mutex mu;
    std::string redirect_part, redirect_to;
    std::unique_ptr<pbtest::TlsTestServer> server;
    std::unique_ptr<pbc::CurlTransport> transport;
    std::unique_ptr<pbc::DavSession> session;
    std::unique_ptr<pbc::TrashApi> api;

    Over() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("good", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        fake.set_account("demo", "demo", "pw");
        server = std::make_unique<pbtest::TlsTestServer>(leaf.cert, leaf.key, [this](const std::string& raw) {
            std::lock_guard<std::mutex> lock(mu);
            return through_fake(fake, raw, redirect_part, redirect_to, nullptr);
        });
        PB_CHECK(server->running());
        transport = std::make_unique<pbc::CurlTransport>(pki.ca_cert());
        session = std::make_unique<pbc::DavSession>(*transport, "https://localhost:" + std::to_string(server->port()), "demo", "pw");
        api = std::make_unique<pbc::TrashApi>(*session, "demo", clock);
        fake.add_folder("/Books");
        fake.add_folder("/Other");
    }
};

}  // namespace

PB_TEST(the_whole_flow_works_over_tls_with_the_propfind_body_and_the_move_headers) {
    Over o;
    const std::string a = o.fake.add_trash_file("/Books/a.txt", "alpha", 1792497000);
    const std::string b = o.fake.add_trash_file("/Books/b.txt", "bravo!", 1792496900);
    const std::string c = o.fake.add_trash_file("/Books/c.txt", "charlie", 1792496800);
    PB_CHECK(o.api->fetch_availability().ok());
    const auto l = o.api->list_trash();
    PB_CHECK(l.ok());
    PB_CHECK_EQ(l.value.items.size(), size_t(3));
    PB_CHECK_EQ(l.value.items[0].original_dir, std::string("/Books"));
    PB_CHECK(o.api->probe_restore(l.value.items[0]).ok());
    // restore to the original place: Destination in the restore collection, Overwrite: F
    const auto r = o.api->restore_original(l.value.items[0]);
    PB_CHECK(r.error.ok() && !r.landed_elsewhere);
    PB_CHECK_EQ(o.fake.content_of("/Books/a.txt"), std::string("alpha"));
    // restore to a chosen place: Destination under the files
    pbc::RestoreTarget t;
    t.folder = "/Other";
    t.name = "moved b.txt";   // a name that needs encoding
    const auto r2 = o.api->restore_to(l.value.items[1], t);
    PB_CHECK(r2.error.ok());
    PB_CHECK_EQ(o.fake.content_of("/Other/moved b.txt"), std::string("bravo!"));
    // delete one item for good
    const auto d = o.api->delete_item(l.value.items[2]);
    PB_CHECK(d.error.ok());
    PB_CHECK_EQ(d.removed, 1);
    PB_CHECK(!o.fake.trashbin_has(c) && !o.fake.trashbin_has(a) && !o.fake.trashbin_has(b));
    bool saw_move_f = false, saw_delete_item = false;
    for (const auto& line : o.fake.log()) {
        if (line.method == "MOVE") {
            PB_CHECK_EQ(line.overwrite, std::string("F"));
            PB_CHECK(!line.destination.empty());
            saw_move_f = true;
        }
        if (line.method == "DELETE") saw_delete_item = line.path.size() > std::string("/remote.php/dav/trashbin/demo/trash/").size();
    }
    PB_CHECK(saw_move_f && saw_delete_item);
}

PB_TEST(the_versions_flow_works_over_tls) {
    Over o;
    o.fake.add_file("/Books/doc.txt", "current", 1760000300);
    o.fake.add_version("/Books/doc.txt", "v1", 1760000100);
    pbc::RemoteEntry file;
    file.path = "/Books/doc.txt";
    file.name = "doc.txt";
    file.kind = pbc::Kind::File;
    file.fileid = o.fake.nodes().at(file.path).fileid;
    const auto l = o.api->list_versions(file);
    PB_CHECK(l.ok());
    PB_CHECK_EQ(l.value.rows.size(), size_t(1));
    const auto r = o.api->restore_version(file, l.value, l.value.rows[0]);
    PB_CHECK(r.error.ok() && !r.previous_not_kept);
    PB_CHECK_EQ(o.fake.content_of("/Books/doc.txt"), std::string("v1"));
    bool saw = false;
    for (const auto& line : o.fake.log())
        if (line.method == "MOVE") saw = line.destination.find("/remote.php/dav/versions/demo/restore/target") != std::string::npos && line.overwrite == "F";
    PB_CHECK(saw);
}

PB_TEST(the_credentials_travel_as_basic_authentication_and_never_in_a_url) {
    Over o;
    bool saw_auth = false;
    std::string seen_url;
    o.server = std::make_unique<pbtest::TlsTestServer>(o.leaf.cert, o.leaf.key, [&](const std::string& raw) {
        std::lock_guard<std::mutex> lock(o.mu);
        seen_url += raw.substr(0, raw.find("\r\n")) + "\n";
        return through_fake(o.fake, raw, "", "", &saw_auth);
    });
    PB_CHECK(o.server->running());
    pbc::DavSession session(*o.transport, "https://localhost:" + std::to_string(o.server->port()), "demo", "pw");
    pbc::TrashApi api(session, "demo", o.clock);
    PB_CHECK(api.list_trash().ok());
    PB_CHECK(saw_auth);
    PB_CHECK(seen_url.find("pw@") == std::string::npos && seen_url.find('@') == std::string::npos);
}

PB_TEST(a_redirect_to_another_host_is_not_followed_and_nothing_is_restored) {
    Over o;
    o.fake.add_trash_file("/Books/a.txt", "a", 1792497000);
    // every PROPFIND on the trash is answered with a redirect to another host
    o.server = std::make_unique<pbtest::TlsTestServer>(o.leaf.cert, o.leaf.key, [&](const std::string& raw) {
        std::lock_guard<std::mutex> lock(o.mu);
        if (raw.compare(0, 8, "PROPFIND") == 0 && raw.find("/trashbin/") != std::string::npos)
            return std::string("HTTP/1.1 307 Temporary Redirect\r\nLocation: https://elsewhere.example.net/steal\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return through_fake(o.fake, raw, "", "", nullptr);
    });
    PB_CHECK(o.server->running());
    pbc::DavSession session(*o.transport, "https://localhost:" + std::to_string(o.server->port()), "demo", "pw");
    pbc::TrashApi api(session, "demo", o.clock);
    PB_CHECK(!api.list_trash().ok());
    PB_CHECK_EQ(o.fake.count("PROPFIND"), size_t(0));   // the redirect was never followed, nothing reached the fake
}

PB_TEST(an_untrusted_certificate_stops_every_request_of_the_feature) {
    Over o;
    o.fake.add_trash_file("/Books/a.txt", "a", 1792497000);
    pbc::CurlTransport strict(o.dir.file("no-such-ca.pem"));   // a transport that does not trust the test authority
    pbc::DavSession session(strict, "https://localhost:" + std::to_string(o.server->port()), "demo", "pw");
    pbc::TrashApi api(session, "demo", o.clock);
    PB_CHECK(!api.fetch_availability().ok());
    PB_CHECK(!api.list_trash().ok());
    pbc::TrashItem item;
    item.trash_name = "a.txt.d1792497000";
    item.name = "a.txt";
    item.original_known = true;
    item.original_dir = "/Books";
    item.original_name = "a.txt";
    PB_CHECK(!api.restore_original(item).error.ok());
    PB_CHECK(!api.delete_item(item).error.ok());
    PB_CHECK_EQ(o.fake.request_count(), size_t(0));
}

PB_TEST_MAIN()
