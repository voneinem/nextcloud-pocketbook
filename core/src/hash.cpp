#include "pbc/hash.h"

#include <openssl/evp.h>

#include <cstdio>
#include <memory>

namespace pbc {

namespace {

const EVP_MD* md_of(HashAlgo a) {
    switch (a) {
        case HashAlgo::Sha256: return EVP_sha256();
        case HashAlgo::Sha1: return EVP_sha1();
        default: return EVP_md5();
    }
}

size_t hex_len(HashAlgo a) { return a == HashAlgo::Sha256 ? 64 : (a == HashAlgo::Sha1 ? 40 : 32); }

}  // namespace

std::vector<Checksum> parse_checksums(std::string_view text) {
    std::vector<Checksum> out;
    size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == ',' || text[pos] == '\t' || text[pos] == '\n')) ++pos;
        size_t end = pos;
        while (end < text.size() && text[end] != ' ' && text[end] != ',' && text[end] != '\t' && text[end] != '\n') ++end;
        const std::string_view item = text.substr(pos, end - pos);
        pos = end;
        const size_t colon = item.find(':');
        if (colon == std::string_view::npos || colon == 0) continue;
        std::string name(item.substr(0, colon));
        for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        HashAlgo algo;
        if (name == "SHA256" || name == "SHA-256") algo = HashAlgo::Sha256;
        else if (name == "SHA1" || name == "SHA-1") algo = HashAlgo::Sha1;
        else if (name == "MD5") algo = HashAlgo::Md5;
        else continue;
        std::string hex(item.substr(colon + 1));
        if (hex.size() != hex_len(algo)) continue;
        bool ok = true;
        for (char& c : hex) {
            if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) ok = false;
        }
        if (!ok) continue;
        out.push_back({algo, hex});
    }
    return out;
}

std::optional<Checksum> strongest(const std::vector<Checksum>& list) {
    std::optional<Checksum> best;
    auto rank = [](HashAlgo a) { return a == HashAlgo::Sha256 ? 3 : (a == HashAlgo::Sha1 ? 2 : 1); };
    for (const Checksum& c : list)
        if (!best || rank(c.algo) > rank(best->algo)) best = c;
    return best;
}

Result<std::string> hash_file(const std::string& path, HashAlgo algo, Cancel& cancel) {
    Result<std::string> r;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        r.error = make_error(ErrorClass::Storage, "write_failed", "the file cannot be read");
        return r;
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), md_of(algo), nullptr) != 1) {
        std::fclose(f);
        r.error = make_error(ErrorClass::Unsupported, "protocol_error", "the digest is not available");
        return r;
    }
    unsigned char buf[64 * 1024];
    for (;;) {
        if (cancel.requested()) {
            std::fclose(f);
            r.error = make_error(ErrorClass::Cancelled, "cancelled", "cancelled");
            return r;
        }
        const size_t n = std::fread(buf, 1, sizeof buf, f);
        if (n > 0) EVP_DigestUpdate(ctx.get(), buf, n);
        if (n < sizeof buf) {
            if (std::ferror(f)) {
                std::fclose(f);
                r.error = make_error(ErrorClass::Storage, "write_failed", "the file cannot be read");
                return r;
            }
            break;
        }
    }
    std::fclose(f);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned len = 0;
    EVP_DigestFinal_ex(ctx.get(), md, &len);
    static const char* hex = "0123456789abcdef";
    for (unsigned i = 0; i < len; ++i) {
        r.value.push_back(hex[md[i] >> 4]);
        r.value.push_back(hex[md[i] & 15]);
    }
    return r;
}

}  // namespace pbc
