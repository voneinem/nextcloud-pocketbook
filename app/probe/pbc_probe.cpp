// pbc-probe: prints the facts about the device runtime that design decisions wait for (research R-02,
// open items O-1, O-6, O-7, O-8). Plain "key: value" lines, no personal data: no host name, user name, home
// directory, account or network address. Every answer is labelled with what was observed, not guessed.
//
//   pbc-probe [data-dir]       data-dir defaults to /mnt/ext1/system/pbcloud
//   PBC_PROBE_URL=https://...  an https address of any plain file server (a file of at least 1 MiB): the probe then tests Range, If-Range,
//                              the progress callback and the redirect rule of the bundled libcurl; the address is never printed
//   PBC_PROBE_PUT_URL=https://... [PBC_PROBE_PUT_LOGIN=user:app-password]  an https address that accepts a PUT (a file of a TEST account:
//                              the probe OVERWRITES it): the probe then sends a range of a file with the app's own transport and reports
//                              the bytes the server received, the cancel by the progress callback and `delivery` (feature 002, DT-W1/DT-W2)
#include <curl/curl.h>
#include "pbc/curl_transport.h"
#include <dirent.h>
#include <iconv.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

void section(const char* title) { std::printf("\n== %s\n", title); }

bool read_file(const std::string& path, std::string& out, size_t max = 1 << 20) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    if (out.size() > max) out.resize(max);
    return true;
}

bool has_protocol(const curl_version_info_data* info, const char* name) {
    if (!info->protocols) return false;
    for (const char* const* p = info->protocols; *p; ++p)
        if (std::strcmp(*p, name) == 0) return true;
    return false;
}

void probe_libcurl() {
    section("libcurl");
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    std::printf("version: %s\n", info->version ? info->version : "unknown");
    std::printf("ssl: %s\n", info->ssl_version ? info->ssl_version : "none");
    std::string protocols;
    for (const char* const* p = info->protocols; p && *p; ++p) protocols += std::string(protocols.empty() ? "" : " ") + *p;
    std::printf("protocols: %s\n", protocols.c_str());
    for (const char* name : {"imaps", "smtps", "imap", "smtp", "https"})
        std::printf("%s: %s\n", name, has_protocol(info, name) ? "yes" : "no");
    struct Feature { int bit; const char* name; };
    const Feature features[] = {{CURL_VERSION_SSL, "SSL"}, {CURL_VERSION_LIBZ, "libz"}, {CURL_VERSION_IPV6, "IPv6"},
                                {CURL_VERSION_NTLM, "NTLM"}, {CURL_VERSION_HTTP2, "HTTP2"}, {CURL_VERSION_THREADSAFE, "threadsafe"}};
    std::string feats;
    for (const Feature& f : features)
        if (info->features & f.bit) feats += std::string(feats.empty() ? "" : " ") + f.name;
    std::printf("features: %s\n", feats.c_str());
}

// Charsets commonly met on the web and in files; iconv_open succeeds only if the C library has the conversion module.
void probe_charsets() {
    section("charsets (iconv to UTF-8)");
    for (const char* cs : {"ISO-8859-1", "ISO-8859-2", "ISO-8859-15", "WINDOWS-1250", "WINDOWS-1251", "WINDOWS-1252",
                           "KOI8-R", "ISO-2022-JP", "SHIFT_JIS", "EUC-JP", "GB2312", "GBK", "BIG5", "EUC-KR", "UTF-7", "UTF-16LE"}) {
        iconv_t cd = iconv_open("UTF-8", cs);
        const bool ok = cd != reinterpret_cast<iconv_t>(-1);
        std::printf("%s: %s\n", cs, ok ? "yes" : "no");
        if (ok) iconv_close(cd);
    }
}

size_t count_certificates(const std::string& text) {
    size_t n = 0;
    for (size_t pos = text.find("BEGIN CERTIFICATE"); pos != std::string::npos; pos = text.find("BEGIN CERTIFICATE", pos + 1)) ++n;
    return n;
}

void probe_ca_files() {
    section("CA files");
    for (const char* b : {"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem", "/etc/ssl/certs/ca-bundle.crt"}) {
        std::string text;
        if (!read_file(b, text, 8 << 20)) {
            std::printf("%s: not present\n", b);
            continue;
        }
        std::printf("%s: %zu bytes, %zu certificates\n", b, text.size(), count_certificates(text));
    }
    size_t files = 0, usable = 0;
    if (DIR* d = opendir("/etc/ssl/certs")) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() < 5) continue;
            const std::string ext = name.substr(name.size() - 4);
            if (ext != ".pem" && ext != ".crt") continue;
            ++files;
            std::string text;
            if (read_file("/etc/ssl/certs/" + name, text, 1 << 16) && count_certificates(text) > 0) ++usable;
        }
        closedir(d);
    }
    std::printf("/etc/ssl/certs: %zu pem/crt files, %zu with a certificate\n", files, usable);
}

const char* fs_name(unsigned long type) {
    switch (type) {
        case 0x4d44: return "vfat";
        case 0x2011BAB0: return "exfat";
        case 0xEF53: return "ext2/3/4";
        case 0x65735546: return "fuse";
        case 0x01021994: return "tmpfs";
        case 0x58465342: return "xfs";
        case 0x9123683E: return "btrfs";
        case 0x5346544e: return "ntfs";
        case 0xF2F52010: return "f2fs";
        default: return "other";
    }
}

void probe_data_dir(const std::string& dir) {
    section("data directory");
    std::printf("path: %s\n", dir.c_str());
    struct stat st {};
    if (stat(dir.c_str(), &st) != 0 && mkdir(dir.c_str(), 0700) != 0) {
        std::printf("filesystem: unknown (directory missing and cannot be created)\n");
        std::printf("chmod 0600: not tested\nfree: unknown\n");
        return;
    }
    struct statfs sf {};
    if (statfs(dir.c_str(), &sf) == 0)
        std::printf("filesystem: %s (0x%lx)\n", fs_name(static_cast<unsigned long>(sf.f_type)), static_cast<unsigned long>(sf.f_type));
    else
        std::printf("filesystem: unknown\n");
    // chmod 0600 is "observable" when the mode read back is the mode set (FAT does not keep it)
    const std::string file = dir + "/.pbc-probe." + std::to_string(static_cast<long>(getpid()));
    bool written = false;
    if (FILE* f = std::fopen(file.c_str(), "w")) {
        std::fputs("x", f);
        std::fclose(f);
        written = true;
    }
    if (!written) {
        std::printf("chmod 0600: not tested (directory not writable)\n");
    } else {
        struct stat fst {};
        if (chmod(file.c_str(), 0600) == 0 && stat(file.c_str(), &fst) == 0)
            std::printf("chmod 0600: %s (mode read back: %04o)\n", (fst.st_mode & 0777) == 0600 ? "observable" : "NOT observable",
                        static_cast<unsigned>(fst.st_mode & 0777));
        else
            std::printf("chmod 0600: failed\n");
        std::remove(file.c_str());
    }
    struct statvfs sv {};
    if (statvfs(dir.c_str(), &sv) == 0)
        std::printf("free: %llu MiB\n", static_cast<unsigned long long>(sv.f_bavail) * sv.f_frsize / (1024 * 1024));
    else
        std::printf("free: unknown\n");
}

void probe_system() {
    section("system");
    const char* tz = std::getenv("TZ");
    std::printf("TZ: %s\n", tz && *tz ? tz : "(unset)");
    char target[512];
    const ssize_t n = readlink("/etc/localtime", target, sizeof target - 1);
    if (n > 0) {
        target[n] = 0;
        std::printf("localtime: -> %s\n", target);
    } else {
        struct stat st {};
        std::printf("localtime: %s\n", stat("/etc/localtime", &st) == 0 ? "regular file" : "missing");
    }
    std::string meminfo;
    long kb = -1;
    if (read_file("/proc/meminfo", meminfo)) {
        const size_t p = meminfo.find("MemTotal:");
        if (p != std::string::npos) kb = std::atol(meminfo.c_str() + p + 9);
    }
    if (kb >= 0) std::printf("MemTotal: %ld kB\n", kb);
    else std::printf("MemTotal: unknown\n");
    struct stat st {};
    std::printf("gconv: %s\n", stat("/usr/lib/gconv", &st) == 0 ? "/usr/lib/gconv present" : "/usr/lib/gconv absent");
    // the frame buffer size is the nearest answer to the screen size that needs no display library
    std::string fb;
    if (read_file("/sys/class/graphics/fb0/virtual_size", fb)) {
        while (!fb.empty() && (fb.back() == '\n' || fb.back() == '\r')) fb.pop_back();
        std::printf("screen: frame buffer %s (width,height in pixels)\n", fb.c_str());
    } else {
        std::printf("screen: unknown (no frame buffer information)\n");
    }
}

// ---- downloads (specs/001-nextcloud-files-client research R-8, R-13; device checks DT-NC-1, DT-NC-3, DT-NC-9) ----

// Can a file of 4 GiB or more exist here? On FAT it cannot; the app refuses such files before writing a byte.
void probe_big_file(const std::string& dir) {
    section("downloads: file size limit");
    const std::string file = dir + "/.pbc-probe-big." + std::to_string(static_cast<long>(getpid()));
    FILE* f = std::fopen(file.c_str(), "wb");
    if (!f) {
        std::printf("4 GiB file: not tested (directory not writable)\n");
        return;
    }
    const long long five_gib = 5ll << 30;
    const int rc = ftruncate(fileno(f), static_cast<off_t>(five_gib));
    struct stat st {};
    const bool ok = rc == 0 && fstat(fileno(f), &st) == 0 && static_cast<long long>(st.st_size) == five_gib;
    std::printf("5 GiB sparse file: %s\n", ok ? "can be created (this file system has no 4 GiB limit)" : "cannot be created (4 GiB limit, as on FAT)");
    std::fclose(f);
    std::remove(file.c_str());
}

struct RangeCapture {
    long status = 0;
    std::string content_range;
    long long bytes = 0;
};

size_t range_header(char* data, size_t size, size_t n, void* user) {
    auto* c = static_cast<RangeCapture*>(user);
    const std::string line(data, size * n);
    if (line.compare(0, 5, "HTTP/") == 0) {
        const size_t sp = line.find(' ');
        c->status = sp == std::string::npos ? 0 : std::atol(line.c_str() + sp + 1);
        c->content_range.clear();
    } else if (line.size() > 14 && (line[0] == 'C' || line[0] == 'c') && line.compare(0, 14, "Content-Range:") == 0) {
        c->content_range = line.substr(14);
        while (!c->content_range.empty() && (c->content_range.back() == '\r' || c->content_range.back() == '\n')) c->content_range.pop_back();
    }
    return size * n;
}
size_t discard_body(char*, size_t size, size_t n, void* user) {
    static_cast<RangeCapture*>(user)->bytes += static_cast<long long>(size * n);
    return size * n;
}
int cancel_after_64k(void*, curl_off_t, curl_off_t now, curl_off_t, curl_off_t) { return now >= 64 * 1024 ? 1 : 0; }

std::string first_ca_file() {
    for (const char* b : {"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem", "/etc/ssl/certs/ca-bundle.crt"}) {
        std::string text;
        if (read_file(b, text, 1 << 16)) return b;
    }
    return std::string();
}

CURL* probe_handle(const char* url, const std::string& ca, RangeCapture* cap) {
    CURL* c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!ca.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, ca.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, range_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, cap);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, cap);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "identity");
    return c;
}

// Needs PBC_PROBE_URL. Prints one result line per test and never the address.
void probe_range_url() {
    section("downloads: ranged requests (bundled libcurl)");
    const char* url = std::getenv("PBC_PROBE_URL");
    if (!url || std::strncmp(url, "https://", 8) != 0) {
        std::printf("range: skipped (PBC_PROBE_URL is not set to an https address)\n");
        std::printf("if-range: skipped\ncancel: skipped\nredirect: skipped\n");
        return;
    }
    const std::string ca = first_ca_file();
    {
        RangeCapture cap;
        CURL* c = probe_handle(url, ca, &cap);
        curl_easy_setopt(c, CURLOPT_RANGE, "1024-");
        const CURLcode rc = curl_easy_perform(c);
        std::printf("range: %s (status %ld, content-range \"%s\")\n", rc == CURLE_OK && cap.status == 206 ? "206 honoured" : (rc == CURLE_OK && cap.status == 200 ? "NOT honoured (200)" : "failed"),
                    cap.status, cap.content_range.c_str());
        curl_easy_cleanup(c);
    }
    {
        RangeCapture cap;
        CURL* c = probe_handle(url, ca, &cap);
        curl_easy_setopt(c, CURLOPT_RANGE, "1024-");
        struct curl_slist* h = curl_slist_append(nullptr, "If-Range: \"pbc-probe-no-such-etag\"");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        const CURLcode rc = curl_easy_perform(c);
        std::printf("if-range: %s (status %ld)\n", rc == CURLE_OK && cap.status == 200 ? "a wrong validator gives the whole file (200), as required"
                                                   : (rc == CURLE_OK && cap.status == 206 ? "a wrong validator was ignored (206): resume would not be safe" : "failed"), cap.status);
        curl_slist_free_all(h);
        curl_easy_cleanup(c);
    }
    {
        RangeCapture cap;
        CURL* c = probe_handle(url, ca, &cap);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, cancel_after_64k);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &cap);
        curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 65536L);
        const CURLcode rc = curl_easy_perform(c);
        std::printf("cancel: %s (received %lld bytes)\n", rc == CURLE_ABORTED_BY_CALLBACK ? "the progress callback stops the transfer" : "did not stop", cap.bytes);
        curl_easy_cleanup(c);
    }
    {
        RangeCapture cap;
        CURL* c = probe_handle(url, ca, &cap);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
        const CURLcode rc = curl_easy_perform(c);
        std::printf("redirect: %s\n", rc != CURLE_OK ? "not tested (request failed)" : (cap.status >= 300 && cap.status < 400 ? "the address redirects; the app checks every redirect target" : "none"));
        curl_easy_cleanup(c);
    }
}

// Needs PBC_PROBE_PUT_URL (and, for a server that wants a login, PBC_PROBE_PUT_LOGIN). Prints one result line per test and never the address, the
// login or the file name. The body is a range of a temporary file of the probe, so the range logic of the product (Request::body_file) is what runs.
void probe_put_range(const std::string& dir) {
    section("uploads: a PUT from a file range (the app's own transport)");
    const char* url = std::getenv("PBC_PROBE_PUT_URL");
    if (!url || std::strncmp(url, "https://", 8) != 0) {
        std::printf("put-range: skipped (PBC_PROBE_PUT_URL is not set to an https address)\nput-cancel: skipped\nput-delivery: skipped\n");
        return;
    }
    const std::string path = dir + "/pbc-probe-put.bin";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        std::string block(1 << 20, 'p');
        for (int i = 0; i < 3; ++i) f.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    pbc::CurlTransport transport(first_ca_file());
    const char* login = std::getenv("PBC_PROBE_PUT_LOGIN");
    std::string user, pass;
    if (login) {
        const std::string l = login;
        const size_t colon = l.find(':');
        user = l.substr(0, colon);
        pass = colon == std::string::npos ? std::string() : l.substr(colon + 1);
    }
    {   // the second MiB, whole: the server must see exactly 1 MiB
        pbc::Request r;
        r.method = "PUT";
        r.url = url;
        r.username = user;
        r.password = pass;
        r.body_file = path;
        r.body_offset = 1 << 20;
        r.body_length = 1 << 20;
        r.buffer_size = 65536;
        long long last = 0;
        r.progress = [&](long long done, long long) { last = done; return true; };
        const pbc::Response a = transport.perform(r);
        std::printf("put-range: %s (status %ld, sent %lld bytes of %lld, delivery %s)\n", a.status >= 200 && a.status < 300 ? "accepted" : "not accepted", a.status, last,
                    r.body_length, a.status != 0 ? "answered" : (a.delivery == pbc::Delivery::NotSent ? "not_sent" : (a.delivery == pbc::Delivery::PartlySent ? "partly_sent" : "fully_sent")));
    }
    {   // cancelled by the progress callback after 256 KiB: nothing may be stored by a well-behaved server
        pbc::Request r;
        r.method = "PUT";
        r.url = url;
        r.username = user;
        r.password = pass;
        r.body_file = path;
        r.body_offset = 0;
        r.body_length = 3 << 20;
        r.buffer_size = 65536;
        long long last = 0;
        r.progress = [&](long long done, long long) { last = done; return done < 256 * 1024; };
        const pbc::Response a = transport.perform(r);
        std::printf("put-cancel: %s (sent %lld bytes, delivery %s)\n", a.cancelled ? "the progress callback stops the transfer" : "did not stop", last,
                    a.delivery == pbc::Delivery::NotSent ? "not_sent" : (a.delivery == pbc::Delivery::PartlySent ? "partly_sent" : "fully_sent"));
    }
    std::printf("put-delivery: cut the Wi-Fi during a longer PUT by hand and read the delivery of the app's log line (DT-W2); this probe cannot cut it\n");
    ::unlink(path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string data_dir = argc > 1 ? argv[1] : "/mnt/ext1/system/pbcloud";
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::printf("pbc-probe (prints no personal data)\n");
    probe_libcurl();
    probe_charsets();
    probe_ca_files();
    probe_data_dir(data_dir);
    probe_big_file(data_dir);
    probe_range_url();
    probe_put_range(data_dir);
    probe_system();
    curl_global_cleanup();
    return 0;
}
