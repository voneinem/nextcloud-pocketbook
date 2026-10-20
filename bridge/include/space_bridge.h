#pragma once
// What the app put on the reader and how much room it takes (specs/001-nextcloud-files-client US6, contracts/bridge-qml.md): the
// downloaded copies page by page, the used and the free space, the size of the metadata cache, the removal of a copy and the clearing
// of the cache. Nothing here ever contacts the server.

#include <QObject>
#include <QString>
#include <QVariantList>

#include <string>

#include "pbc/storage_info.h"
#include "pbc/store.h"

class Worker;

class SpaceBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)   // one page: [{name, remotePath, sizeText, state, changedLocally}]
    Q_PROPERTY(int page READ page NOTIFY changed)               // 0-based
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY changed)   // 20 until the page sets it from the height it has
    Q_PROPERTY(int total READ total NOTIFY changed)             // all downloaded copies
    Q_PROPERTY(QString usedText READ usedText NOTIFY changed)   // the copies together
    Q_PROPERTY(QString freeText READ freeText NOTIFY changed)   // free on the card ("" = not known)
    Q_PROPERTY(QString cacheText READ cacheText NOTIFY changed) // the database file: lists, marks, favorites and the records of the copies
    Q_PROPERTY(bool busy READ busy NOTIFY changed)

public:
    static constexpr int kDefaultPageSize = 20;

    struct Config {
        pbc::Store* store = nullptr;
        Worker* worker = nullptr;
        pbc::StorageInfo* storage = nullptr;
        std::string download_root;
    };

    explicit SpaceBridge(Config config, QObject* parent = nullptr);
    ~SpaceBridge() override;

    QVariantList items() const { return items_; }
    int page() const { return page_; }
    int pageCount() const { return page_count_; }
    int pageSize() const { return page_size_; }
    void setPageSize(int n);
    int total() const { return total_; }
    QString usedText() const { return used_; }
    QString freeText() const { return free_; }
    QString cacheText() const { return cache_; }
    bool busy() const { return busy_; }

    Q_INVOKABLE void refresh();   // reads everything again (the page opens, the card changed)
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();
    // Deletes the copy on the reader and its record. confirmedLoss must be true for a copy that was changed on the reader.
    Q_INVOKABLE void remove(const QString& remotePath, bool confirmedLoss);
    // Drops the cached lists (not the downloads, the marks, the favorites).
    Q_INVOKABLE void clearCache();

signals:
    void changed();
    void removeFinished(bool ok, const QString& errorKey);
    void cacheCleared(bool ok);

private:
    void reload();

    Config c_;
    QVariantList items_;
    int page_ = 0, page_count_ = 1, total_ = 0, page_size_ = kDefaultPageSize;
    QString used_, free_, cache_;
    bool busy_ = false;
};
