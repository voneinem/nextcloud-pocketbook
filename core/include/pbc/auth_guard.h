#pragma once
// After two consecutive refused logins (401) the app stops sending requests for the account until the user tests again (research R-12,
// api-notes section 9: repeated wrong passwords are what triggers the server's brute-force protection). The block lives in the account
// row (state needs_signin), so it survives a restart. Qt-free.

#include "pbc/store.h"
#include "pbc/types.h"

namespace pbc {

class AuthGuard {
public:
    explicit AuthGuard(Store& store) : store_(&store) {}

    // True while the account is in the needs_signin state: no request may be sent.
    bool blocked() const;
    // Feeds the outcome of a request that carried the account's credentials. A refused login counts, a success clears the count and records the time of the last confirmed login, any other
    // error neither counts nor clears. Without an account nothing happens (the first Test of a new account is always allowed).
    void record(const Error& outcome);
    // The user tapped Test: lifts the block and starts the count again.
    void reset();

private:
    Store* store_;
};

}  // namespace pbc
