// status.php (contracts/server-requests.md section 1): is this a Nextcloud, is it usable, and what may be shown about it.
#include <string>

#include "fake_transport.h"
#include "pbc/nc_server.h"
#include "test_support.h"

using pbtest::FakeTransport;

namespace {

pbc::Error classify(const pbc::Response& r, pbc::ServerInfo* info = nullptr) {
    pbc::ServerInfo local;
    return pbc::classify_status_document(r, info ? info : &local);
}
pbc::Response doc(const std::string& json, long status = 200) { return FakeTransport::status(status, json); }

const char* kGood = R"({"installed":true,"maintenance":false,"needsDbUpgrade":false,"version":"31.0.4.1","versionstring":"31.0.4","edition":"","productname":"Nextcloud"})";

}  // namespace

PB_TEST(a_normal_status_document_is_accepted_and_read) {
    pbc::ServerInfo info;
    PB_CHECK(classify(doc(kGood), &info).ok());
    PB_CHECK_EQ(info.version, std::string("31.0.4.1"));
    PB_CHECK_EQ(info.major, 31);
    PB_CHECK_EQ(info.name, std::string("Nextcloud"));
}

PB_TEST(the_product_name_is_optional) {
    pbc::ServerInfo info;
    PB_CHECK(classify(doc(R"({"installed":true,"maintenance":false,"version":"30.0.0.1"})"), &info).ok());
    PB_CHECK_EQ(info.name, std::string("Nextcloud"));
    PB_CHECK_EQ(info.major, 30);
}

PB_TEST(a_document_that_is_not_a_nextcloud_status_is_refused) {
    for (const char* bad : {"<html><body>Welcome to nginx</body></html>", "", "[]", "{}", "42", "null", R"({"installed":"yes"})",
                            R"({"maintenance":false,"version":"31.0.0.0"})", R"({"installed":false,"maintenance":false,"version":"31.0.0.0"})",
                            R"({"installed":true,"maintenance":false})", R"({"installed":true,"maintenance":false,"version":31})"})
        PB_CHECK_EQ(classify(doc(bad)).key, std::string("not_nextcloud"));
    PB_CHECK_EQ(classify(doc(kGood, 404)).key, std::string("not_nextcloud"));   // status.php of a Nextcloud is always public
    PB_CHECK_EQ(classify(doc(kGood, 301)).key, std::string("not_nextcloud"));
    PB_CHECK_EQ(classify(doc(kGood, 502)).key, std::string("server_error"));
}

PB_TEST(maintenance_and_a_pending_upgrade_are_reported_as_maintenance) {
    PB_CHECK_EQ(classify(doc(R"({"installed":true,"maintenance":true,"needsDbUpgrade":false,"version":"31.0.4.1"})")).key, std::string("maintenance"));
    PB_CHECK_EQ(classify(doc(R"({"installed":true,"maintenance":false,"needsDbUpgrade":true,"version":"31.0.4.1"})")).key, std::string("maintenance"));
}

PB_TEST(a_version_below_30_is_too_old_and_still_names_the_version) {
    pbc::ServerInfo info;
    PB_CHECK_EQ(classify(doc(R"({"installed":true,"maintenance":false,"version":"27.1.11.1"})"), &info).key, std::string("server_too_old"));
    PB_CHECK_EQ(info.version, std::string("27.1.11.1"));
    PB_CHECK_EQ(info.major, 27);
    PB_CHECK(classify(doc(R"({"installed":true,"maintenance":false,"version":"30.0.0.0"})")).ok());
    PB_CHECK_EQ(classify(doc(R"({"installed":true,"maintenance":false,"version":"29.9.9.9"})")).key, std::string("server_too_old"));
    PB_CHECK_EQ(classify(doc(R"({"installed":true,"maintenance":false,"version":"abc"})")).key, std::string("not_nextcloud"));
}

PB_TEST(shown_texts_are_sanitized_and_bounded) {
    pbc::ServerInfo info;
    // only the leading run of digits and dots survives, whatever follows it
    PB_CHECK(classify(doc(R"({"installed":true,"maintenance":false,"version":"31.0.4.1-evil<script>alert(1)</script>"})"), &info).ok());
    PB_CHECK_EQ(info.version, std::string("31.0.4.1"));
    PB_CHECK(classify(doc(R"({"installed":true,"maintenance":false,"version":"31.0.4.1.2.3.4.5.6.7.8.9.1.2.3.4.5.6.7.8.9.1.2.3.4.5"})"), &info).ok());
    PB_CHECK(info.version.size() <= 32);

    const std::string name = "Nextcloud\\u0007\\u001b[31m" + std::string(200, 'x');
    PB_CHECK(classify(doc(R"({"installed":true,"maintenance":false,"version":"31.0.4.1","productname":")" + name + R"("})"), &info).ok());
    PB_CHECK(info.name.size() <= 64);
    for (unsigned char c : info.name) PB_CHECK(c >= 0x20 && c != 0x7f);

    pbc::ServerInfo emptied;
    PB_CHECK(classify(doc(R"({"installed":true,"maintenance":false,"version":"31.0.4.1","productname":"\u0007"})"), &emptied).ok());
    PB_CHECK_EQ(emptied.name, std::string("Nextcloud"));   // nothing left of the name: the default
}

PB_TEST(a_body_over_64_kib_is_not_a_status_document) {
    const std::string big = std::string(R"({"installed":true,"maintenance":false,"version":"31.0.4.1","pad":")") + std::string(70 * 1024, 'x') + "\"}";
    PB_CHECK_EQ(classify(doc(big)).key, std::string("not_nextcloud"));
}

PB_TEST(a_transport_failure_keeps_its_own_key) {
    pbc::Response down;
    down.error = pbc::ErrorClass::Tls;
    down.error_key = "cert_untrusted";
    PB_CHECK_EQ(classify(down).key, std::string("cert_untrusted"));
    pbc::Response none;
    PB_CHECK_EQ(classify(none).key, std::string("server_unreachable"));
}

PB_TEST_MAIN()
