// pbc-handoff: a small InkView program that answers one question for the "Mail link" idea (a PB app handing a piece of text to PB Office): when an app
// starts another .app with OpenBook/OpenBook2, does the other app receive the parameters, and what happens to the caller? Nothing here is secret:
// the texts handed over are fixed test strings. It is started by the launcher when the file run-handoff-probe exists in the data folder (INSTALL.md,
// "Hand-off probe") and writes only to <data-dir>/handoff-probe.txt. It has no network, no Qt and no personal data.
//
//   pbc-handoff [data-dir]     data-dir defaults to /mnt/ext1/system/pbcloud
//
// Four boxes on the screen: A = OpenBook(target, one parameter string); B = OpenBook2(target, three arguments with a space, non-ASCII letters and a
// line break); C = OpenBook2(target, one argument of 1500 characters). The target is pbcloud-handoff-target.app, which records what it received.
// D = OpenBook2 to the installed PB Office with the fixed test text, subject and recipient of its contract "pbo-mail-v1" (PB Office opens a new draft;
// nothing is sent). D works for a PB Office that is not running and for one that is.
#include <inkview.h>

#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

const char* kTarget = "/mnt/ext1/applications/pbcloud-handoff-target.app";
const char* kPbOffice = "/mnt/ext1/applications/pboffice.app";
const int kBoxes = 4;
std::string g_log;
ifont* g_font = nullptr;
bool g_started = false;   // a hand-off was tried: every later event is written down (the caller may be hidden, suspended or closed)
int g_logged_after = 0;

void note(const std::string& line) {
    if (FILE* f = std::fopen(g_log.c_str(), "a")) {
        std::fprintf(f, "caller: t=%ld %s\n", static_cast<long>(std::time(nullptr)), line.c_str());
        std::fflush(f);
        std::fclose(f);
    }
}

struct Box { int x, y, w, h; const char* text; };
Box box(int i) {
    const int w = ScreenWidth(), h = ScreenHeight();
    const char* texts[kBoxes] = {"A: OpenBook(one parameter string)", "B: OpenBook2(3 arguments)", "C: OpenBook2(1 argument of 1500 characters)",
                                 "D: PB Office, mail contract pbo-mail-v1 (test text)"};
    const int top = h / 5, each = (h - top - h / 10) / kBoxes;
    return {w / 20, top + i * each, w - w / 10, each - h / 40, texts[i]};
}

void draw() {
    ClearScreen();
    if (g_font) SetFont(g_font, BLACK);
    DrawTextRect(0, ScreenHeight() / 20, ScreenWidth(), ScreenHeight() / 10, "Hand-off probe: tap a box, read system/pbcloud/handoff-probe.txt", ALIGN_CENTER);
    for (int i = 0; i < kBoxes; ++i) {
        const Box b = box(i);
        FillArea(b.x, b.y, b.w, b.h, BLACK);
        FillArea(b.x + 3, b.y + 3, b.w - 6, b.h - 6, WHITE);
        DrawTextRect(b.x, b.y, b.w, b.h, b.text, ALIGN_CENTER);
    }
    FullUpdate();
}

int hit(int x, int y) {
    for (int i = 0; i < kBoxes; ++i) {
        const Box b = box(i);
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return i;
    }
    return -1;
}

void start(int which) {
    g_started = true;
    int rv = -999;
    if (which == 0) {
        note("calling OpenBook(target, \"HANDOFF-A a\xc3\xa4 \xc3\xb6 \xe2\x82\xac 'q' \\\"d\\\" 123\", 0)");
        rv = OpenBook(kTarget, "HANDOFF-A a\xc3\xa4 \xc3\xb6 \xe2\x82\xac 'q' \"d\" 123", 0);
    } else if (which == 1) {
        const char* args[3] = {"HANDOFF-B first argument", "second \xc3\xa4\xc3\xb6\xc3\xbc \xe2\x82\xac", "line1\nline2"};
        note("calling OpenBook2(target, 3 arguments, 0)");
        rv = OpenBook2(kTarget, 3, args, 0);
    } else if (which == 3) {
        const std::string text = "HANDOFF-PROBE-7f3a9c PB Cloud test text, line one.\nLine two with \xc3\xa4\xc3\xb6\xc3\xbc \xe2\x82\xac and https://example.org/s/TESTONLY";
        const std::string textlen = "textlen=" + std::to_string(text.size());
        const std::string text_arg = "text=" + text;
        const char* args[5] = {"pbo-mail-v1", text_arg.c_str(), textlen.c_str(), "subject=HANDOFF-PROBE-7f3a9c-subject from PB Cloud", "to=probe@example.org"};
        note("calling OpenBook2(PB Office, 5 arguments of the contract, 0), text of " + std::to_string(text.size()) + " bytes");
        rv = OpenBook2(kPbOffice, 5, args, 0);
    } else {
        std::string text = "HANDOFF-C-START-";
        text.append(1500 - 16 - 4, 'x');
        text += "-END";
        const char* args[1] = {text.c_str()};
        note("calling OpenBook2(target, 1 argument of " + std::to_string(text.size()) + " bytes, 0)");
        rv = OpenBook2(kTarget, 1, args, 0);
    }
    note("the call returned " + std::to_string(rv));
}

int handler(int type, int p1, int p2) {
    if (!g_started && (type == EVT_SHOW || type == EVT_FOREGROUND || type == EVT_BACKGROUND || type == EVT_POINTERUP)) {
        note("before any hand-off: event " + std::to_string(type) + " p1=" + std::to_string(p1) + " p2=" + std::to_string(p2) +
             (type == EVT_POINTERUP ? " box=" + std::to_string(hit(p1, p2)) : std::string()));
    }
    if (g_started && g_logged_after < 200 && type != EVT_POINTERUP && type != EVT_POINTERDOWN && type != EVT_POINTERMOVE && type != EVT_POINTERDRAG) {
        ++g_logged_after;
        note("event " + std::to_string(type) + " p1=" + std::to_string(p1));
    }
    switch (type) {
        case EVT_INIT:
            g_font = OpenFont("LiberationSans", 30, 1);
            note("started (event INIT)");
            break;
        case EVT_SHOW:
            draw();
            break;
        case EVT_POINTERUP: {
            const int i = hit(p1, p2);
            if (i >= 0) start(i);
            break;
        }
        case EVT_KEYPRESS:
            if (p1 == IV_KEY_BACK || p1 == IV_KEY_HOME || p1 == IV_KEY_MENU) {
                note("closing (key)");
                CloseApp();
            }
            break;
        case EVT_EXIT:
            note("event EXIT");
            break;
        default:
            break;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "/mnt/ext1/system/pbcloud";
    g_log = dir + "/handoff-probe.txt";
    InkViewMain(handler);
    return 0;
}
