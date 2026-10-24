#pragma once
// The folder chooser of Move and Copy (specs/002-manage-server-files US5, contracts/bridge-qml.md): folders of the user's Nextcloud, page by page,
// from the stored listings. A folder that was never fetched is fetched when the user opens it and the reader is online (one request, like the Files
// page); offline it shows nothing but says so. Folders that are still queued (a new folder) are listed too, so that a move can target them. The
// chooser never writes anything; the choice goes back to the page that asked.

#include <QObject>
#include <QString>
#include <QVariantList>

#include <functional>
#include <string>
#include <vector>

#include "pbc/http_transport.h"
#include "pbc/store.h"
#include "pbc/types.h"

class Worker;

class FolderChooserBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path NOTIFY changed)
    Q_PROPERTY(QVariantList pathSegments READ pathSegments NOTIFY changed)   // [{label, path}], the first is the root
    Q_PROPERTY(bool canGoUp READ canGoUp NOTIFY changed)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)                   // [{name, remotePath, pending}] of the shown page: folders only
    Q_PROPERTY(int page READ page NOTIFY changed)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY changed)
    Q_PROPERTY(bool hasListing READ hasListing NOTIFY changed)               // this folder has a stored listing (or only queued folders)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // true: only folders the server really has are listed (a restore from the trash, specs/006-trash-and-versions): no folder that is merely queued, no New folder
    Q_PROPERTY(bool serverOnly READ serverOnly WRITE setServerOnly NOTIFY changed)
    Q_PROPERTY(bool offline READ offline NOTIFY changed)
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY changed)

public:
    struct Config {
        pbc::Store* store = nullptr;
        Worker* worker = nullptr;
        pbc::HttpTransport* http = nullptr;
        const pbc::Clock* clock = nullptr;
        std::function<bool()> wifi_up;   // empty = online
        std::function<void(std::function<void(bool)>)> ensure_network;
    };

    explicit FolderChooserBridge(Config config, QObject* parent = nullptr);
    ~FolderChooserBridge() override;

    QString path() const { return QString::fromStdString(path_); }
    QVariantList pathSegments() const;
    bool canGoUp() const { return path_ != "/"; }
    QVariantList rows() const { return rows_; }
    int page() const { return page_; }
    int pageCount() const { return page_count_; }
    int pageSize() const { return page_size_; }
    bool serverOnly() const { return server_only_; }
    void setServerOnly(bool on);
    void setPageSize(int n);
    bool hasListing() const { return has_listing_; }
    bool busy() const { return busy_; }
    bool offline() const { return c_.wifi_up ? !c_.wifi_up() : false; }
    QString errorKey() const { return error_; }

    // Shows a folder (the root for ""): the stored folders at once, a fetch when it was never fetched and the reader is online.
    Q_INVOKABLE void open(const QString& remotePath);
    Q_INVOKABLE void up();
    Q_INVOKABLE void refresh();   // re-reads the stored rows (after a New folder); no request
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();

signals:
    void changed();

private:
    void reload();
    void fetch_if_missing();

    Config c_;
    std::string path_ = "/";
    std::vector<QVariantMap> all_;
    QVariantList rows_;
    int page_ = 0, page_count_ = 1, page_size_ = 10;
    bool has_listing_ = false, busy_ = false, server_only_ = false;
    QString error_;
};
