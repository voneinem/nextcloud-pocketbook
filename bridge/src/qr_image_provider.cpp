#include "qr_image_provider.h"

#include <QImage>

#include "connection_bridge.h"
#include "pbc/qr_code.h"

QrImageProvider::QrImageProvider(const ConnectionBridge* connection) : QQuickImageProvider(QQuickImageProvider::Image), connection_(connection) {}

QImage QrImageProvider::requestImage(const QString&, QSize* size, const QSize&) {
    const std::string link = connection_->loginLinkSnapshot();
    if (link.empty()) return QImage();
    const auto qr = pbc::qr_encode(link);
    if (!qr.ok()) return QImage();
    constexpr int kQuiet = 4;
    const int modules = qr.value.size + 2 * kQuiet;
    const int scale = std::max(4, 640 / modules);   // whole pixels per module: sharp edges, no smoothing
    QImage image(modules * scale, modules * scale, QImage::Format_Grayscale8);
    image.fill(255);
    for (int y = 0; y < qr.value.size; ++y)
        for (int x = 0; x < qr.value.size; ++x)
            if (qr.value.at(x, y))
                for (int dy = 0; dy < scale; ++dy) {
                    uchar* line = image.scanLine((y + kQuiet) * scale + dy);
                    for (int dx = 0; dx < scale; ++dx) line[(x + kQuiet) * scale + dx] = 0;
                }
    if (size) *size = image.size();
    return image;
}
