#pragma once
// The reader-side file browser of the Upload action (specs/002-manage-server-files US1, contracts/bridge-qml.md, research R-13): which folders and
// files of the reader's storage can be picked, in a list or as tiles, remembered. Adapted from the attach browser of notes-pocketbook (MIT,
// same author; see NOTICE). It reads no file content and sends no request; what may be picked is decided by the core (source_walk.h,
// upload_source.h): never a link, never the app's own data, never an empty file or one the storage cannot hold.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <string>
#include <vector>

#include "pbc/source_walk.h"
#include "pbc/store.h"

class PickerBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString rootDirectory READ rootDirectory CONSTANT)
    Q_PROPERTY(QStringList topFolders READ topFolders CONSTANT)
    Q_PROPERTY(QString view READ view WRITE setView NOTIFY viewChanged)   // list | tile, kept in the setting picker_view
    Q_PROPERTY(int fileCount READ fileCount NOTIFY selectionChanged)       // picked files, and the files inside picked folders
    Q_PROPERTY(int folderCount READ folderCount NOTIFY selectionChanged)   // picked folders and the folders inside them
    Q_PROPERTY(qlonglong byteCount READ byteCount NOTIFY selectionChanged)
    Q_PROPERTY(QString byteText READ byteText NOTIFY selectionChanged)
    Q_PROPERTY(int maxItems READ maxItems NOTIFY selectionChanged)         // how many more items the queue takes
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)

public:
    struct Config {
        pbc::Store* store = nullptr;
        pbc::SourceRules rules;
        QStringList topFolders;   // the folders listed at the root; empty = the root is listed unfiltered
    };

    explicit PickerBridge(Config config, QObject* parent = nullptr);

    QString rootDirectory() const { return QString::fromStdString(c_.rules.root); }
    QStringList topFolders() const { return c_.topFolders; }
    QString view() const;
    void setView(const QString& view);
    int fileCount() const;
    int folderCount() const;
    qlonglong byteCount() const;
    QString byteText() const;
    int maxItems() const;
    bool hasSelection() const { return !sel_.empty(); }

    // [{name, path, dir, size, sizeText, selected}] of one folder; empty for a path that is not readable or not allowed
    Q_INVOKABLE QVariantList listDirectory(const QString& path);
    // Selects or unselects a file. "" on success, else the key why it cannot be picked (source_unreadable, source_not_allowed,
    // too_large_for_storage, name_forbidden, queue_full).
    Q_INVOKABLE QString toggle(const QString& path);
    // Selects a folder with everything inside it (a bounded walk), or unselects it. {ok, folders, files, bytes, bytesText, overLimit, errorKey}
    Q_INVOKABLE QVariantMap addFolder(const QString& path);
    Q_INVOKABLE void clear();
    Q_INVOKABLE QStringList selectedFiles() const;
    Q_INVOKABLE QStringList selectedFolders() const;

signals:
    void viewChanged();
    void selectionChanged();

private:
    struct Pick {
        std::string path;
        bool folder = false;
        int files = 0, folders = 0;
        int64_t bytes = 0;
    };
    int items_in_selection() const;
    std::vector<Pick>::iterator find(const std::string& path);

    Config c_;
    std::vector<Pick> sel_;
};
