#include "settings_bridge.h"

#include <QVariantMap>

SettingsBridge::SettingsBridge(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void SettingsBridge::refresh() {
    error_.clear();
    emit changed();
}

QString SettingsBridge::language() const {
    const std::string v = config_.store->setting("ui_language");
    return v == "en" || v == "de" ? QString::fromStdString(v) : QStringLiteral("auto");
}

QVariantList SettingsBridge::languageChoices() const {
    QVariantList out;
    for (const auto& c : {std::pair<const char*, const char*>{"auto", "Automatic / Automatisch"}, {"en", "English"}, {"de", "Deutsch"}}) {
        QVariantMap m;
        m["value"] = QString::fromLatin1(c.first);
        m["label"] = QString::fromUtf8(c.second);
        out.append(m);
    }
    return out;
}

bool SettingsBridge::setLanguage(const QString& value) {
    if (value != QLatin1String("auto") && value != QLatin1String("en") && value != QLatin1String("de")) return false;
    if (!config_.store->set_setting("ui_language", value.toStdString()).ok()) {
        error_ = QStringLiteral("write_failed");
        emit changed();
        return false;
    }
    error_.clear();
    emit languageChanged();
    emit changed();
    return true;
}
