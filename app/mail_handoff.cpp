#include "mail_handoff.h"

#include <cstdlib>

#ifdef PLATFORM_POCKETBOOK
#include <inkview.h>
#endif

// The only OpenBook2 call of the product, and the only place that names the launcher of PB Office (constitution 1.2.0, rules R11 and N10). It is made
// after the user's confirmed tap and passes a text and a subject only: the arguments are exactly those that core/src/mail_handoff.cpp built.
int send_mail_handoff(const std::vector<std::string>& argv) {
#ifdef PLATFORM_POCKETBOOK
    std::vector<const char*> pointers;   // pointers into the caller's strings: no copy of the link is made here
    pointers.reserve(argv.size());
    for (const std::string& a : argv) pointers.push_back(a.c_str());
    return OpenBook2("/mnt/ext1/applications/pboffice.app", static_cast<int>(pointers.size()), pointers.data(), 0);
#else
    // The host has no mail app: the answer comes from the environment so that the tests and the walk can choose it (0 = passed on).
    (void)argv;
    const char* answer = std::getenv("PBC_HOST_HANDOFF_RESULT");
    if (!answer || !*answer) return 0;
    char* end = nullptr;
    const long v = std::strtol(answer, &end, 10);
    return (end && !*end) ? static_cast<int>(v) : 1;
#endif
}
