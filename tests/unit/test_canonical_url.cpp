// The address the user types (specs/001-nextcloud-files-client FR-004): https only, a bare host is completed with https://,
// everything else that could carry credentials or change the target is refused before any connection.
#include <string>

#include "pbc/nc_server.h"
#include "test_support.h"

namespace {
std::string canon(const std::string& typed) {
    const auto r = pbc::canonical_base_url(typed);
    return r.ok() ? r.value : "ERROR:" + r.error.key;
}
}  // namespace

PB_TEST(a_full_https_address_is_normalized) {
    PB_CHECK_EQ(canon("https://cloud.example.org"), std::string("https://cloud.example.org"));
    PB_CHECK_EQ(canon("https://cloud.example.org/"), std::string("https://cloud.example.org"));
    PB_CHECK_EQ(canon("HTTPS://Cloud.Example.ORG/"), std::string("https://cloud.example.org"));   // scheme and host are lower-cased
    PB_CHECK_EQ(canon("https://host:8443/sub/path/"), std::string("https://host:8443/sub/path"));
    PB_CHECK_EQ(canon("https://host/Sub/Path"), std::string("https://host/Sub/Path"));            // the path keeps its case
    PB_CHECK_EQ(canon("  https://cloud.example.org  "), std::string("https://cloud.example.org"));  // typing blanks around it
    PB_CHECK_EQ(canon("https://[::1]:8443/x"), std::string("https://[::1]:8443/x"));
    PB_CHECK_EQ(canon("https://192.0.2.7:8080"), std::string("https://192.0.2.7:8080"));
    PB_CHECK_EQ(canon("https://cloud.example.org/my%20cloud"), std::string("https://cloud.example.org/my%20cloud"));
}

PB_TEST(a_bare_host_is_completed_with_https) {
    PB_CHECK_EQ(canon("cloud.example.org"), std::string("https://cloud.example.org"));
    PB_CHECK_EQ(canon("cloud.example.org:8443"), std::string("https://cloud.example.org:8443"));
    PB_CHECK_EQ(canon("cloud.example.org/nextcloud/"), std::string("https://cloud.example.org/nextcloud"));
}

PB_TEST(every_other_scheme_is_refused_and_http_is_never_upgraded_silently) {
    for (const char* bad : {"http://cloud.example.org", "HTTP://cloud.example.org", "ftp://x.example.org", "file:///etc/passwd",
                            "ws://x.example.org", "javascript://x", "ssh://x.example.org", "imap://x.example.org", "httpss://x.example.org"})
        PB_CHECK_EQ(canon(bad), std::string("ERROR:scheme_not_allowed"));
}

PB_TEST(anything_that_could_redirect_the_credentials_is_refused) {
    for (const char* bad : {
             "https://user:pw@cloud.example.org",     // user information
             "https://user@cloud.example.org",
             "cloud.example.org@evil.example.org",
             "https://cloud.example.org?x=1",         // a query
             "https://cloud.example.org/#frag",       // a fragment
             "https://cloud.example.org/a?b",
             "https://",                              // no host
             "https:///path",
             "",
             "   ",
             "//cloud.example.org/x",                 // scheme-relative
             "/path/only",
             "cloud example.org",                     // blank inside
             "https://cloud.example.org/a b",
             "https://cloud.example.org/\x01",
             "https://cl\xC3\xB6ud.example.org",      // non-ASCII host: no silent IDN conversion
             "https://host:0",
             "https://host:65536",
             "https://host:-1",
             "https://host:abc",
             "https://host:",
             "https://[::1",
             "https://exa mple.org",
             "https://host_with%percent.example.org",
             "javascript:alert(1)",
         })
        PB_CHECK_EQ(canon(bad).substr(0, 6), std::string("ERROR:"));
    PB_CHECK_EQ(canon("https://user:pw@cloud.example.org"), std::string("ERROR:address_invalid"));
    PB_CHECK_EQ(canon("https://host:0"), std::string("ERROR:address_invalid"));
}

PB_TEST(the_host_is_at_most_253_bytes) {
    PB_CHECK_EQ(canon("https://" + std::string(253, 'a')).substr(0, 6), std::string("https:"));
    PB_CHECK_EQ(canon("https://" + std::string(254, 'a')), std::string("ERROR:address_invalid"));
}

PB_TEST_MAIN()
