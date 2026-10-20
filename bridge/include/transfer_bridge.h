#pragma once
// Downloads and what the page shows about a file (specs/001-nextcloud-files-client US3, contracts/bridge-qml.md): the details of a file,
// the checks before a download, one download at a time with coarse progress, cancel, resume after a restart, the choices for a copy the
// user changed, and the one-tap open in the reader's viewer. The work runs on the Worker; nothing starts by itself.

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

#include "pbc/downloader.h"
#include "pbc/http_transport.h"
#include "pbc/storage_info.h"
#include "pbc/store.h"
#include "pbc/types.h"
#include "pbc/update.h"

class Worker;

class TransferBridge : public QObject {
    Q_OBJECT
    // idle | running | waiting (the connection dropped) | paused | done | cancelled | failed | blocked (a changed copy is in the way)
    Q_PROPERTY(QString jobState READ jobState NOTIFY changed)
    Q_PROPERTY(bool jobBusy READ jobBusy NOTIFY changed)         // running or waiting
    Q_PROPERTY(QString jobPath READ jobPath NOTIFY changed)      // the remote path of the current or last job
    Q_PROPERTY(QString jobDoneText READ jobDoneText NOTIFY changed)
    Q_PROPERTY(QString jobTotalText READ jobTotalText NOTIFY changed)
    Q_PROPERTY(int jobPercent READ jobPercent NOTIFY changed)
    Q_PROPERTY(QString jobError READ jobError NOTIFY changed)    // the cause key of a failed, paused or blocked job
    Q_PROPERTY(int jobAttempt READ jobAttempt NOTIFY changed)
    Q_PROPERTY(QVariantList pausedDownloads READ pausedDownloads NOTIFY changed)   // [{id, remotePath, name}]
    // Update (US4): started only by a tap; one at a time and never together with a download
    Q_PROPERTY(bool updateRunning READ updateRunning NOTIFY changed)
    Q_PROPERTY(QString updateStage READ updateStage NOTIFY changed)   // idle | checking | confirm | downloading | done
    Q_PROPERTY(int updateDone READ updateDone NOTIFY changed)         // files fetched so far in this run
    Q_PROPERTY(int updateTotal READ updateTotal NOTIFY changed)       // files the run plans to fetch
    Q_PROPERTY(QVariantMap updateReport READ updateReport NOTIFY changed)

public:
    struct Config {
        pbc::Store* store = nullptr;
        Worker* worker = nullptr;
        pbc::HttpTransport* http = nullptr;
        const pbc::Clock* clock = nullptr;
        pbc::StorageInfo* storage = nullptr;
        std::string download_root;
        std::function<void(std::function<void(bool)>)> ensure_network;   // for a tap that needs the network
        std::function<bool()> wifi_up;                                   // asked while a dropped connection is waited for
        std::function<QString(qint64)> format_date_time;
        std::function<bool(const QString&)> open_file;                   // hands an absolute path to the reader's viewer
        pbc::DownloaderOptions options;
    };

    explicit TransferBridge(Config config, QObject* parent = nullptr);
    ~TransferBridge() override;

    QString jobState() const { return state_; }
    bool jobBusy() const { return busy_; }
    QString jobPath() const { return path_; }
    QString jobDoneText() const;
    QString jobTotalText() const;
    int jobPercent() const;
    QString jobError() const { return error_; }
    int jobAttempt() const { return attempt_; }
    QVariantList pausedDownloads() const;
    bool updateRunning() const { return update_busy_; }
    QString updateStage() const { return update_stage_; }
    int updateDone() const { return update_done_; }
    int updateTotal() const { return update_total_; }
    // {folders, files, added, updated, skipped, needDecision, removed, failed, uncheckedFolders, stoppedBy, decisionPaths, removedPaths}
    QVariantMap updateReport() const { return update_report_; }

    // {name, kind, sizeText, mime, dateText, onReader, serverNewer, available, reason, canOpen, localPath, remotePath}
    Q_INVOKABLE QVariantMap details(const QString& remotePath) const;
    // {verdict: ok | confirm | refused, errorKey, sizeText, freeText, neededText}
    Q_INVOKABLE QVariantMap preflight(const QString& remotePath) const;
    Q_INVOKABLE void download(const QString& remotePath, bool confirmed);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void resume(qlonglong jobId);
    // choice: keep_mine | keep_both | replace (the page has asked the second confirmation before it passes replace)
    Q_INVOKABLE void resolveConflict(const QString& remotePath, const QString& choice);
    Q_INVOKABLE void open(const QString& remotePath);
    // Keep offline: marks the file or folder (kind "file" | "folder"); nothing is fetched by marking.
    Q_INVOKABLE void setKept(const QString& remotePath, const QString& kind, bool on);
    // Checks what is kept against the server and downloads what changed. Ignored while a download or an Update runs.
    Q_INVOKABLE void update();
    Q_INVOKABLE void cancelUpdate();
    // The answer to updateNeedsConfirm.
    Q_INVOKABLE void confirmUpdate(bool yes);

signals:
    void changed();
    void jobFinished(const QString& state);
    void updateNeedsConfirm(const QString& bytesText, const QString& freeText);
    void updateFinished();

private:
    struct FlagCancel : pbc::Cancel {
        std::atomic<bool> flag{false};
        bool requested() const override { return flag.load(); }
    };
    void start(const QString& remotePath, pbc::ConflictPolicy policy);
    void run_job(std::function<pbc::DownloadOutcome(pbc::Downloader&, pbc::Cancel&, const std::function<void(const pbc::JobProgress&)>&)> work);
    void fail_now(const QString& key);

    void finish_update(const pbc::UpdateReport& rep);

    Config c_;
    FlagCancel cancel_;
    FlagCancel update_cancel_;
    bool update_busy_ = false;
    QString update_stage_ = QStringLiteral("idle");
    int update_done_ = 0, update_total_ = 0;
    QVariantMap update_report_;
    std::mutex confirm_mu_;
    std::condition_variable confirm_cv_;
    int confirm_answer_ = 0;   // 0 = waiting, 1 = yes, 2 = no
    QString state_ = QStringLiteral("idle");
    bool busy_ = false;
    QString path_;
    QString error_;
    int attempt_ = 0;
    qint64 done_ = 0, total_ = 0;
};
