# Data Model: Mail Link

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Research**: [research.md](research.md)

**No schema change.** Nothing here is stored: every entity is in memory for the duration of one action. `pbcloud.db` stays at schema 5.

## Entities

### MailHandoff (core, `pbc/mail_handoff.h`)

The argument list of one hand-off. Move-only, wipes itself.

| Field | Meaning | Rule |
|-------|---------|------|
| `argv` | four strings, in this order | `pbo-mail-v1`, `text=<link>`, `textlen=<n>`, `subject=<subject>` |
| `text_bytes` | byte length of the link | 1 .. 8192; equals `n` in `textlen=` |

Built by `build_mail_handoff(link, subject_line)`; the error is one of `None`, `LinkEmpty`, `LinkTooLong` (> 8192 bytes), `LinkInvalid` (control
character or invalid UTF-8). There is no password and no recipient field; adding one would require changing the type.

### Subject line (core, `clean_subject`)

Input: the already display-safe item name and the translated prefix. Output: one line, at most 300 characters (code points), control characters and line
breaks replaced by one space, runs of blanks collapsed, trimmed, cut at a character boundary. An empty item name gives the prefix alone.

### MailNotice (bridge)

A key (`""` or `mail_not_accepted`) and nothing else. Set by a refused hand-off; survives `clearSecrets()`; cleared by `dismissMailNotice()`.

### Link screen state (bridge, feature 004)

Unchanged. The hand-off reads `detail_` (id, link state, can-change not needed) and `link_text_`, and ends with `leaveLink()` on acceptance.

## State machine of one hand-off

```
 link screen shown
   | tap "Mail link"          mailLink(false): link readable? not busy? not in flight?
   v                                no  -> {errorKey} (busy | share_item_gone | mail_link_unavailable | mail_too_long), nothing else
 confirming  -- Cancel / leave screen / app switched away --> link screen (no call) | closed
   | confirm                  mailLink(true)
   v
 handing   (in_flight set; build MailHandoff; ONE call; wipe MailHandoff and the temporaries)
   | answer 0                          | answer != 0, or no function injected
   v                                   v
 accepted: leaveLink(), mailHandedOver   refused: mailNotice = mail_not_accepted; link screen stays if the app is
 page pops; in_flight cleared            still active (banner), else cleared with the other sharing screens;
 "sent" flag blocks a second call        the notice shows when the app is shown again; no retry
```

A second `mailLink(true)` while `handing` or after `accepted` (until the link is left) is answered `{errorKey: "busy"}` and makes no call.

## Lifetime of every copy of the link (extends feature 004's table)

| Copy | Created | Ends |
|------|---------|------|
| `link_text_` (QString, shown on the screen) | when the link screen opens (feature 004) | `leaveLink()` / `clearSecrets()`, wiped in place |
| UTF-8 bytes for the builder (`QByteArray` and `std::string`) | `mailLink(true)` | overwritten right after `build_mail_handoff` returns |
| `MailHandoff::argv[1]` (`text=` + link) | `build_mail_handoff` | `wipe()` right after the call returned; destructor as a second line |
| pointers passed to `OpenBook2` | inside `send_mail_handoff` | end of that function (they point into `argv`, no copy) |
| the system's copy (message to PB Office) | the call | not controlled by this app; stated in the README |
| the clipboard | never | not applicable (no clipboard on the reader; the host build clears it as in feature 004) |
| log, database, settings, cache, exports | never | tests search for the link (SC-003, SC-006) |

The subject and the item name are treated the same way (wiped with the arguments); they are not secrets in the spec's sense but are never logged.

## Validation rules

- The tool is offered only for a link with state `ok` or `not_secure` (a readable link); never for `unreadable` or `different_address`.
- The tool is unavailable while `busy` (a sharing request runs).
- The link must be non-empty, at most 8192 bytes, valid UTF-8 without control characters.
- The answer 0 of the hand-off function is the only success.
