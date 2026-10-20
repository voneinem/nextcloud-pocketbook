#pragma once
// The file list (specs/001-nextcloud-files-client US2, contracts/bridge-qml.md): one folder of the user's Nextcloud shown from the
// store, page by page, sorted. Turning pages, sorting and going up are local. A request is sent only when a folder is opened that was
// never fetched, and when the user taps Refresh. Names from the server pass display_safe() before they reach QML.

#include <QObject>

#include <vector>
#include <QString>
#include <QVariantList>

#include <functional>

#include "pbc/http_transport.h"
#include "pbc/store.h"
#include "pbc/types.h"

class Worker;

class BrowseBridge : public QObject {
    Q_OBJECT
    // folder | favorites | search: what the rows are. The one page of rows serves all three.
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(bool moreResults READ moreResults NOTIFY changed)       // the server had more than it was asked to show; the list was cut
    Q_PROPERTY(QString searchTerm READ searchTerm NOTIFY changed)
    Q_PROPERTY(QString path READ path NOTIFY changed)
    // [{label, path}] from the root to the folder; the label of the root is ""
    Q_PROPERTY(QVariantList pathSegments READ pathSegments NOTIFY changed)
    Q_PROPERTY(bool canGoUp READ canGoUp NOTIFY changed)
    Q_PROPERTY(int page READ page NOTIFY changed)               // 0-based
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY changed)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)      // the entries of the shown page
    Q_PROPERTY(QString sortKey READ sortKey NOTIFY changed)     // name | date | size
    Q_PROPERTY(bool sortAscending READ sortAscending NOTIFY changed)
    Q_PROPERTY(bool hasListing READ hasListing NOTIFY changed)  // false: this folder was never fetched
    // fresh (fetched just now) | cached | not_synced (never confirmed) | gone (removed on the server)
    Q_PROPERTY(QString listingState READ listingState NOTIFY changed)
    // not_synced | gone | just_now | minutes | hours | days, and the number for minutes, hours and days
    Q_PROPERTY(QString listingAgeKind READ listingAgeKind NOTIFY changed)
    Q_PROPERTY(int listingAgeCount READ listingAgeCount NOTIFY changed)
    Q_PROPERTY(bool clockSuspect READ clockSuspect NOTIFY changed)
    Q_PROPERTY(bool offline READ offline NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY changed)
    Q_PROPERTY(bool folderKept READ folderKept NOTIFY changed)             // this folder is kept offline (itself or through a folder above)
    Q_PROPERTY(bool folderKeptDirect READ folderKeptDirect NOTIFY changed)  // ... by its own mark
    Q_PROPERTY(int skippedCount READ skippedCount NOTIFY changed)   // entries of the last answer that could not be used

public:
    struct Config {
        pbc::Store* store = nullptr;
        Worker* worker = nullptr;
        pbc::HttpTransport* http = nullptr;
        const pbc::Clock* clock = nullptr;
        // for a tap that needs the network: brings Wi-Fi up if needed, then calls done(online)
        std::function<void(std::function<void(bool)>)> ensure_network;
        // a modified time in the reader's zone, "YYYY-MM-DD"
        std::function<QString(qint64)> format_date;
        // where downloaded copies live: the state of a file ("downloaded", "outdated", ...) is read from the card
        std::string download_root;
    };

    explicit BrowseBridge(Config config, QObject* parent = nullptr);
    ~BrowseBridge() override;

    QString mode() const { return mode_; }
    bool moreResults() const { return more_; }
    QString searchTerm() const { return term_; }
    QString path() const { return path_; }
    QVariantList pathSegments() const;
    bool canGoUp() const { return path_ != QLatin1String("/"); }
    int page() const { return page_; }
    int pageCount() const { return page_count_; }
    int pageSize() const { return page_size_; }
    void setPageSize(int n);
    QVariantList rows() const { return rows_; }
    QString sortKey() const;
    bool sortAscending() const;
    bool hasListing() const { return has_listing_; }
    bool folderKept() const { return folder_kept_; }
    bool folderKeptDirect() const { return folder_kept_direct_; }
    QString listingState() const;
    QString listingAgeKind() const;
    int listingAgeCount() const;
    bool clockSuspect() const;
    bool offline() const { return offline_; }
    bool busy() const { return busy_; }
    QString errorKey() const { return error_; }
    int skippedCount() const { return skipped_; }

    // A folder: shows the cached listing, fetching it once if it was never fetched. A file: showDetails. An entry the user cannot use:
    // showReason (it does not navigate).
    Q_INVOKABLE void open(const QString& remotePath);
    Q_INVOKABLE void up();
    Q_INVOKABLE void refresh();   // the explicit refresh of the shown folder
    Q_INVOKABLE void refreshRows();   // re-reads the shown page from the store (after a download, a mark or an Update); no request
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();
    Q_INVOKABLE void goToPage(int page);
    Q_INVOKABLE void setSort(const QString& key, bool ascending);
    // The favorites: the stored list is shown; it is fetched only when it was never fetched (and Wi-Fi can be had), or on refresh().
    Q_INVOKABLE void showFavorites();
    // Local only: what the reader knows (cached listings, favorites, downloaded copies). Rows carry source = reader | favorites | download.
    Q_INVOKABLE void searchOffline(const QString& term);
    // The explicit "also search the server" tap: one request for the whole account. Rows carry source = server.
    Q_INVOKABLE void searchServer(const QString& term);
    // Back to the folder that was shown (the pages of favorites and search call it when they close); no request.
    Q_INVOKABLE void showFolder();

signals:
    void changed();
    void showDetails(const QString& remotePath);
    void showReason(const QString& reasonKey);

private:
    struct FlatRow {
        pbc::RemoteEntry entry;
        std::string source;
    };
    void fetch(bool always);
    void fetch_favorites();
    void reload();   // reads the shown page from the store (no request)
    void reload_flat();
    QVariantMap row_map(const pbc::RemoteEntry& e, const std::string& source) const;

    Config c_;
    QString mode_ = QStringLiteral("folder");
    QString term_;
    bool more_ = false;
    std::vector<FlatRow> flat_;   // search results; the favorites are read from the store on every reload
    QString path_ = QStringLiteral("/");
    int page_ = 0;
    int page_count_ = 1;
    int page_size_ = 12;
    QVariantList rows_;
    bool folder_kept_ = false, folder_kept_direct_ = false;
    bool has_listing_ = false;
    bool fresh_ = false;
    bool offline_ = false;
    bool busy_ = false;
    QString error_;
    int skipped_ = 0;
    std::optional<int64_t> fetched_at_;
    bool gone_ = false;
};
