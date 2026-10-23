#include "format_util.h"

#include <QCoreApplication>

QString human_size(qint64 bytes) {
    if (bytes < 0) return QString();
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    static const char* const units[] = {"KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes) / 1024.0;
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QLatin1String(units[u]));
}

QString display_safe(const std::string& name, int max_chars) {
    QString s = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));   // invalid UTF-8 becomes U+FFFD
    QString out;
    out.reserve(s.size());
    for (QChar c : s) {
        const ushort u = c.unicode();
        if (u < 0x20 || u == 0x7f || (u >= 0x80 && u < 0xa0)) continue;     // control characters
        if ((u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069)) continue;   // bidirectional embedding and override
        if (u == 0x200e || u == 0x200f || u == 0x061c) continue;                        // direction marks
        out.append(c);
    }
    if (out.size() > max_chars) {
        out.truncate(max_chars);
        out.append(QStringLiteral("..."));
    }
    return out;
}

QString bridge_tr(const char* english_source) { return QCoreApplication::translate("bridge", english_source); }
