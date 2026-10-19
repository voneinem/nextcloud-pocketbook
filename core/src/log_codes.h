#pragma once
// The event codes the application may log (constitution Principle IV, contracts/core-interfaces.md §9).
// A code is a short dotted identifier ("area.what"); it never carries data. Adding a code here is a reviewed
// change: tests/unit/test_log.cpp fails when a source file logs a code that is not listed, and when a listed
// code is no longer used.

#include <iterator>
#include <string>

namespace pbc {
namespace log {

inline constexpr const char* kAllowedCodes[] = {
    "about.bad_blocks",
    "about.opened",
    "about.text_unavailable",
    "ca.selected",
    "credentials.forgotten",
    "credentials.saved",
    "credentials.unreadable",
    "credentials.write_failed",
    "font.emoji_missing",
    "font.ui",
    "network.connect",
    "rotate.begin",
    "rotate.done",
    "rotate.geometry_failed",
    "store.create_failed",
    "store.migration_failed",
    "store.write_failed",
    "tz.unavailable",
};

inline bool is_allowed_code(const std::string& code) {
    for (const char* c : kAllowedCodes)
        if (code == c) return true;
    return false;
}

}  // namespace log
}  // namespace pbc
