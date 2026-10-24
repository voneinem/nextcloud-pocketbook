#pragma once
// The one place where the app hands a text to the mail app (specs/005-mail-link, contracts/handoff-call.md section 2). No Qt, no logging.

#include <string>
#include <vector>

// Passes the argument list to the installed mail app with ONE system call and returns the system's answer: 0 = passed on, anything else = not.
// Keeps nothing and logs nothing. The caller (ShareBridge) overwrites its buffers afterwards.
int send_mail_handoff(const std::vector<std::string>& argv);
