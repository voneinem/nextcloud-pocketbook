#include "ui_script.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QTextStream>
#include <QTimer>

#include <iostream>

namespace ui_script {

namespace {

void pump(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

QList<QQuickItem*> find_all(QQuickItem* root, const QString& name) {
    QList<QQuickItem*> out;
    std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
        if (!item) return;
        if (item->objectName() == name && item->isVisible()) out.append(item);
        for (QQuickItem* c : item->childItems()) walk(c);
    };
    walk(root);
    return out;
}

// the text of an item and, for a row of a list, its value: "On the reader  Yes"
QString item_text(QQuickItem* item) {
    QStringList parts;
    for (const char* prop : {"text", "value"}) {
        const QVariant v = item->property(prop);
        if (v.isValid() && !v.toString().isEmpty()) parts << v.toString();
    }
    return parts.join(QLatin1String("  "));
}

}  // namespace

void start(QQuickWindow* window, const QString& path) {
    QTimer::singleShot(400, qApp, [window, path]() {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "[walk] cannot read the script" << std::endl;
            qApp->exit(3);
            return;
        }
        QTextStream in(&f);
        int failures = 0, lineno = 0, taps = 0;
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            ++lineno;
            if (line.isEmpty() || line.startsWith('#')) continue;
            const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            const QString cmd = parts.value(0);
            const QString rest = line.mid(cmd.size()).trimmed();
            const auto fail = [&](const QString& why) {
                std::cerr << "[walk] line " << lineno << " FAILED: " << line.toStdString() << " -> " << why.toStdString() << std::endl;
                ++failures;
            };
            if (cmd == "size") {
                window->setVisibility(QWindow::Windowed);
                window->resize(parts.value(1).toInt(), parts.value(2).toInt());
                pump(300);
            } else if (cmd == "wait") {
                pump(parts.value(1).toInt());
            } else if (cmd == "tap") {
                const auto items = find_all(window->contentItem(), parts.value(1));
                const int n = parts.value(2).toInt();
                if (n >= items.size()) { fail(QStringLiteral("no such item (%1 found)").arg(items.size())); continue; }
                QQuickItem* item = items.at(n);
                const QPointF center = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
                ++taps;
                pump(150);
            } else if (cmd == "type") {
                for (const QChar c : rest) QTest::keyClick(window, c.toLatin1());
                pump(150);
            } else if (cmd == "key") {
                const QString k = parts.value(1);
                QTest::keyClick(window, k == "Return" ? Qt::Key_Return : k == "Backspace" ? Qt::Key_Backspace : Qt::Key_Escape);
                pump(150);
            } else if (cmd == "shot") {
                pump(200);
                if (!window->grabWindow().save(parts.value(1))) fail(QStringLiteral("cannot save the picture"));
            } else if (cmd == "text") {
                const auto items = find_all(window->contentItem(), parts.value(1));
                const int n = parts.value(2).toInt();
                std::cout << "[walk] " << parts.value(1).toStdString() << ": " << (n < items.size() ? item_text(items.at(n)).toStdString() : std::string("(absent)")) << std::endl;
            } else if (cmd == "expect") {
                const auto items = find_all(window->contentItem(), parts.value(1));
                const QString want = line.section(' ', 2);
                QString all;
                for (QQuickItem* it : items) all += item_text(it) + "\n";
                if (items.isEmpty()) fail(QStringLiteral("item absent"));
                else if (!all.contains(want)) fail(QStringLiteral("text is: ") + all.simplified());
            } else if (cmd == "waitfor") {   // waitfor <objectName> [substring]: until it is there (20 s), so that a slow machine does not fail the walk
                const QString want = line.section(' ', 2);
                QElapsedTimer t;
                t.start();
                bool found = false;
                while (!found && t.elapsed() < 20000) {
                    for (QQuickItem* it : find_all(window->contentItem(), parts.value(1)))
                        if (want.isEmpty() || item_text(it).contains(want)) found = true;
                    if (!found) pump(50);
                }
                if (!found) fail(QStringLiteral("not there after 20 s"));
            } else if (cmd == "expect_absent") {
                if (!find_all(window->contentItem(), parts.value(1)).isEmpty()) fail(QStringLiteral("item is visible"));
            } else if (cmd == "taps_reset") {
                taps = 0;
            } else if (cmd == "expect_taps_max") {   // the taps since taps_reset must not be more than this (the step budgets of the success criteria)
                if (taps > parts.value(1).toInt()) fail(QStringLiteral("%1 taps since taps_reset").arg(taps));
            } else if (cmd == "quit") {
                break;
            } else {
                fail(QStringLiteral("unknown command"));
            }
        }
        qApp->exit(failures == 0 ? 0 : 3);
    });
}

}  // namespace ui_script
