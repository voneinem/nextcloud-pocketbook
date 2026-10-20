#include "pbc/auth_guard.h"

#include <cstdlib>

namespace pbc {

namespace {
constexpr const char* kCountKey = "auth_fail_count";
constexpr int kLimit = 2;
}  // namespace

bool AuthGuard::blocked() const {
    const auto a = store_->account();
    return a && a->state == "needs_signin";
}

void AuthGuard::record(const Error& outcome) {
    if (!store_->account()) return;
    if (outcome.ok()) {
        store_->set_setting(kCountKey, "0");
        return;
    }
    if (outcome.key != "auth_failed" && outcome.key != "auth_app_password_required") return;
    const int count = std::atoi(store_->setting(kCountKey).c_str()) + 1;
    store_->set_setting(kCountKey, std::to_string(count));
    if (count >= kLimit) store_->set_account_state("needs_signin", outcome.key);
}

void AuthGuard::reset() {
    store_->set_setting(kCountKey, "0");
    const auto a = store_->account();
    if (a && a->state == "needs_signin") store_->set_account_state("untested", "");
}

}  // namespace pbc
