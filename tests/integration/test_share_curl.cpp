// The share requests through the real CurlTransport (specs/004-share-links, constitution I): libcurl talks TLS to a local server with its own CA; behind it
// sits the fake Nextcloud's shares side. POST, PUT and DELETE with form bodies, the three reads, certificate verification on, and a redirect to another host
// that the transport does not follow. Needs `openssl`. Nothing here is verified against a real server.
#include <memory>
#include <mutex>
#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/curl_transport.h"
#include "pbc/dav_common.h"
#include "pbc/share_api.h"
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
    std::unique_ptr<pbc::ShareApi> api;

    Over() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("good", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        server = std::make_unique<pbtest::TlsTestServer>(leaf.cert, leaf.key, [this](const std::string& raw) {
            std::lock_guard<std::mutex> lock(mu);
            return through_fake(fake, raw, redirect_part, redirect_to, nullptr);
        });
        PB_CHECK(server->running());
        transport = std::make_unique<pbc::CurlTransport>(pki.ca_cert());
        session = std::make_unique<pbc::DavSession>(*transport, "https://localhost:" + std::to_string(server->port()), "demo", fake.app_password());
        api = std::make_unique<pbc::ShareApi>(*session, clock, "demo");
        fake.add_folder("/Docs");
        fake.add_folder("/Docs/Reports");
    }
};

}  // namespace

PB_TEST(the_whole_flow_works_over_tls_with_form_bodies) {
    Over o;
    const auto rules = o.api->fetch_rules();
    PB_CHECK(rules.ok());
    const pbc::ShareTarget target{"/Docs/Reports", true};
    pbc::LinkRequest req;
    req.permission = pbc::LinkPermission::ReadUpload;
    req.password = pbc::SecretText{std::string("p&w=1 %")};
    req.expiry = std::string("2026-12-31");
    const auto created = o.api->create(target, rules.value, std::move(req));
    PB_CHECK(created.ok());
    PB_CHECK_EQ(o.fake.share_log().back().method, std::string("POST"));
    PB_CHECK_EQ(o.fake.share_log().back().body, std::string("path=%2FDocs%2FReports&shareType=3&permissions=15&password=p%26w%3D1%20%25&expireDate=2026-12-31"));
    PB_CHECK_EQ(o.fake.find_share(created.value.row.id)->password, std::string("p&w=1 %"));   // the server decoded the password exactly
    const auto list = o.api->list(target);
    PB_CHECK(list.ok());
    PB_CHECK_EQ(list.value.own.rows.size(), size_t(1));
    PB_CHECK(list.value.own.rows[0].has_password && list.value.own.rows[0].permission == pbc::LinkPermission::ReadUpload);
    const auto shown = pbc::snapshot_of(o.api->detail(created.value.row.id, true).value.row);
    pbc::LinkChange change;
    change.action = pbc::LinkChange::Action::ClearPassword;
    const auto changed = o.api->change(target, rules.value, created.value.row.id, shown, std::move(change));
    PB_CHECK(changed.ok());
    PB_CHECK_EQ(o.fake.share_log().back().method, std::string("PUT"));
    PB_CHECK_EQ(o.fake.share_log().back().body, std::string("password="));
    PB_CHECK(o.fake.find_share(created.value.row.id)->password.empty());
    PB_CHECK(o.api->remove_link(created.value.row.id, target, rules.value).ok());
    PB_CHECK_EQ(o.fake.share_log().back().method, std::string("DELETE"));
    PB_CHECK_EQ(o.fake.share_count(), size_t(0));
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
    pbc::DavSession session(*o.transport, "https://localhost:" + std::to_string(o.server->port()), "demo", o.fake.app_password());
    pbc::ShareApi api(session, o.clock, "demo");
    PB_CHECK(api.list({"/Docs/Reports", true}).ok());
    PB_CHECK(saw_auth);
    PB_CHECK(seen_url.find(o.fake.app_password()) == std::string::npos && seen_url.find('@') == std::string::npos);
}

PB_TEST(a_redirect_to_another_host_is_not_followed) {
    Over o;
    o.redirect_part = "/shares?path";
    o.redirect_to = "https://elsewhere.example.net/ocs/steal";
    const auto l = o.api->list({"/Docs/Reports", true});
    PB_CHECK(!l.ok());
    PB_CHECK_EQ(o.fake.share_log().size(), size_t(0));   // nothing reached the share resources of the fake
}

PB_TEST(an_untrusted_certificate_stops_every_share_request) {
    Over o;
    pbc::CurlTransport strict(o.dir.file("no-such-ca.pem"));   // a transport that does not trust the test authority
    pbc::DavSession session(strict, "https://localhost:" + std::to_string(o.server->port()), "demo", o.fake.app_password());
    pbc::ShareApi api(session, o.clock, "demo");
    PB_CHECK(!api.fetch_rules().ok());
    PB_CHECK(!api.list({"/Docs/Reports", true}).ok());
    PB_CHECK_EQ(o.fake.share_log().size(), size_t(0));
}

PB_TEST_MAIN()
