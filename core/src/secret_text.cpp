#include "pbc/secret_text.h"

#include <utility>

#include "pbc/secure_wipe.h"

namespace pbc {

SecretText::SecretText(std::string&& value) : value_(std::move(value)) {}

SecretText::SecretText(SecretText&& other) noexcept : value_(std::move(other.value_)) { other.value_.clear(); }

SecretText& SecretText::operator=(SecretText&& other) noexcept {
    if (this != &other) {
        wipe();
        value_ = std::move(other.value_);
        other.value_.clear();
    }
    return *this;
}

SecretText::~SecretText() { wipe(); }

bool SecretText::has_control_character() const {
    for (unsigned char c : value_)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
}

bool SecretText::all_blank() const {
    for (char c : value_)
        if (c != ' ' && c != '\t') return false;
    return true;
}

void SecretText::wipe() {
    // wipe the whole capacity that holds characters of the secret, then drop the length
    if (!value_.empty()) secure_wipe(&value_[0], value_.size());
    value_.clear();
}

}  // namespace pbc
