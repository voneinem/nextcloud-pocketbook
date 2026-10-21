#pragma once
// Abstract HTTP seam (T015). The production implementation (CurlTransport) verifies TLS and
// refuses non-https URLs; tests use a scripted FakeTransport. No Qt.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "pbc/types.h"

namespace pbc {

using Headers = std::vector<std::pair<std::string, std::string>>;

// Case-insensitive lookup; returns "" when absent.
std::string header_value(const Headers& headers, const std::string& name);
bool has_header(const Headers& headers, const std::string& name);
// Every value of a header that was sent more than once, in order (names compared without regard to case).
std::vector<std::string> header_values(const Headers& headers, const std::string& name);

struct Request {
    std::string method = "GET";
    std::string url;
    Headers headers;
    std::string body;
    // HTTP Basic credentials; the transport builds the Authorization header. Empty = none.
    std::string username;
    std::string password;
    // Methods: GET, HEAD, POST and the WebDAV verbs PROPFIND, REPORT, SEARCH, OPTIONS (also with a body), and the write verbs PUT, MKCOL,
    // MOVE, COPY, DELETE (the transport carries any verb; which module may name a write verb is rule N2 of scripts/check-constitution.sh).
    // A redirect keeps method and body; credentials are not sent to another host or port.
    // max_response_bytes: cap for an in-memory body (8 MiB; 0 = none). When the declared or the running size exceeds it, the transfer is
    //   stopped and Response::body_too_large is set (the body is empty).
    long long max_response_bytes = 8ll << 20;
    long connect_timeout_s = 10;
    long total_timeout_s = 60;
    // File transfer (specs/001, research R-8).
    // save_body_to: a successful (2xx) body is streamed into this file instead of `Response::body`; an error
    //   status keeps its (small) body in memory and creates no file. Nothing is left behind on failure.
    // max_body_bytes: 0 = no limit; otherwise the transfer is stopped as soon as Content-Length or the running
    //   total exceeds it (Response::body_too_large) and the file is removed.
    std::string save_body_to;
    long long max_body_bytes = 0;
    // A request body that is a range of a file (a PUT of a file or of one chunk, specs/002-manage-server-files): the bytes
    // [body_offset, body_offset + body_length) of body_file, streamed with a 64 KiB read buffer; Content-Length is body_length. The file
    // is opened read-only and links are not followed. Exclusive with `body`. A file that is not a regular readable file, or is shorter
    // than offset + length, is refused before anything is sent (Response::error_key "source_unreadable", delivery NotSent).
    std::string body_file;
    long long body_offset = 0;
    long long body_length = 0;
    // Downloads that continue a partial file (specs/001-nextcloud-files-client, research R-8):
    // save_append: with save_body_to, a 206 answer is appended to the existing file; any other 2xx answer (the server ignored the
    //   Range) rewrites the file from the start. Nothing is ever removed by the transport in this mode (an error status, a dropped
    //   connection, a cancel): the caller owns the partial file and sees what the server said in Response::status/headers.
    // resume_from: the bytes already in the file; they count towards max_body_bytes and in the progress numbers.
    bool save_append = false;
    long long resume_from = 0;
    // Called from the transfer thread (at least once per second while connected); false cancels the transfer. `done` includes
    // resume_from for a 206 answer; `total` is 0 while unknown.
    std::function<bool(long long done, long long total)> progress;
    long buffer_size = 0;            // 0 = libcurl's default; file transfers use 65536
    bool identity_encoding = false;  // send Accept-Encoding: identity, so Content-Length and Range mean the stored bytes
};

// How far a request got when no answer came back: the difference between "nothing happened" and "the server may have acted on it".
enum class Delivery {
    NotSent,      // nothing was handed to the connection (no connection, handshake failed, refused before sending)
    PartlySent,   // the headers or a part of the body went out
    FullySent,    // the whole request went out; the server may have processed it
};

struct Response {
    long status = 0;  // 0 = no HTTP response (see error)
    // Meaningful when status == 0 (a request that got an answer arrived). The fakes of the tests leave NotSent for an injected connection error.
    Delivery delivery = Delivery::NotSent;
    Headers headers;
    std::string body;
    ErrorClass error = ErrorClass::None;  // transport-level classification when status == 0
    std::string error_key;                // stable cause key of the mapper (curl_session.h), "" when the status is set
    std::string error_detail;             // short, never contains credentials, addresses or message text
    bool body_too_large = false;          // the transfer was stopped at Request::max_body_bytes
    bool storage_full = false;            // writing Request::save_body_to failed because the device is full
    bool cancelled = false;               // the progress callback returned false
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual Response perform(const Request& request) = 0;
};

}  // namespace pbc
