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
#include "picker_bridge.h"
#include "folder_chooser_bridge.h"
#include "queue_bridge.h"
#include "browse_bridge.h"
#include "connection_bridge.h"
#include "qr_image_provider.h"
#include "settings_bridge.h"
#include "space_bridge.h"
#ifndef PLATFORM_POCKETBOOK
#include "ui_script.h"
#endif
#include "transfer_bridge.h"
#include "table_translator.h"
#include "device_bridge.h"
#include "pbc/ca_selector.h"
#include "pbc/curl_transport.h"
#include "pbc/log.h"
#include "pbc/reader_zone.h"
#include "pbc/storage_info.h"
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
    std::string ca_file;   // the certificate authorities every connection is checked against (device store or bundled, plus custom-ca.pem)
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
        ca_file = ca.path;
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

    // The connection to the user's Nextcloud. The transport verifies every certificate and accepts only https (constitution IV). It is
    // declared before the bridge, so the bridge (which waits for the worker) is destroyed first.
    std::unique_ptr<pbc::CurlTransport> http;
    std::unique_ptr<ConnectionBridge> connectionBridge;
    std::unique_ptr<BrowseBridge> browseBridge;
    std::unique_ptr<TransferBridge> transferBridge;
    std::unique_ptr<SpaceBridge> spaceBridge;
    std::unique_ptr<PickerBridge> pickerBridge;
    std::unique_ptr<QueueBridge> queueBridge;
    std::unique_ptr<FolderChooserBridge> chooserBridge;
    // where the downloads go: a folder the reader's library scans on the device, a throw-away folder on the host
#ifdef PLATFORM_POCKETBOOK
    const QString download_root = QStringLiteral("/mnt/ext1/PBCloud");
#else
    const QString download_root = QDir(qEnvironmentVariable("PBC_DOWNLOAD_DIR", QStringLiteral("./pbcloud-downloads"))).absolutePath();
#endif
    QDir().mkpath(download_root);
    device.setDownloadRoot(download_root);
#ifdef PLATFORM_POCKETBOOK
    pbc::PosixStorageInfo storage(true);   // the card is FAT, and may be reformatted: the 4 GiB limit applies whatever statfs says
#else
    pbc::PosixStorageInfo storage(false);
#endif
    if (store && !ca_file.empty()) {
        http = std::make_unique<pbc::CurlTransport>(ca_file);
        std::string reader_config;
        {
#ifdef PLATFORM_POCKETBOOK
            const QString cfg_path = QStringLiteral("/mnt/ext1/system/config/global.cfg");
#else
            const QString cfg_path = qEnvironmentVariable("PBC_READER_CONFIG");
#endif
            QFile f(cfg_path);
            if (!cfg_path.isEmpty() && f.open(QIODevice::ReadOnly)) reader_config = f.read(64 * 1024).toStdString();
        }
        const auto zone = std::make_shared<pbc::ReaderZone>(pbc::reader_zone(tzdb.get(), reader_config, pbc::read_zone_environment()));
        ConnectionBridge::Config cc;
        cc.store = store.get();
        cc.worker = &worker;
        cc.http = http.get();
        cc.clock = &clock;
        cc.format_date_time = [zone, &tzdb](qint64 t) { return QString::fromStdString(pbc::format_date_time(t, *zone, tzdb.get())); };
        cc.wifi_up = [&device] { return device.wifiConnected(); };
        // a tap that needs the network: ask the system to bring Wi-Fi up if it is not, then continue (never at start)
        cc.ensure_network = [&device](std::function<void(bool)> done) {
            if (device.wifiConnected()) {
                done(true);
                return;
            }
            auto link = std::make_shared<QMetaObject::Connection>();
            *link = QObject::connect(&device, &DeviceBridge::networkResult, &device, [link, done, &device](bool ok) {
                QObject::disconnect(*link);
                if (!ok) {
                    done(false);
                    return;
                }
                // the system reports "connected" before names and routes work: the first request right after it failed on the reader (device log
                // 2026-10-21), so the tap goes on after a short settling time
                QTimer::singleShot(3000, &device, [done, &device] { done(device.wifiConnected()); });
            });
            device.connectNetwork();
        };
        connectionBridge = std::make_unique<ConnectionBridge>(cc);

        // the file list: dates in the reader's own zone, read from its configuration (research R-14; the key is unverified, DT-NC-6)
        BrowseBridge::Config bc;
        bc.store = store.get();
        bc.worker = &worker;
        bc.http = http.get();
        bc.clock = &clock;
        bc.ensure_network = cc.ensure_network;
        bc.format_date = [zone, &tzdb](qint64 t) { return QString::fromStdString(pbc::format_date(t, *zone, tzdb.get())); };
        bc.download_root = download_root.toStdString();
        browseBridge = std::make_unique<BrowseBridge>(bc);

        TransferBridge::Config tc;
        tc.store = store.get();
        tc.worker = &worker;
        tc.http = http.get();
        tc.clock = &clock;
        tc.storage = &storage;
        tc.download_root = download_root.toStdString();
        tc.ensure_network = cc.ensure_network;
        tc.wifi_up = cc.wifi_up;
        tc.format_date_time = [zone, &tzdb](qint64 t) { return QString::fromStdString(pbc::format_date_time(t, *zone, tzdb.get())); };
        tc.open_file = [&device](const QString& path) { return device.openFile(path); };
        transferBridge = std::make_unique<TransferBridge>(tc);

        SpaceBridge::Config sc;
        sc.store = store.get();
        sc.worker = &worker;
        sc.storage = &storage;
        sc.download_root = download_root.toStdString();
        spaceBridge = std::make_unique<SpaceBridge>(sc);

        // Uploads and the queue (specs/002-manage-server-files): what may be picked is decided by the core against these rules - the reader's
        // storage root, and the app's own data folder (database, credentials, log) which can never be a source.
        pbc::SourceRules source_rules;
#ifdef PLATFORM_POCKETBOOK
        source_rules.root = "/mnt/ext1";
#else
        source_rules.root = QDir(qEnvironmentVariable("PBC_PICKER_ROOT", QStringLiteral("./pbcloud-picker"))).absolutePath().toStdString();
        QDir().mkpath(QString::fromStdString(source_rules.root));
#endif
        source_rules.deny = {data_dir.toStdString()};
        PickerBridge::Config pc;
        pc.store = store.get();
        pc.rules = source_rules;
        // the seven folders of the attach browser of PB Notes, and this app's own download folder (so that an edited copy can be sent back)
        pc.topFolders = {QStringLiteral("Audio Books"), QStringLiteral("Downloads"), QStringLiteral("Music"), QStringLiteral("My pictures"),
                         QStringLiteral("Notes"), QStringLiteral("Photo"), QStringLiteral("screens"), QStringLiteral("PBCloud")};
        pickerBridge = std::make_unique<PickerBridge>(pc);

        QueueBridge::Config qc;
        qc.store = store.get();
        qc.worker = &worker;
        qc.http = http.get();
        qc.clock = &clock;
        qc.rules = source_rules;
        qc.wifi_up = cc.wifi_up;
        qc.ensure_network = cc.ensure_network;
        qc.format_date_time = tc.format_date_time;
        qc.download_root = download_root.toStdString();
        queueBridge = std::make_unique<QueueBridge>(qc);

        FolderChooserBridge::Config fc;
        fc.store = store.get();
        fc.worker = &worker;
        fc.http = http.get();
        fc.clock = &clock;
        fc.wifi_up = cc.wifi_up;
        fc.ensure_network = cc.ensure_network;
        chooserBridge = std::make_unique<FolderChooserBridge>(fc);
    }

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
    if (browseBridge) engine.rootContext()->setContextProperty(QStringLiteral("browse"), browseBridge.get());
    if (transferBridge) engine.rootContext()->setContextProperty(QStringLiteral("transfer"), transferBridge.get());
    if (spaceBridge) engine.rootContext()->setContextProperty(QStringLiteral("space"), spaceBridge.get());
    if (pickerBridge) engine.rootContext()->setContextProperty(QStringLiteral("picker"), pickerBridge.get());
    if (queueBridge) engine.rootContext()->setContextProperty(QStringLiteral("queue"), queueBridge.get());
    if (chooserBridge) engine.rootContext()->setContextProperty(QStringLiteral("chooser"), chooserBridge.get());
    if (connectionBridge) {
        engine.rootContext()->setContextProperty(QStringLiteral("connection"), connectionBridge.get());
        engine.addImageProvider(QStringLiteral("qr"), new QrImageProvider(connectionBridge.get()));   // the engine owns it
    }
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::cerr << "[pbcloud] failed to load QML" << std::endl;
        return 1;
    }
    if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) device.setWindow(window);

    // The one network action at start: a single check that the login still works - only when Wi-Fi is already on, never in the self-test.
    if (connectionBridge && !self_test) connectionBridge->checkAtStart();

#ifndef PLATFORM_POCKETBOOK
    // The scripted walk of the host build (app/ui_script.h): taps the real pages and saves pictures of them. Not part of the reader's build.
    if (const QString walk = qEnvironmentVariable("PBC_UI_SCRIPT"); !walk.isEmpty()) {
        if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) ui_script::start(window, walk);
    }
#endif

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
            // the dialogs and widgets of the feature 002 screens (singletons and the main file are loaded by the engine itself)
            for (const QString& name : QDir(QStringLiteral(":/qml")).entryList(QStringList() << QStringLiteral("*.qml"))) {
                if (name == QLatin1String("Main.qml") || name == QLatin1String("Style.qml") || name == QLatin1String("Problems.qml") || name == QLatin1String("EntryWords.qml")) continue;
                QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qml/") + name));
                QObject* widget = component.isError() ? nullptr : component.create();
                if (!widget) {
                    std::cerr << "[pbcloud] component failed: " << name.toStdString() << " " << component.errorString().toStdString() << std::endl;
                    ++failed;
                }
                delete widget;
            }
            // the pages and the dialog that the queue, the picker and the chooser stand on must be in the package
            for (const char* required : {"pages/QueuePage.qml", "pages/FolderChooserPage.qml", "pages/FilePickerPage.qml", "CollisionDialog.qml"}) {
                if (!QFile::exists(QStringLiteral(":/qml/") + QLatin1String(required))) {
                    std::cerr << "[pbcloud] missing from the package: " << required << std::endl;
                    ++failed;
                }
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
