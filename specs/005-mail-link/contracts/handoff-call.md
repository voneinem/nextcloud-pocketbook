# Contract: the hand-off call (PB Cloud side)

**Feature**: [spec.md](../spec.md) | **Research**: [R-1 .. R-6](../research.md)

This is the interface between PB Cloud (the sender) and PB Office (the receiver). The receiver's side is PB Office's own
`specs/003-receive-shared-text/contracts/handoff-contract.md` (v1); this page states what PB Cloud sends and what it does with the answer. Status of every
statement about the reader: [Status](#status).

## 1. The core interface (`core/include/pbc/mail_handoff.h`, Qt-free)

```cpp
namespace pbc {

constexpr size_t kMailTextLimit = 8192;       // bytes of the text (PB Office contract v1, provisional)
constexpr size_t kMailSubjectLimit = 300;     // characters of the subject

enum class MailError { None, LinkEmpty, LinkTooLong, LinkInvalid };

class MailHandoff {                           // move-only; the destructor and wipe() overwrite every string
public:
    const std::vector<std::string>& argv() const;   // {"pbo-mail-v1", "text=…", "textlen=…", "subject=…"}
    void wipe();
    MailHandoff(MailHandoff&&) noexcept;  MailHandoff& operator=(MailHandoff&&) noexcept;
    MailHandoff(const MailHandoff&) = delete;  MailHandoff& operator=(const MailHandoff&) = delete;
    ~MailHandoff();
};

struct MailBuild { MailError error = MailError::None; MailHandoff handoff; };

// The link is taken as an rvalue (its buffer is moved and wiped); the subject line is already the cleaned subject.
MailBuild build_mail_handoff(std::string&& link, const std::string& subject_line);
// Control characters and line breaks -> one space, blank runs collapsed, trimmed, at most kMailSubjectLimit characters, cut at a character boundary.
std::string clean_subject(const std::string& prefix_and_name);

}  // namespace pbc
```

There is **no password parameter and no recipient parameter**. The key names and the marker are string literals in `core/src/mail_handoff.cpp` only
(rule N10).

## 2. The device function (`app/mail_handoff.h`)

```cpp
// Hands the argument list to the installed mail app with ONE system call and returns the system's answer: 0 = passed on, anything else = not.
// Keeps nothing, logs nothing.
int send_mail_handoff(const std::vector<std::string>& argv);
```

Device: `OpenBook2(<launcher path of the mail app>, argv.size(), <pointers into argv>, 0)`; the path is a literal in `app/mail_handoff.cpp` only.
Host: returns the integer in the environment variable `PBC_HOST_HANDOFF_RESULT` (default 0) and does nothing else; used by the walk and the demo.

## 3. What PB Cloud sends (exactly)

| Index | Value | Note |
|-------|-------|------|
| 0 | `pbo-mail-v1` | marker and version |
| 1 | `text=<the link>` | complete link, no other text |
| 2 | `textlen=<n>` | decimal byte length of the link |
| 3 | `subject=<one line>` | `Link to <item name>` (translated prefix), at most 300 characters |

No `to=`, no other key. The flags of the call are `0` (a running PB Office is not supported: R-1).

## 4. What PB Cloud does with the answer

| Answer | Meaning | PB Cloud |
|--------|---------|----------|
| 0 | passed on | `leaveLink()`, signal `mailHandedOver`, the link page pops; nothing is shown |
| anything else | not passed on (observed: 1 for a running PB Office) | `mailNotice = mail_not_accepted`; no second call |
| no function injected | host build without a stand-in | treated like "anything else" |

## 5. What is promised and what is not

- Exactly one call per confirmation; none without confirmation; none twice for the same link screen after an accepted call.
- The password never enters the list; the link never enters a log, a file, a database, a setting or a clipboard.
- The buffers PB Cloud controls are overwritten right after the call returned. The copy that the system passes to PB Office, and PB Office's own handling
  of it (a private pending folder, a draft, later the server's Drafts folder), are outside PB Cloud's control.
- PB Cloud makes no statement about a running PB Office.

## Status

| Statement | Status |
|-----------|--------|
| Cold PB Office accepts the call (0) and opens a draft with the text and subject | **observed** 2026-10-23/24 (device findings) |
| A running PB Office returns 1 and receives nothing | **observed** 2026-10-24 |
| PB Cloud's own tool (not the probe) behaves the same | **unverified** until DT-M1 |
| The answer when PB Office is not installed | **unverified** until DT-M2 |
| Nothing of the link in PB Cloud's logs and files after a real use | **unverified** until DT-M3 |
