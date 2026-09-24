#ifndef IPRANGES_H
#define IPRANGES_H

#include <QList>
#include <QString>
#include <QStringList>

namespace amnezia
{
    namespace routing
    {
        struct U128
        {
            quint64 hi = 0;
            quint64 lo = 0;

            bool operator<(const U128 &o) const { return hi < o.hi || (hi == o.hi && lo < o.lo); }
            bool operator==(const U128 &o) const { return hi == o.hi && lo == o.lo; }
            bool operator<=(const U128 &o) const { return *this < o || *this == o; }
            bool isMax() const { return hi == ~quint64(0) && lo == ~quint64(0); }
            bool isZero() const { return hi == 0 && lo == 0; }
            U128 plusOne() const;
            U128 minusOne() const;
        };

        // A set of IPv4/IPv6 address ranges that can be merged, complemented
        // and converted back to a minimal list of CIDR prefixes.
        class IpRangeSet
        {
        public:
            void addPrefixV4(quint32 address, int prefix);
            void addPrefixV6(const unsigned char address[16], int prefix);
            // Accepts "1.2.3.4", "1.2.3.0/24", "2001:db8::/32". Returns false if invalid.
            bool addCidr(const QString &cidr);
            void addSet(const IpRangeSet &other);

            bool isEmpty() const { return m_v4.isEmpty() && m_v6.isEmpty(); }

            IpRangeSet complement() const;
            QStringList toCidrs() const;
            int v4Count() const;

        private:
            struct R4 { quint32 lo, hi; };
            struct R6 { U128 lo, hi; };

            void normalize() const;

            mutable QList<R4> m_v4;
            mutable QList<R6> m_v6;
            mutable bool m_normalized = true;
        };
    } // namespace routing
} // namespace amnezia

#endif // IPRANGES_H
