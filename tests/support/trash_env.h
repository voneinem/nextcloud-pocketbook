#pragma once
// The environment of the trash and versions tests that talk to the in-process fake Nextcloud through the core (specs/006-trash-and-versions): the
// server, a session, the API, and the helpers that turn a trash name into the item the app would hold. Test infrastructure: it proves the app against
// this harness, never against a real server (constitution Principle IX).

#include <string>

#include "fake_nextcloud_server.h"
#include "pbc/dav_common.h"
#include "pbc/trash_api.h"
#include "test_support.h"

namespace pbtest {

struct TrashEnv {
    FakeNextcloud server;
    pbc::SystemClock clock;
    pbc::DavSession session;
    pbc::TrashApi api;
    TrashEnv() : session(server, server.base_url(), "demo", "pw"), api(session, "demo", clock) {
        server.set_account("demo", "demo", "pw");
        server.set_server_time(1792497600);
    }
    int64_t t(int64_t ago) const { return 1792497600 - ago; }
    // the item as the list gives it
    pbc::TrashItem item(const std::string& trash_name) {
        const auto l = api.list_trash();
        if (!l.ok()) PB_FAIL_AT("the trash cannot be listed: " + l.error.key);
        for (const auto& i : l.value.items)
            if (i.trash_name == trash_name) return i;
        PB_FAIL_AT("not in the trash: " + trash_name);
    }
    size_t moves() const { return server.count("MOVE"); }
};

}  // namespace pbtest
