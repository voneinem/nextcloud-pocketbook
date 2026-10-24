# Contract: bridge and QML additions

**Feature**: [spec.md](../spec.md) | **Contracts**: [handoff-call.md](handoff-call.md) | Feature 004: [bridge-qml.md](../../004-share-links/contracts/bridge-qml.md)

Everything not listed here is unchanged from feature 004.

## ShareBridge additions

```cpp
struct Config {   // one new member
    // Hands the argument list to the mail app and returns the system's answer (0 = passed on). Empty = no hand-off possible (treated as not accepted).
    std::function<int(const std::vector<std::string>&)> hand_off;
};

Q_PROPERTY(QString mailNotice READ mailNotice NOTIFY mailNoticeChanged)   // "" or a key (mail_not_accepted); no secret, no item name

// Asks (confirmed = false) or does (confirmed = true) the hand-off of the open link.
//   confirmed = false: {needsConfirm: true, text} - the confirmation text; nothing is sent
//   confirmed = true : {ok: true} after an accepted call (the link is already dropped: mailHandedOver follows), or {errorKey} after a refused one
//   other answers    : {errorKey} with nothing sent: share_item_gone (no link open), mail_link_unavailable (unreadable or foreign host),
//                      mail_too_long (> 8192 bytes), busy (a sharing request or a hand-off is running, or an accepted one is done)
Q_INVOKABLE QVariantMap mailLink(bool confirmed);
Q_INVOKABLE void dismissMailNotice();     // clears mailNotice

signals:
    void mailHandedOver();                // the call was accepted; the link is gone; the link page pops
    void mailNoticeChanged();
```

Behaviour:

- `clearSecrets()` does **not** clear `mailNotice` (it holds a key only). It clears `mail_in_flight_` and the accepted flag with the link.
- `onApplicationStateChanged` is unchanged: a non-active state clears the sharing screens; with a pending notice the notice stays.
- `mailLink(true)` builds the subject from `item_name_` with the translated prefix `bridge_tr("Link to %1")`, builds the list, calls `c_.hand_off` once,
  wipes everything it created (arguments, temporaries) and returns. It makes no request and logs nothing (rules N8, N10).

## QML

**`ShareLinkPage.qml`**

- A `ToolButton { objectName: "shareLinkMail"; text: qsTr("Mail link"); icon: "send"; visible: link state is ok or not_secure; enabled: !share.busy }`
  in the bottom panel; `onClicked: page.askMail()`.
- `askMail()` calls `share.mailLink(false)`; `needsConfirm` opens `ConfirmDialog { objectName: "shareMailConfirm"; title: qsTr("Mail link"); acceptText: qsTr("Mail link") }`
  with the returned text; an `errorKey` shows as the page's message banner. `onAccepted: share.mailLink(true)`; a result with `errorKey` shows the banner
  (`mail_not_accepted` while the app is still active).
- `Connections { target: share; function onMailHandedOver() { pop } }`.

**`Main.qml`**: `ConfirmDialog { objectName: "mailNoticeDialog"; showCancel: false; title: Problems.title("mail_not_accepted"); text: Problems.action("mail_not_accepted") }`
opened when `share.mailNotice !== ""` and the application is active; `onAccepted`/`onRejected` call `share.dismissMailNotice()`.

**`Problems.qml`** (new keys): `mail_not_accepted`, `mail_too_long`, `mail_link_unavailable` (titles and action lines below).

## Texts (English source; German in `qml/strings_de.tsv`)

| Key / place | English |
|-------------|---------|
| Tool label | Mail link |
| Confirmation (text) | The link will be passed to PB Office as a new mail draft. A password is not included: tell it to the recipient separately. The draft is saved to your Drafts folder on the mail server later. |
| Confirmation, link without a password | The link will be passed to PB Office as a new mail draft. The draft is saved to your Drafts folder on the mail server later. |
| Subject prefix (bridge) | Link to %1 |
| `mail_not_accepted` title | PB Office could not take the link. |
| `mail_not_accepted` action | Close PB Office and try again. If PB Office is not installed, the link cannot be mailed. |
| `mail_too_long` title / action | The link is too long to mail. / Type it from the screen instead. |
| `mail_link_unavailable` title / action | This link cannot be mailed. / Open the link again or check the address. |

The notice and every message contain no link, no subject and no item name.

## Host walk verbs (`app/ui_script`, host build only)

`mail-link` (tap the tool), `confirm-mail` (accept the confirmation), `cancel-mail`, `dismiss-notice`, `set-handoff <n>` (sets the stand-in's answer); the plan's tree lists them by their first three names.
