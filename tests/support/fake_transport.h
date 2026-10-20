#pragma once
// Scripted HttpTransport for contract and integration tests (T016). Records every request
// including credentials and headers. Accepts any URL scheme.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "pbc/http_transport.h"

namespace pbtest {

class FakeTransport : public pbc::HttpTransport {
public:
    struct Expectation {
        std::string method;
        std::string url_contains;  // substring of the full URL
        pbc::Response response;
    };

    // Queue of expectations, consumed in order. A request that does not match the next
    // expectation is recorded as a mismatch and answered with a Server error.
    FakeTransport& expect(const std::string& method, const std::string& url_contains, pbc::Response response) {
        queue_.push_back({method, url_contains, std::move(response)});
        return *this;
    }

    // Alternative to the queue: a handler decides every answer (fake server logic).
    void set_handler(std::function<pbc::Response(const pbc::Request&)> handler) { handler_ = std::move(handler); }

    // Failure injection: the n-th request (0-based) fails with a transport error instead.
    void fail_request(size_t index, pbc::ErrorClass error) { failures_.push_back({index, error}); }

    // Simulates a full device for the next file downloads (Response::storage_full, nothing written).
    bool simulate_storage_full = false;
    // Simulates a write error that is not "full" (the card pulled, EIO): the download fails with the key write_failed.
    bool simulate_io_error = false;
    // The next file download is cut off after this many body bytes, like a Wi-Fi drop: what arrived stays in the file, the answer is a
    // network error (with the status and headers when the request appends to a partial file, as the real transport does).
    void drop_next_download_after(int64_t bytes) { drop_after_ = bytes; }

    pbc::Response perform(const pbc::Request& request) override {
        pbc::Response r = perform_scripted(request);
        apply_file_semantics(request, &r);
        return r;
    }

    const std::vector<pbc::Request>& requests() const { return requests_; }
    void clear_requests() { requests_.clear(); }
    const std::vector<std::string>& mismatches() const { return mismatches_; }
    bool all_consumed() const { return next_ == queue_.size(); }

private:
    // What the real transport does with save_body_to / max_body_bytes: the body of a 2xx answer goes into the
    // file (Response::body stays empty); a Content-Length or a body over the cap stops the transfer.
    void apply_file_semantics(const pbc::Request& request, pbc::Response* r) {
        if (request.save_body_to.empty() || r->status < 200 || r->status >= 300 || r->error != pbc::ErrorClass::None) return;
        if (r->body_too_large || r->storage_full) return;
        if (request.method == "HEAD") return;
        const std::string length = pbc::header_value(r->headers, "Content-Length");
        // only a 206 continues a partial file (Request::save_append); every other 2xx answer rewrites it
        const bool append = request.save_append && r->status == 206;
        const long long base = append ? request.resume_from : 0;
        if (request.max_body_bytes > 0) {
            const long long declared = length.empty() ? -1 : std::atoll(length.c_str());
            if (base + declared > request.max_body_bytes || base + static_cast<long long>(r->body.size()) > request.max_body_bytes) {
                r->body_too_large = true;
                r->body.clear();
                return;
            }
        }
        if (simulate_storage_full) {
            r->storage_full = true;
            r->body.clear();
            return;
        }
        if (simulate_io_error) {
            r->status = 0;
            r->headers.clear();
            r->body.clear();
            r->error = pbc::ErrorClass::Storage;
            r->error_key = "write_failed";
            return;
        }
        if (drop_after_ >= 0) {
            const size_t n = static_cast<size_t>(std::min<int64_t>(drop_after_, static_cast<int64_t>(r->body.size())));
            drop_after_ = -1;
            {
                std::ofstream f(request.save_body_to, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
                f.write(r->body.data(), static_cast<std::streamsize>(n));
            }
            r->body.clear();
            r->error = pbc::ErrorClass::Network;
            r->error_key = "connection_lost";
            if (!request.save_append) {   // a plain download leaves nothing behind
                std::remove(request.save_body_to.c_str());
                r->status = 0;
                r->headers.clear();
            }
            return;
        }
        std::ofstream f(request.save_body_to, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        const long long total = base + static_cast<long long>(r->body.size());
        size_t written = 0;
        while (written < r->body.size()) {   // in 64 KiB steps, so the progress callback can cancel in the middle
            if (request.progress && !request.progress(base + static_cast<long long>(written), total)) {
                f.close();
                r->cancelled = true;
                r->error = pbc::ErrorClass::Cancelled;
                r->error_key = "cancelled";
                r->body.clear();
                if (!request.save_append) std::remove(request.save_body_to.c_str());
                return;
            }
            const size_t n = std::min<size_t>(64 * 1024, r->body.size() - written);
            f.write(r->body.data() + written, static_cast<std::streamsize>(n));
            written += n;
        }
        if (request.progress) request.progress(total, total);
        r->body.clear();
    }

    pbc::Response perform_scripted(const pbc::Request& request) {
        const size_t index = requests_.size();
        requests_.push_back(request);
        for (const auto& f : failures_) {
            if (f.first == index) {
                pbc::Response r;
                r.error = f.second;
                r.error_detail = "injected failure";
                return r;
            }
        }
        if (handler_) return handler_(request);
        if (next_ < queue_.size()) {
            const Expectation& e = queue_[next_];
            if (e.method == request.method && request.url.find(e.url_contains) != std::string::npos) {
                ++next_;
                return e.response;
            }
        }
        mismatches_.push_back(request.method + " " + request.url);
        pbc::Response r;
        r.status = 599;
        r.error = pbc::ErrorClass::Server;
        r.error_detail = "unexpected request";
        return r;
    }

public:
    static pbc::Response ok(const std::string& body, pbc::Headers headers = {}) {
        pbc::Response r;
        r.status = 200;
        r.body = body;
        r.headers = std::move(headers);
        return r;
    }
    static pbc::Response status(long code, const std::string& body = "", pbc::Headers headers = {}) {
        pbc::Response r;
        r.status = code;
        r.body = body;
        r.headers = std::move(headers);
        return r;
    }

private:
    std::vector<Expectation> queue_;
    size_t next_ = 0;
    std::function<pbc::Response(const pbc::Request&)> handler_;
    std::vector<std::pair<size_t, pbc::ErrorClass>> failures_;
    std::vector<pbc::Request> requests_;
    std::vector<std::string> mismatches_;
    int64_t drop_after_ = -1;
};

}  // namespace pbtest
