#include "pbc/upload_source.h"

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <memory>

namespace pbc {

namespace {

// Read-only, no link: the file is looked at through the open descriptor, so what was checked is what is read.
int open_source(const std::string& path, Error* error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) return fd;
    if (errno == ELOOP) *error = make_error(ErrorClass::Storage, "source_not_allowed", "a symbolic link is not followed");
    else *error = make_error(ErrorClass::Storage, "source_unreadable", "the file cannot be read");
    return -1;
}

}  // namespace

Result<SourceIdentity> inspect_source(const std::string& path, const SourceRules& rules) {
    Result<SourceIdentity> r;
    const Result<std::string> allowed = check_source_path(path, rules);
    if (!allowed.ok()) {
        r.error = allowed.error;
        return r;
    }
    const int fd = open_source(path, &r.error);
    if (fd < 0) return r;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        r.error = make_error(ErrorClass::Storage, "source_unreadable", "not a regular file");
        return r;
    }
    ::close(fd);
    if (st.st_size <= 0) {
        r.error = make_error(ErrorClass::Storage, "source_unreadable", "the file is empty");
        return r;
    }
    if (static_cast<long long>(st.st_size) > 4294967295ll) {
        r.error = make_error(ErrorClass::Storage, "too_large_for_storage", "the file is larger than the storage can hold");
        return r;
    }
    r.value.size = static_cast<int64_t>(st.st_size);
    r.value.mtime = static_cast<int64_t>(st.st_mtime);
    return r;
}

bool same_identity(const SourceIdentity& a, const SourceIdentity& b) { return a.size == b.size && a.mtime == b.mtime; }

Result<std::string> hash_source(const std::string& path, Cancel& cancel, const std::function<void(long long, long long)>& progress) {
    Result<std::string> r;
    const int fd = open_source(path, &r.error);
    if (fd < 0) return r;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        r.error = make_error(ErrorClass::Storage, "source_unreadable", "not a regular file");
        return r;
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        ::close(fd);
        r.error = make_error(ErrorClass::Unsupported, "protocol_error", "the digest is not available");
        return r;
    }
    const long long total = static_cast<long long>(st.st_size);
    long long done = 0;
    unsigned char buf[64 * 1024];
    for (;;) {
        if (cancel.requested()) {
            ::close(fd);
            r.error = make_error(ErrorClass::Cancelled, "cancelled", "cancelled");
            return r;
        }
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            ::close(fd);
            r.error = make_error(ErrorClass::Storage, "source_unreadable", "the file cannot be read");
            return r;
        }
        if (n == 0) break;
        EVP_DigestUpdate(ctx.get(), buf, static_cast<size_t>(n));
        done += n;
        if (progress) progress(done, total);
    }
    ::close(fd);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx.get(), md, &len);
    static const char* kHex = "0123456789abcdef";
    r.value.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        r.value.push_back(kHex[md[i] >> 4]);
        r.value.push_back(kHex[md[i] & 15]);
    }
    return r;
}

}  // namespace pbc
