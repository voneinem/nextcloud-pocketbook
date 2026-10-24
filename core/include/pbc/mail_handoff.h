#pragma once
// What is handed to the mail app when the user mails a share link (specs/005-mail-link, contracts/handoff-call.md). Qt-free, InkView-free and
// free of any I/O: it only builds the argument list and checks the limits of the receiver's interface. The list holds the link, so it is
// move-only and overwrites its strings (wipe, destructor). There is no parameter for a password and none for a recipient: they cannot be handed over.

#include <cstddef>
#include <string>
#include <vector>

namespace pbc {

constexpr size_t kMailTextLimit = 8192;     // bytes of the text (limit of the receiver's interface, provisional there)
constexpr size_t kMailSubjectLimit = 300;   // characters of the subject

enum class MailError { None, LinkEmpty, LinkTooLong, LinkInvalid };

class MailHandoff {
public:
    MailHandoff() = default;
    MailHandoff(MailHandoff&& other) noexcept;
    MailHandoff& operator=(MailHandoff&& other) noexcept;
    MailHandoff(const MailHandoff&) = delete;
    MailHandoff& operator=(const MailHandoff&) = delete;
    ~MailHandoff();

    // {marker, "text=<link>", "textlen=<n>", "subject=<subject>"}; empty when the build failed or after wipe()
    const std::vector<std::string>& argv() const { return argv_; }
    // Overwrites the bytes of every string with zeros and empties the strings.
    void wipe();

private:
    friend MailHandoff make_handoff(std::vector<std::string>&& argv);
    std::vector<std::string> argv_;
};

struct MailBuild {
    MailError error = MailError::None;
    MailHandoff handoff;
};

// Builds the list from the link (taken by rvalue: its buffer is moved into the list and wiped on every failure) and the subject line, which the caller
// cleaned with clean_subject(). Refuses an empty link, a link over kMailTextLimit bytes and a link with a control character or invalid UTF-8.
MailBuild build_mail_handoff(std::string&& link, const std::string& subject_line);

// One line of at most kMailSubjectLimit characters: control characters and line breaks become a space, blank runs collapse, the ends are trimmed, and
// the cut is at a character boundary (a character that would straddle the limit is dropped whole).
std::string clean_subject(const std::string& text);

}  // namespace pbc
