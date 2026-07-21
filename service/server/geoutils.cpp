#include "geoutils.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QtEndian>

// Minimal V2Ray/Xray geoip.dat (GeoIPList) protobuf wire-format reader. Only the
// fields needed to turn a country code into a CIDR list are decoded; everything else
// is skipped by wire type.
//   message CIDR      { bytes ip = 1; uint32 prefix = 2; }
//   message GeoIP     { string country_code = 1; repeated CIDR cidr = 2; ... }
//   message GeoIPList { repeated GeoIP entry = 1; }

namespace
{
    bool readVarint(const char *data, qsizetype size, qsizetype &pos, quint64 &out)
    {
        out = 0;
        int shift = 0;
        while (pos < size) {
            const quint8 b = static_cast<quint8>(data[pos++]);
            out |= (static_cast<quint64>(b & 0x7F) << shift);
            if (!(b & 0x80)) {
                return true;
            }
            shift += 7;
            if (shift > 63) {
                return false;
            }
        }
        return false;
    }

    bool skipField(const char *data, qsizetype size, qsizetype &pos, int wireType)
    {
        switch (wireType) {
        case 0: {
            quint64 v;
            return readVarint(data, size, pos, v);
        }
        case 1:
            pos += 8;
            return pos <= size;
        case 5:
            pos += 4;
            return pos <= size;
        case 2: {
            quint64 len;
            if (!readVarint(data, size, pos, len)) {
                return false;
            }
            pos += static_cast<qsizetype>(len);
            return pos <= size;
        }
        default:
            return false;
        }
    }

    QString cidrToString(const QByteArray &ip, quint64 prefix)
    {
        if (ip.size() == 4) {
            const QHostAddress addr(qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(ip.constData())));
            return addr.toString() + QStringLiteral("/") + QString::number(prefix);
        }
        if (ip.size() == 16) {
            Q_IPV6ADDR raw;
            memcpy(raw.c, ip.constData(), 16);
            const QHostAddress addr(raw);
            return addr.toString() + QStringLiteral("/") + QString::number(prefix);
        }
        return {};
    }

    // Parse one GeoIP message body. If its country_code matches wantCode, collect its
    // CIDRs into out (unless onlyCount). limit >= 0 caps the entry: once the count
    // exceeds it, collection stops early and overLimit is set.
    void parseEntry(const char *data, qsizetype size, const QString &wantCode, bool onlyCount,
                    QStringList &out, int &count, int limit, bool &matched, bool &overLimit)
    {
        // overLimit is per-entry: reset it so an earlier oversized (non-matching) entry
        // in the same scan does not suppress the matching one.
        overLimit = false;
        qsizetype pos = 0;
        QString code;
        struct Cidr
        {
            QByteArray ip;
            quint64 prefix = 0;
        };
        QVector<Cidr> cidrs;
        int collected = 0;

        while (pos < size) {
            quint64 tag;
            if (!readVarint(data, size, pos, tag)) {
                return;
            }
            const int field = static_cast<int>(tag >> 3);
            const int wire = static_cast<int>(tag & 0x7);

            if (field == 1 && wire == 2) {
                quint64 len;
                if (!readVarint(data, size, pos, len)) {
                    return;
                }
                code = QString::fromLatin1(data + pos, static_cast<int>(len));
                pos += static_cast<qsizetype>(len);
            } else if (field == 2 && wire == 2) {
                quint64 len;
                if (!readVarint(data, size, pos, len)) {
                    return;
                }
                const qsizetype cidrEnd = pos + static_cast<qsizetype>(len);
                Cidr c;
                while (pos < cidrEnd) {
                    quint64 ctag;
                    if (!readVarint(data, size, pos, ctag)) {
                        return;
                    }
                    const int cfield = static_cast<int>(ctag >> 3);
                    const int cwire = static_cast<int>(ctag & 0x7);
                    if (cfield == 1 && cwire == 2) {
                        quint64 iplen;
                        if (!readVarint(data, size, pos, iplen)) {
                            return;
                        }
                        c.ip = QByteArray(data + pos, static_cast<int>(iplen));
                        pos += static_cast<qsizetype>(iplen);
                    } else if (cfield == 2 && cwire == 0) {
                        if (!readVarint(data, size, pos, c.prefix)) {
                            return;
                        }
                    } else {
                        if (!skipField(data, size, pos, cwire)) {
                            return;
                        }
                    }
                }
                ++collected;
                if (limit >= 0 && collected > limit) {
                    overLimit = true;
                    if (onlyCount) {
                        // Keep scanning to report the real size.
                    } else {
                        // Stop building; we will discard this category anyway.
                        continue;
                    }
                }
                if (!onlyCount && !overLimit) {
                    cidrs.append(c);
                }
            } else {
                if (!skipField(data, size, pos, wire)) {
                    return;
                }
            }
        }

        if (code.compare(wantCode, Qt::CaseInsensitive) != 0) {
            return;
        }
        matched = true;
        count = collected;
        if (onlyCount || overLimit) {
            return;
        }
        for (const Cidr &c : cidrs) {
            const QString s = cidrToString(c.ip, c.prefix);
            if (!s.isEmpty()) {
                out.append(s);
            }
        }
    }

    bool parseGeoip(const QByteArray &blob, const QString &code, bool onlyCount, QStringList &out,
                    int &count, int limit, bool &overLimit)
    {
        const char *data = blob.constData();
        const qsizetype size = blob.size();
        qsizetype pos = 0;
        bool matched = false;
        while (pos < size) {
            quint64 tag;
            if (!readVarint(data, size, pos, tag)) {
                break;
            }
            const int field = static_cast<int>(tag >> 3);
            const int wire = static_cast<int>(tag & 0x7);
            if (field == 1 && wire == 2) {
                quint64 len;
                if (!readVarint(data, size, pos, len)) {
                    break;
                }
                parseEntry(data + pos, static_cast<qsizetype>(len), code, onlyCount, out, count,
                           limit, matched, overLimit);
                pos += static_cast<qsizetype>(len);
                if (matched) {
                    return true;
                }
            } else {
                if (!skipField(data, size, pos, wire)) {
                    break;
                }
            }
        }
        return matched;
    }

    QByteArray loadGeoipBlob()
    {
        static QByteArray cached;
        static bool loaded = false;
        if (loaded) {
            return cached;
        }
        loaded = true;
        const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/geoip.dat");
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            cached = file.readAll();
        }
        return cached;
    }

    QString geoipCode(const QString &token)
    {
        if (!token.startsWith(QLatin1String("geoip:"), Qt::CaseInsensitive)) {
            return {};
        }
        QString code = token.mid(6).trimmed();
        if (code.isEmpty() || code.startsWith(QLatin1Char('!'))) {
            // Empty or inverse ("everything except") cannot be a finite route list.
            return {};
        }
        return code;
    }
}

namespace amnezia
{
    namespace geoutils
    {
        QStringList expandGeoipRules(const QStringList &tokens, int maxCidrsPerRule)
        {
            QStringList result;
            QByteArray blob;
            bool blobLoaded = false;

            for (const QString &token : tokens) {
                const QString code = geoipCode(token);
                if (code.isEmpty()) {
                    continue;
                }
                if (!blobLoaded) {
                    blob = loadGeoipBlob();
                    blobLoaded = true;
                }
                if (blob.isEmpty()) {
                    continue;
                }
                QStringList cidrs;
                int count = 0;
                bool overLimit = false;
                const bool ok = parseGeoip(blob, code, false, cidrs, count, maxCidrsPerRule, overLimit);
                if (ok && !overLimit) {
                    result += cidrs;
                }
            }
            result.removeDuplicates();
            return result;
        }

        int geoipCategorySize(const QString &code)
        {
            const QString normalized = geoipCode(code.startsWith(QLatin1String("geoip:"), Qt::CaseInsensitive)
                                                         ? code
                                                         : QStringLiteral("geoip:") + code);
            if (normalized.isEmpty()) {
                return -1;
            }
            const QByteArray blob = loadGeoipBlob();
            if (blob.isEmpty()) {
                return -1;
            }
            QStringList unused;
            int count = 0;
            bool overLimit = false;
            if (!parseGeoip(blob, normalized, true, unused, count, -1, overLimit)) {
                return -1;
            }
            return count;
        }
    }
}
