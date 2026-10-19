// pbcloud: Nextcloud client for the PocketBook InkPad One.
// Pattern adapted from InkFeed and notes-pocketbook (see NOTICE).

#include <sys/stat.h>

#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFile>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickWindow>
#include <QStringList>
#include <QStyleHints>
#include <QTimer>
#include <iostream>

#include <memory>
#include <vector>

#include "about_bridge.h"
#include "settings_bridge.h"
#include "table_translator.h"
#include "device_bridge.h"
#include "pbc/ca_selector.h"
#include "pbc/log.h"
#include "pbc/store.h"
#include "pbc/tz.h"
#include "pbc/version.h"
#include "pbc/types.h"
#include "worker.h"

#ifdef PLATFORM_POCKETBOOK
#include <inkview.h>
#endif

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pbcloud"));
#ifdef PLATFORM_POCKETBOOK
    // JPEG and GIF pictures: a copy of the Qt plugins travels with the app, in case the reader's own Qt has none
    QCoreApplication::addLibraryPath(QStringLiteral("/mnt/ext1/applications/pbcloud-plugins"));
#endif
    const bool self_test = app.arguments().contains(QStringLiteral("--self-test"));

    // Emoji: the monochrome Noto Emoji font (SIL OFL, see NOTICE) is added behind the default font, so
    // every Text/TextField/TextArea shows emojis without a per-item setting. A missing or unreadable font
    // is not an error: the device font is used as before.
#ifdef PLATFORM_POCKETBOOK
    const QString emoji_font = QStringLiteral("/mnt/ext1/applications/pbcloud-emoji.ttf");
#else
    const QString emoji_font = qEnvironmentVariable("PBC_EMOJI_FONT", QStringLiteral(PBC_EMOJI_FONT));
#endif
    const int emoji_id = QFontDatabase::addApplicationFont(emoji_font);
    const QStringList emoji_families = emoji_id >= 0 ? QFontDatabase::applicationFontFamilies(emoji_id) : QStringList();
    // The reader's own apps are set in Roboto: use it when the font database has it, else the default font.
    const bool roboto = QFontDatabase::families().contains(QStringLiteral("Roboto"));
    // the family that is really used (the default may be a generic name such as "Sans Serif", which is
    // not matched by name once a family list is set)
    const QString ui_family = roboto ? QStringLiteral("Roboto") : QFontInfo(app.font()).family();
    if (!emoji_families.isEmpty()) {
        QFont font = app.font();
        font.setFamilies({ui_family, emoji_families.first()});
        app.setFont(font);
    } else {
        pbc::log::event("font.emoji_missing", {});
        if (roboto) {
            QFont font = app.font();
            font.setFamilies({ui_family});
            app.setFont(font);
        }
    }

#ifdef PLATFORM_POCKETBOOK
    // PocketBook convention: app data lives under USERDATA (/mnt/ext1/system).
    const QString data_dir = QString::fromLatin1(USERDATA) + QStringLiteral("/pbcloud");
#else
    // PBC_DATA_DIR lets the host demo (scripts/run-host-demo.sh) and tests use a throw-away folder.
    const QString data_dir = qEnvironmentVariable("PBC_DATA_DIR", QStringLiteral("./pbcloud-data"));
#endif
    // The data directory is private to the app where the file system allows it (constitution: restrictive
    // permissions). The stat-after-chmod check that reports whether it took effect arrives with the store.
    mkdir(data_dir.toStdString().c_str(), 0700);

    // A blinking caret makes the e-ink panel refresh periodically; keep it static.
    app.styleHints()->setCursorFlashTime(0);

    pbc::log::event("font.ui", {pbc::log::text("family", ui_family.toStdString()), pbc::log::count("roboto", roboto ? 1 : 0)});

    // The store, the worker thread and the bridges. Declaration order is destruction order backwards: the bridges
    // wait for the worker, the worker stops before the store closes.
    const std::string data = data_dir.toStdString();
    pbc::SystemClock clock;
    std::unique_ptr<pbc::Store> store;
    QString startup_error;   // "" or the cause key shown by the storage error page
    {
        auto opened = pbc::Store::open(data, clock);
        if (opened.ok()) store = std::move(opened.value);
        else startup_error = QString::fromStdString(opened.error.key);
    }
    Worker worker;
    std::unique_ptr<pbc::TzDatabase> tzdb;
    const char* custom_ca_status = "none";   // custom-ca.pem: none | loaded | invalid (shown in Settings)
    int custom_ca_count = 0;
    std::unique_ptr<SettingsBridge> settingsBridge;
    if (store) {
        // The time zone data (IANA, compact TZif files) comes with the app, never from the device.
#ifdef PLATFORM_POCKETBOOK
        const std::string zoneinfo_dir = qEnvironmentVariable("PBC_ZONEINFO", QStringLiteral("/mnt/ext1/applications/pbcloud-zoneinfo")).toStdString();
#else
        const std::string zoneinfo_dir = qEnvironmentVariable("PBC_ZONEINFO", QStringLiteral(PBC_ZONEINFO_DIR)).toStdString();
#endif
        tzdb = pbc::TzDatabase::open(zoneinfo_dir);
        if (!tzdb) pbc::log::event("tz.unavailable", {});
        // TLS trust: the device's own store when usable, else the bundled bundle; plus the user's custom-ca.pem.
#ifdef PLATFORM_POCKETBOOK
        const std::string bundled_ca = "/mnt/ext1/applications/pbcloud-cacert.pem";
#else
        const std::string bundled_ca = qEnvironmentVariable("PBC_BUNDLED_CA", QStringLiteral(PBC_BUNDLED_CA)).toStdString();
#endif
        const pbc::CaSelection base = pbc::select_ca_file(
            {"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem", "/etc/ssl/certs/ca-bundle.crt"}, bundled_ca);
        const pbc::EffectiveCa ca = pbc::build_effective_ca(base.path, data + "/custom-ca.pem", data + "/effective-ca.pem");
        custom_ca_status = ca.custom_status == pbc::CustomCaStatus::Loaded ? "loaded" : (ca.custom_status == pbc::CustomCaStatus::Invalid ? "invalid" : "none");
        custom_ca_count = ca.custom_cert_count;
        pbc::log::event("ca.selected", {pbc::log::text("source", base.source == pbc::CaSource::Device ? "device" : "bundled"),
                                        pbc::log::count("custom_certificates", ca.custom_cert_count)});
        SettingsBridge::Config stc;
        stc.store = store.get();
        stc.custom_ca_status = QString::fromLatin1(custom_ca_status);
        stc.custom_ca_count = custom_ca_count;
        settingsBridge = std::make_unique<SettingsBridge>(stc);
    }

    DeviceBridge device;
    app.installEventFilter(&device);  // Back / Escape

    // About screens: the manifest and the license texts live in pbcloud-licenses/ next to the app on the reader,
    // and in the repository on the host (PBC_LICENSES_ROOT overrides both).
    AboutBridge::Config about_config;
#ifdef PLATFORM_POCKETBOOK
    about_config.licensesRoot = qEnvironmentVariable("PBC_LICENSES_ROOT", QStringLiteral("/mnt/ext1/applications/pbcloud-licenses"));
#else
    about_config.licensesRoot = qEnvironmentVariable("PBC_LICENSES_ROOT", QStringLiteral(PBC_LICENSES_ROOT));
#endif
    about_config.version = QString::fromUtf8(pbc::version());
    about_config.dataDir = data_dir;
    if (tzdb) about_config.tzRelease = QString::fromStdString(tzdb->version());
    AboutBridge about(about_config);

    // The interface language: English texts in the QML, German from a table (bridge/include/table_translator.h). "auto" follows the reader's language.
    TableTranslator german;
    {
        QFile table(QStringLiteral(":/qml/strings_de.tsv"));
        if (!table.open(QIODevice::ReadOnly) || !german.loadTable(table.readAll())) std::cerr << "[pbcloud] the German texts could not be read" << std::endl;
    }
    bool german_on = false;
    QQmlApplicationEngine engine;
    const auto apply_language = [&]() {
        QString want = settingsBridge ? settingsBridge->language() : QStringLiteral("auto");
        if (want == QLatin1String("auto")) {
#ifdef PLATFORM_POCKETBOOK
            const QString device = QString::fromLatin1(currentLang());   // the reader's language, e.g. "de"
#else
            const QString device = qEnvironmentVariable("PBC_LANG", QLocale::system().name());
#endif
            want = device.startsWith(QLatin1String("de"), Qt::CaseInsensitive) ? QStringLiteral("de") : QStringLiteral("en");
        }
        const bool on = want == QLatin1String("de") && !german.isEmpty();
        if (on == german_on) return;
        german_on = on;
        if (on) QCoreApplication::installTranslator(&german);
        else QCoreApplication::removeTranslator(&german);
        engine.retranslate();
    };
    apply_language();
    if (settingsBridge) QObject::connect(settingsBridge.get(), &SettingsBridge::languageChanged, &app, apply_language);
    int qml_warnings = 0;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app, [&](const QList<QQmlError>& list) {
        for (const QQmlError& e : list) std::cerr << "[pbcloud] QML: " << e.toString().toStdString() << std::endl;
        qml_warnings += list.size();
    });
    engine.rootContext()->setContextProperty(QStringLiteral("device"), &device);
    engine.rootContext()->setContextProperty(QStringLiteral("about"), &about);
    engine.rootContext()->setContextProperty(QStringLiteral("startupError"), startup_error);
    if (store) engine.rootContext()->setContextProperty(QStringLiteral("settings"), settingsBridge.get());
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::cerr << "[pbcloud] failed to load QML" << std::endl;
        return 1;
    }
    if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) device.setWindow(window);

    if (self_test) {  // CTest: every page compiles and instantiates without QML errors
        QTimer::singleShot(300, &app, [&]() {
            int failed = 0;
            for (const QString& name : QDir(QStringLiteral(":/qml/pages")).entryList(QStringList() << QStringLiteral("*.qml"))) {
                QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qml/pages/") + name));
                QObject* page = component.isError() ? nullptr : component.create();
                if (!page) {
                    std::cerr << "[pbcloud] page failed: " << name.toStdString() << " " << component.errorString().toStdString() << std::endl;
                    ++failed;
                }
                delete page;
            }
            // a blank screen (no page shown) is a failure too
            auto* window = engine.rootObjects().isEmpty() ? nullptr : engine.rootObjects().first();
            const bool shown = window && window->property("pageLoaded").toBool();
            if (!shown) std::cerr << "[pbcloud] no page is shown" << std::endl;
            QTimer::singleShot(300, &app, [&, failed, shown]() { app.exit((failed == 0 && shown && qml_warnings == 0) ? 0 : 2); });
        });
    }
    return app.exec();
}
