#pragma once
// A secret string (specs/004-share-links, research R-9): a password the user typed for a share link. Move-only, wiped when it dies or on
// request (secure_wipe.h), and without a copy, a stream operator or a conversion to a plain string, so that a later careless change cannot
// log it or leave a second copy behind. The only way to read it is reveal(), which the form encoder of share_api.cpp alone may use
// (rule N8 forbids the word in the other share modules). Best effort for copies the runtime made before the value arrived here.

#include <cstddef>
#include <string>

namespace pbc {

class SecretText {
public:
    SecretText() = default;
    // Takes the buffer of the argument (no copy remains); an lvalue is refused at compile time.
    explicit SecretText(std::string&& value);
    SecretText(SecretText&& other) noexcept;
    SecretText& operator=(SecretText&& other) noexcept;
    SecretText(const SecretText&) = delete;
    SecretText& operator=(const SecretText&) = delete;
    ~SecretText();

    bool empty() const { return value_.empty(); }
    size_t size() const { return value_.size(); }
    const std::string& reveal() const { return value_; }
    // Content checks that spare the validators from reading the value (rule N8 keeps reveal() out of the other share modules).
    bool has_control_character() const;   // a byte below 0x20 or 0x7f
    bool all_blank() const;               // only spaces and tabs (or empty)
    // Overwrites the whole buffer with zeros, then empties the string.
    void wipe();

private:
    std::string value_;
};

}  // namespace pbc
