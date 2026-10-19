#pragma once
// The options of the app (contracts/bridge-qml.md SettingsBridge): the language of the interface and the state of the user's own
// certificate authority. The About page has its own bridge. A refused value changes nothing and names the cause in errorKey.

#include <QObject>
#include <QString>
#include <QVariantList>

#include "pbc/store.h"

class SettingsBridge : public QObject {
    Q_OBJECT
    // "" or the cause of the last refused value: write_failed
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY changed)
    // the language of the interface: "auto" (the reader's language) | "en" | "de"
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(QVariantList languageChoices READ languageChoices CONSTANT)
    // the user's own certificate authority (custom-ca.pem in the data folder): "none" | "loaded" | "invalid" (the file was not used), and how many
    // certificates it contributed; verification stays on in every case
    Q_PROPERTY(QString customCaStatus READ customCaStatus CONSTANT)
    Q_PROPERTY(int customCaCount READ customCaCount CONSTANT)

public:
    struct Config {
        pbc::Store* store = nullptr;
        QString custom_ca_status = QStringLiteral("none");
        int custom_ca_count = 0;
    };

    explicit SettingsBridge(Config config, QObject* parent = nullptr);

    QString errorKey() const { return error_; }
    QString language() const;
    QVariantList languageChoices() const;   // [{value, label}]; the labels are in their own language and are not translated
    Q_INVOKABLE bool setLanguage(const QString& value);   // false for a language that is not offered
    QString customCaStatus() const { return config_.custom_ca_status; }
    int customCaCount() const { return config_.custom_ca_count; }
    Q_INVOKABLE void refresh();

signals:
    void changed();
    void languageChanged();

private:
    Config config_;
    QString error_;
};
