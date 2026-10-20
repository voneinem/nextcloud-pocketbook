#include "pbc/nc_login.h"

#include "pbc/json_min.h"
#include "pbc/nc_server.h"
#include "pbc/remote_path.h"
#include "pbc/version.h"
#include "pbc/secure_wipe.h"

namespace pbc {

namespace {

constexpr size_t kMaxToken = 512;

bool printable_token(const std::string& t) {
    if (t.empty() || t.size() > kMaxToken) return false;
    for (unsigned char c : t)
        if (c < 0x21 || c > 0x7e) return false;
    return true;
}

std::string form_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

Error transport_error(const Response& r) {
    const ErrorClass cls = r.error == ErrorClass::None ? ErrorClass::Network : r.error;
    return make_error(cls, r.error_key.empty() ? "server_unreachable" : r.error_key, r.error_detail);
}

Error other_origin() { return make_error(ErrorClass::Tls, "redirect_other_host", "an address of another server was refused"); }
Error unusable() { return make_error(ErrorClass::Protocol, "protocol_error", "the answer of the server is not usable"); }

}  // namespace

void login_discard(LoginStart& flow) {
    secure_wipe(flow.token);
    flow.token.clear();
    flow.poll_endpoint.clear();
    flow.login_url.clear();
}

Result<LoginStart> login_start(HttpTransport& http, const std::string& base_url) {
    Result<LoginStart> r;
    Request req;
    req.method = "POST";
    req.url = base_url + "/index.php/login/v2";
    req.headers = {{"User-Agent", std::string("PBCloud/") + version()}, {"Accept", "application/json"}};
    req.max_response_bytes = 64 * 1024;
    const Response resp = http.perform(req);
    if (resp.status == 0 || resp.error != ErrorClass::None) {
        r.error = transport_error(resp);
        return r;
    }
    if (resp.status != 200) {
        r.error = make_error(ErrorClass::Server, resp.status == 404 ? "not_nextcloud" : "login_failed", "the server did not start a login");
        return r;
    }
    JsonLimits limits;
    limits.max_bytes = 64 * 1024;
    auto doc = parse_json(resp.body, limits);
    const JsonValue* poll = doc.ok() ? doc.value.get("poll") : nullptr;
    const JsonValue* token = poll ? poll->get("token") : nullptr;
    const JsonValue* endpoint = poll ? poll->get("endpoint") : nullptr;
    const JsonValue* login = doc.ok() ? doc.value.get("login") : nullptr;
    auto is_str = [](const JsonValue* v) { return v && v->type == JsonValue::Type::String; };
    if (!is_str(token) || !is_str(endpoint) || !is_str(login) || !printable_token(token->text)) {
        r.error = unusable();
        return r;
    }
    // both addresses must lie on the server's own https origin, so a hostile answer cannot send the token elsewhere
    if (!same_https_origin(endpoint->text, base_url) || !same_https_origin(login->text, base_url)) {
        r.error = other_origin();
        return r;
    }
    r.value.token = token->text;
    r.value.poll_endpoint = endpoint->text;
    r.value.login_url = login->text;
    return r;
}

Result<std::optional<LoginResult>> login_check(HttpTransport& http, const std::string& base_url, LoginStart& flow) {
    Result<std::optional<LoginResult>> r;
    if (flow.token.empty() || flow.poll_endpoint.empty()) {
        r.error = make_error(ErrorClass::Auth, "login_expired", "there is no login to check");
        return r;
    }
    if (!same_https_origin(flow.poll_endpoint, base_url)) {
        login_discard(flow);
        r.error = other_origin();
        return r;
    }
    Request req;
    req.method = "POST";
    req.url = flow.poll_endpoint;
    req.headers = {{"User-Agent", std::string("PBCloud/") + version()}, {"Accept", "application/json"},
                   {"Content-Type", "application/x-www-form-urlencoded"}};
    req.body = "token=" + form_encode(flow.token);
    req.max_response_bytes = 64 * 1024;
    Response resp = http.perform(req);
    secure_wipe(req.body);
    if (resp.status == 0 || resp.error != ErrorClass::None) {   // no answer: the flow stays open for another tap
        r.error = transport_error(resp);
        return r;
    }
    if (resp.status == 404) return r;   // not approved yet
    if (resp.status != 200) {
        login_discard(flow);
        r.error = make_error(ErrorClass::Auth, "login_failed", "the login did not complete");
        return r;
    }
    JsonLimits limits;
    limits.max_bytes = 64 * 1024;
    auto doc = parse_json(resp.body, limits);
    secure_wipe(resp.body);
    login_discard(flow);   // the answer comes only once: from here on the flow is over, whatever it held
    const JsonValue* server = doc.ok() ? doc.value.get("server") : nullptr;
    const JsonValue* name = doc.ok() ? doc.value.get("loginName") : nullptr;
    const JsonValue* pw = doc.ok() ? doc.value.get("appPassword") : nullptr;
    auto is_str = [](const JsonValue* v) { return v && v->type == JsonValue::Type::String; };
    if (!is_str(server) || !is_str(name) || !is_str(pw) || name->text.empty() || name->text.size() > 256 || pw->text.empty() || pw->text.size() > 512) {
        r.error = unusable();
        return r;
    }
    const auto canonical = canonical_base_url(server->text);
    if (!canonical.ok() || !same_https_origin(canonical.value, base_url)) {
        r.error = other_origin();
        return r;
    }
    LoginResult result;
    result.server = canonical.value;
    result.login_name = name->text;
    result.app_password = pw->text;
    r.value = std::move(result);
    return r;
}

}  // namespace pbc
