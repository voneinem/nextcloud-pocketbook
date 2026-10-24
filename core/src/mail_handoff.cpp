#include "pbc/mail_handoff.h"

#include <utility>

#include "pbc/secure_wipe.h"
#include "pbc/utf8.h"

namespace pbc {

MailHandoff make_handoff(std::vector<std::string>&& argv) {
    MailHandoff h;
    h.argv_ = std::move(argv);
    return h;
}

MailHandoff::MailHandoff(MailHandoff&& other) noexcept : argv_(std::move(other.argv_)) { other.argv_.clear(); }

MailHandoff& MailHandoff::operator=(MailHandoff&& other) noexcept {
    if (this != &other) {
        wipe();
        argv_ = std::move(other.argv_);
        other.argv_.clear();
    }
    return *this;
}

MailHandoff::~MailHandoff() { wipe(); }

void MailHandoff::wipe() {
    for (std::string& s : argv_) {
        secure_wipe(s);
        s.clear();
    }
}

namespace {

bool valid_text(const std::string& link) {
    for (size_t i = 0; i < link.size();) {
        const unsigned char c = static_cast<unsigned char>(link[i]);
        if (c < 0x20 || c == 0x7f) return false;
        const Utf8Char ch = utf8_decode(link, i);
        if (!ch.valid) return false;
        i += ch.len;
    }
    return true;
}

}  // namespace

MailBuild build_mail_handoff(std::string&& link, const std::string& subject_line) {
    std::string owned = std::move(link);
    MailBuild out;
    if (owned.empty()) {
        out.error = MailError::LinkEmpty;
    } else if (owned.size() > kMailTextLimit) {
        out.error = MailError::LinkTooLong;
    } else if (!valid_text(owned)) {
        out.error = MailError::LinkInvalid;
    }
    if (out.error != MailError::None) {
        secure_wipe(owned);
        return out;
    }
    std::vector<std::string> argv;
    argv.reserve(4);
    argv.emplace_back("pbo-mail-v1");
    argv.emplace_back("text=" + owned);
    argv.emplace_back("textlen=" + std::to_string(owned.size()));
    argv.emplace_back("subject=" + subject_line);
    secure_wipe(owned);
    out.handoff = make_handoff(std::move(argv));
    return out;
}

std::string clean_subject(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pending_blank = false;
    size_t chars = 0;
    for (size_t i = 0; i < text.size() && chars < kMailSubjectLimit;) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7f || c == ' ') {
            pending_blank = !out.empty();
            ++i;
            continue;
        }
        const Utf8Char ch = utf8_decode(text, i);
        const size_t len = ch.len == 0 ? 1 : ch.len;
        if (pending_blank) {
            if (chars + 1 >= kMailSubjectLimit) break;   // the blank and the next character no longer fit
            out.push_back(' ');
            ++chars;
            pending_blank = false;
        }
        out.append(text, i, len);
        ++chars;
        i += len;
    }
    return out;
}

}  // namespace pbc
