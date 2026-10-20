#pragma once
// image://qr/<n>: the QR code of the running browser login, drawn black on white with a quiet zone of four modules and a whole number of
// pixels per module (specs/001-nextcloud-files-client, research R-5). One image is one screen update on the e-ink panel.

#include <QQuickImageProvider>

class ConnectionBridge;

class QrImageProvider : public QQuickImageProvider {
public:
    explicit QrImageProvider(const ConnectionBridge* connection);
    // An empty image when there is no login running or the link does not fit in a QR code (the page then shows only the text).
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    const ConnectionBridge* connection_;
};
