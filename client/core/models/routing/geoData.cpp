#include "geoData.h"

#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QSet>

#include "ipRanges.h"

namespace amnezia
{
    namespace routing
    {
        namespace
        {
            // Minimal protobuf wire format reader.
            class PbReader
            {
            public:
                PbReader(const char *data, qsizetype size) : m_p(reinterpret_cast<const uchar *>(data)), m_end(m_p + size)
                {
                }

                bool atEnd() const { return m_p >= m_end || !m_ok; }
                bool ok() const { return m_ok; }

                quint64 varint()
                {
                    quint64 result = 0;
                    int shift = 0;
                    while (m_p < m_end && shift < 64) {
                        const uchar c = *m_p++;
                        result |= quint64(c & 0x7f) << shift;
                        if (!(c & 0x80)) {
                            return result;
                        }
                        shift += 7;
                    }
                    m_ok = false;
                    return 0;
                }

                bool next(int &field, int &wire)
                {
                    if (atEnd()) {
                        return false;
                    }
                    const quint64 key = varint();
                    if (!m_ok) {
                        return false;
                    }
                    field = int(key >> 3);
                    wire = int(key & 7);
                    return true;
                }

                // Length delimited field; returns a pointer/size pair.
                bool bytes(const char *&data, qsizetype &size)
                {
                    const quint64 len = varint();
                    if (!m_ok || len > quint64(m_end - m_p)) {
                        m_ok = false;
                        return false;
                    }
                    data = reinterpret_cast<const char *>(m_p);
                    size = qsizetype(len);
                    m_p += len;
                    return true;
                }

                void skip(int wire)
                {
                    switch (wire) {
                    case 0: varint(); break;
                    case 1: advance(8); break;
                    case 2: {
                        const char *d;
                        qsizetype s;
                        bytes(d, s);
                        break;
                    }
                    case 5: advance(4); break;
                    default: m_ok = false; break;
                    }
                }

            private:
                void advance(qsizetype n)
                {
                    if (n > m_end - m_p) {
                        m_ok = false;
                        return;
                    }
                    m_p += n;
                }

                const uchar *m_p;
                const uchar *m_end;
                bool m_ok = true;
            };

            bool readFile(const QString &path, QByteArray &data, QString *error)
            {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) {
                    if (error) {
                        *error = QObject::tr("Cannot open geo file %1").arg(path);
                    }
                    return false;
                }
                data = file.readAll();
                return true;
            }

            QString readCode(const char *data, qsizetype size)
            {
                PbReader r(data, size);
                int field, wire;
                while (r.next(field, wire)) {
                    if (field == 1 && wire == 2) {
                        const char *d;
                        qsizetype s;
                        if (!r.bytes(d, s)) {
                            return QString();
                        }
                        return QString::fromUtf8(d, s).toLower();
                    }
                    r.skip(wire);
                }
                return QString();
            }

            struct TagFilter
            {
                QStringList required;
                QStringList excluded;
                bool all = false;
            };
        } // namespace

        QStringList GeoData::listTags(const QString &path, QString *error)
        {
            QByteArray data;
            if (!readFile(path, data, error)) {
                return {};
            }
            QStringList tags;
            PbReader r(data.constData(), data.size());
            int field, wire;
            while (r.next(field, wire)) {
                if (field == 1 && wire == 2) {
                    const char *d;
                    qsizetype s;
                    if (!r.bytes(d, s)) {
                        break;
                    }
                    const QString code = readCode(d, s);
                    if (!code.isEmpty()) {
                        tags.append(code);
                    }
                } else {
                    r.skip(wire);
                }
            }
            if (!r.ok() && error) {
                *error = QObject::tr("Geo file %1 is corrupted").arg(path);
            }
            tags.sort();
            tags.removeDuplicates();
            return tags;
        }

        QStringList GeoData::expandSites(const QString &path, const QStringList &tags, QStringList *missingTags, QString *error)
        {
            QHash<QString, TagFilter> filters;
            for (const QString &rawTag : tags) {
                QStringList parts = rawTag.trimmed().toLower().split(QLatin1Char('@'));
                const QString code = parts.takeFirst();
                if (code.isEmpty()) {
                    continue;
                }
                TagFilter &filter = filters[code];
                if (parts.isEmpty()) {
                    filter.all = true;
                }
                for (const QString &attr : parts) {
                    if (attr.startsWith(QLatin1Char('!'))) {
                        filter.excluded.append(attr.mid(1));
                    } else if (!attr.isEmpty()) {
                        filter.required.append(attr);
                    }
                }
            }
            if (filters.isEmpty()) {
                return {};
            }

            QByteArray data;
            if (!readFile(path, data, error)) {
                if (missingTags) {
                    *missingTags = filters.keys();
                }
                return {};
            }

            QStringList result;
            QSet<QString> seen;
            QSet<QString> found;
            PbReader top(data.constData(), data.size());
            int field, wire;
            while (top.next(field, wire)) {
                if (field != 1 || wire != 2) {
                    top.skip(wire);
                    continue;
                }
                const char *entryData;
                qsizetype entrySize;
                if (!top.bytes(entryData, entrySize)) {
                    break;
                }
                const QString code = readCode(entryData, entrySize);
                auto filterIt = filters.constFind(code);
                if (filterIt == filters.constEnd()) {
                    continue;
                }
                found.insert(code);
                const TagFilter &filter = filterIt.value();

                PbReader entry(entryData, entrySize);
                int f, w;
                while (entry.next(f, w)) {
                    if (f != 2 || w != 2) {
                        entry.skip(w);
                        continue;
                    }
                    const char *domData;
                    qsizetype domSize;
                    if (!entry.bytes(domData, domSize)) {
                        break;
                    }
                    int type = 0;
                    QString value;
                    QStringList attributes;
                    PbReader dom(domData, domSize);
                    int df, dw;
                    while (dom.next(df, dw)) {
                        if (df == 1 && dw == 0) {
                            type = int(dom.varint());
                        } else if (df == 2 && dw == 2) {
                            const char *d;
                            qsizetype s;
                            if (dom.bytes(d, s)) {
                                value = QString::fromUtf8(d, s);
                            }
                        } else if (df == 3 && dw == 2) {
                            const char *d;
                            qsizetype s;
                            if (dom.bytes(d, s)) {
                                PbReader attr(d, s);
                                int af, aw;
                                while (attr.next(af, aw)) {
                                    if (af == 1 && aw == 2) {
                                        const char *kd;
                                        qsizetype ks;
                                        if (attr.bytes(kd, ks)) {
                                            attributes.append(QString::fromUtf8(kd, ks).toLower());
                                        }
                                    } else {
                                        attr.skip(aw);
                                    }
                                }
                            }
                        } else {
                            dom.skip(dw);
                        }
                    }
                    if (value.isEmpty()) {
                        continue;
                    }
                    bool include = filter.all;
                    if (!include) {
                        include = true;
                        for (const QString &required : filter.required) {
                            if (!attributes.contains(required)) {
                                include = false;
                                break;
                            }
                        }
                        for (const QString &excluded : filter.excluded) {
                            if (attributes.contains(excluded)) {
                                include = false;
                                break;
                            }
                        }
                    }
                    if (!include) {
                        continue;
                    }
                    QString rule;
                    switch (type) {
                    case 0: rule = QStringLiteral("keyword:") + value; break;
                    case 1: rule = QStringLiteral("regexp:") + value; break;
                    case 2: rule = QStringLiteral("domain:") + value; break;
                    case 3: rule = QStringLiteral("full:") + value; break;
                    default: continue;
                    }
                    if (!seen.contains(rule)) {
                        seen.insert(rule);
                        result.append(rule);
                    }
                }
            }
            if (!top.ok() && error) {
                *error = QObject::tr("Geo file %1 is corrupted").arg(path);
            }
            if (missingTags) {
                missingTags->clear();
                for (auto it = filters.constBegin(); it != filters.constEnd(); ++it) {
                    if (!found.contains(it.key())) {
                        missingTags->append(it.key());
                    }
                }
            }
            return result;
        }

        QStringList GeoData::expandIps(const QString &path, const QStringList &codes, QStringList *missingCodes, QString *error)
        {
            QSet<QString> wanted;
            QSet<QString> reversed;
            for (const QString &raw : codes) {
                QString code = raw.trimmed().toLower();
                if (code.startsWith(QLatin1Char('!'))) {
                    code = code.mid(1);
                    reversed.insert(code);
                }
                if (!code.isEmpty()) {
                    wanted.insert(code);
                }
            }
            if (wanted.isEmpty()) {
                return {};
            }

            QByteArray data;
            if (!readFile(path, data, error)) {
                if (missingCodes) {
                    *missingCodes = QStringList(wanted.begin(), wanted.end());
                }
                return {};
            }

            IpRangeSet direct;
            IpRangeSet reverse;
            QSet<QString> found;
            PbReader top(data.constData(), data.size());
            int field, wire;
            while (top.next(field, wire)) {
                if (field != 1 || wire != 2) {
                    top.skip(wire);
                    continue;
                }
                const char *entryData;
                qsizetype entrySize;
                if (!top.bytes(entryData, entrySize)) {
                    break;
                }
                const QString code = readCode(entryData, entrySize);
                if (!wanted.contains(code)) {
                    continue;
                }
                found.insert(code);

                IpRangeSet ranges;
                bool reverseMatch = false;
                PbReader entry(entryData, entrySize);
                int f, w;
                while (entry.next(f, w)) {
                    if (f == 2 && w == 2) {
                        const char *cidrData;
                        qsizetype cidrSize;
                        if (!entry.bytes(cidrData, cidrSize)) {
                            break;
                        }
                        QByteArray ip;
                        int prefix = -1;
                        PbReader cidr(cidrData, cidrSize);
                        int cf, cw;
                        while (cidr.next(cf, cw)) {
                            if (cf == 1 && cw == 2) {
                                const char *d;
                                qsizetype s;
                                if (cidr.bytes(d, s)) {
                                    ip = QByteArray(d, s);
                                }
                            } else if (cf == 2 && cw == 0) {
                                prefix = int(cidr.varint());
                            } else {
                                cidr.skip(cw);
                            }
                        }
                        if (ip.size() == 4) {
                            ranges.addPrefixV4(quint32(uchar(ip[0])) << 24 | quint32(uchar(ip[1])) << 16 | quint32(uchar(ip[2])) << 8
                                                       | quint32(uchar(ip[3])),
                                               prefix < 0 ? 32 : qMin(prefix, 32));
                        } else if (ip.size() == 16) {
                            ranges.addPrefixV6(reinterpret_cast<const uchar *>(ip.constData()), prefix < 0 ? 128 : qMin(prefix, 128));
                        }
                    } else if (f == 3 && w == 0) {
                        reverseMatch = entry.varint() != 0;
                    } else {
                        entry.skip(w);
                    }
                }
                if (reverseMatch != reversed.contains(code)) {
                    reverse.addSet(ranges);
                } else {
                    direct.addSet(ranges);
                }
            }
            if (!top.ok() && error) {
                *error = QObject::tr("Geo file %1 is corrupted").arg(path);
            }
            if (missingCodes) {
                missingCodes->clear();
                for (const QString &code : wanted) {
                    if (!found.contains(code)) {
                        missingCodes->append(code);
                    }
                }
            }
            if (!reverse.isEmpty()) {
                direct.addSet(reverse.complement());
            }
            return direct.toCidrs();
        }

        bool GeoData::validate(const QByteArray &data, bool geoIp, QString *error)
        {
            PbReader top(data.constData(), data.size());
            int field, wire;
            int entries = 0;
            while (top.next(field, wire)) {
                if (field == 1 && wire == 2) {
                    const char *d;
                    qsizetype s;
                    if (!top.bytes(d, s)) {
                        break;
                    }
                    if (!readCode(d, s).isEmpty()) {
                        ++entries;
                    }
                } else {
                    top.skip(wire);
                }
            }
            if (!top.ok() || entries == 0) {
                if (error) {
                    *error = geoIp ? QObject::tr("The downloaded geoip file is not valid")
                                   : QObject::tr("The downloaded geosite file is not valid");
                }
                return false;
            }
            return true;
        }
    } // namespace routing
} // namespace amnezia
