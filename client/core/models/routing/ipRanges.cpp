#include "ipRanges.h"

#include <QHostAddress>

#include <algorithm>

namespace amnezia
{
    namespace routing
    {
        U128 U128::plusOne() const
        {
            U128 r = *this;
            if (++r.lo == 0) {
                ++r.hi;
            }
            return r;
        }

        U128 U128::minusOne() const
        {
            U128 r = *this;
            if (r.lo-- == 0) {
                --r.hi;
            }
            return r;
        }

        namespace
        {
            U128 fromBytes(const unsigned char a[16])
            {
                U128 r;
                for (int i = 0; i < 8; ++i) {
                    r.hi = (r.hi << 8) | a[i];
                    r.lo = (r.lo << 8) | a[i + 8];
                }
                return r;
            }

            // Mask with the lowest (128 - prefix) bits set.
            U128 hostMask(int prefix)
            {
                U128 m;
                const int hostBits = 128 - prefix;
                if (hostBits >= 128) {
                    m.hi = m.lo = ~quint64(0);
                } else if (hostBits > 64) {
                    m.lo = ~quint64(0);
                    m.hi = (quint64(1) << (hostBits - 64)) - 1;
                } else if (hostBits == 64) {
                    m.lo = ~quint64(0);
                } else if (hostBits > 0) {
                    m.lo = (quint64(1) << hostBits) - 1;
                }
                return m;
            }

            QString v4ToString(quint32 a)
            {
                return QStringLiteral("%1.%2.%3.%4").arg(a >> 24).arg((a >> 16) & 0xff).arg((a >> 8) & 0xff).arg(a & 0xff);
            }

            QString v6ToString(const U128 &a)
            {
                Q_IPV6ADDR raw;
                for (int i = 0; i < 8; ++i) {
                    raw[i] = quint8(a.hi >> (56 - 8 * i));
                    raw[i + 8] = quint8(a.lo >> (56 - 8 * i));
                }
                return QHostAddress(raw).toString();
            }

            // Number of trailing zero bits of a (0..32), 32 for a == 0.
            int trailingZeros32(quint32 a)
            {
                if (a == 0) {
                    return 32;
                }
                int n = 0;
                while (!(a & 1)) {
                    a >>= 1;
                    ++n;
                }
                return n;
            }

            int trailingZeros128(const U128 &a)
            {
                if (a.lo != 0) {
                    int n = 0;
                    quint64 v = a.lo;
                    while (!(v & 1)) {
                        v >>= 1;
                        ++n;
                    }
                    return n;
                }
                if (a.hi != 0) {
                    int n = 64;
                    quint64 v = a.hi;
                    while (!(v & 1)) {
                        v >>= 1;
                        ++n;
                    }
                    return n;
                }
                return 128;
            }
        } // namespace

        void IpRangeSet::addPrefixV4(quint32 address, int prefix)
        {
            prefix = qBound(0, prefix, 32);
            const quint32 host = prefix == 0 ? ~quint32(0) : (prefix == 32 ? 0 : (~quint32(0) >> prefix));
            const quint32 lo = address & ~host;
            m_v4.append({ lo, lo | host });
            m_normalized = false;
        }

        void IpRangeSet::addPrefixV6(const unsigned char address[16], int prefix)
        {
            prefix = qBound(0, prefix, 128);
            const U128 a = fromBytes(address);
            const U128 host = hostMask(prefix);
            U128 lo { a.hi & ~host.hi, a.lo & ~host.lo };
            U128 hi { lo.hi | host.hi, lo.lo | host.lo };
            m_v6.append({ lo, hi });
            m_normalized = false;
        }

        bool IpRangeSet::addCidr(const QString &cidr)
        {
            const QString s = cidr.trimmed();
            QHostAddress address;
            int prefix = -1;
            if (s.contains(QLatin1Char('/'))) {
                const auto subnet = QHostAddress::parseSubnet(s);
                if (subnet.first.isNull() || subnet.second < 0) {
                    return false;
                }
                address = subnet.first;
                prefix = subnet.second;
            } else if (!address.setAddress(s)) {
                return false;
            }
            bool isV4 = false;
            const quint32 v4 = address.toIPv4Address(&isV4);
            if (isV4) {
                addPrefixV4(v4, prefix < 0 ? 32 : prefix);
                return true;
            }
            const Q_IPV6ADDR v6 = address.toIPv6Address();
            addPrefixV6(v6.c, prefix < 0 ? 128 : prefix);
            return true;
        }

        void IpRangeSet::addSet(const IpRangeSet &other)
        {
            m_v4.append(other.m_v4);
            m_v6.append(other.m_v6);
            m_normalized = false;
        }

        void IpRangeSet::normalize() const
        {
            if (m_normalized) {
                return;
            }
            std::sort(m_v4.begin(), m_v4.end(), [](const R4 &a, const R4 &b) { return a.lo < b.lo; });
            QList<R4> merged4;
            for (const R4 &r : m_v4) {
                if (!merged4.isEmpty()) {
                    R4 &last = merged4.last();
                    if (r.lo <= last.hi || (last.hi != ~quint32(0) && r.lo == last.hi + 1)) {
                        last.hi = qMax(last.hi, r.hi);
                        continue;
                    }
                }
                merged4.append(r);
            }
            m_v4 = merged4;

            std::sort(m_v6.begin(), m_v6.end(), [](const R6 &a, const R6 &b) { return a.lo < b.lo; });
            QList<R6> merged6;
            for (const R6 &r : m_v6) {
                if (!merged6.isEmpty()) {
                    R6 &last = merged6.last();
                    if (r.lo <= last.hi || (!last.hi.isMax() && r.lo == last.hi.plusOne())) {
                        if (last.hi < r.hi) {
                            last.hi = r.hi;
                        }
                        continue;
                    }
                }
                merged6.append(r);
            }
            m_v6 = merged6;
            m_normalized = true;
        }

        IpRangeSet IpRangeSet::complement() const
        {
            normalize();
            IpRangeSet result;
            quint32 next4 = 0;
            bool done4 = false;
            for (const R4 &r : m_v4) {
                if (r.lo > next4) {
                    result.m_v4.append({ next4, r.lo - 1 });
                }
                if (r.hi == ~quint32(0)) {
                    done4 = true;
                    break;
                }
                next4 = r.hi + 1;
            }
            if (!done4) {
                result.m_v4.append({ next4, ~quint32(0) });
            }

            U128 next6;
            bool done6 = false;
            for (const R6 &r : m_v6) {
                if (next6 < r.lo) {
                    result.m_v6.append({ next6, r.lo.minusOne() });
                }
                if (r.hi.isMax()) {
                    done6 = true;
                    break;
                }
                next6 = r.hi.plusOne();
            }
            if (!done6) {
                U128 max;
                max.hi = max.lo = ~quint64(0);
                result.m_v6.append({ next6, max });
            }
            result.m_normalized = true;
            return result;
        }

        int IpRangeSet::v4Count() const
        {
            normalize();
            return int(m_v4.size());
        }

        QStringList IpRangeSet::toCidrs() const
        {
            normalize();
            QStringList out;
            for (const R4 &r : m_v4) {
                quint64 start = r.lo;
                const quint64 end = r.hi;
                while (start <= end) {
                    int bits = trailingZeros32(quint32(start));
                    // Largest block that starts at 'start' and fits within the range.
                    while (bits > 0 && start + ((quint64(1) << bits) - 1) > end) {
                        --bits;
                    }
                    out.append(v4ToString(quint32(start)) + QLatin1Char('/') + QString::number(32 - bits));
                    start += quint64(1) << bits;
                }
            }
            for (const R6 &r : m_v6) {
                U128 start = r.lo;
                while (true) {
                    int bits = trailingZeros128(start);
                    if (bits > 128) {
                        bits = 128;
                    }
                    // Shrink the block until its end is within the range.
                    while (bits > 0) {
                        const U128 mask = hostMask(128 - bits);
                        const U128 blockEnd { start.hi | mask.hi, start.lo | mask.lo };
                        if (blockEnd <= r.hi) {
                            break;
                        }
                        --bits;
                    }
                    out.append(v6ToString(start) + QLatin1Char('/') + QString::number(128 - bits));
                    const U128 mask = hostMask(128 - bits);
                    const U128 blockEnd { start.hi | mask.hi, start.lo | mask.lo };
                    if (blockEnd == r.hi || blockEnd.isMax()) {
                        break;
                    }
                    start = blockEnd.plusOne();
                }
            }
            return out;
        }
    } // namespace routing
} // namespace amnezia
