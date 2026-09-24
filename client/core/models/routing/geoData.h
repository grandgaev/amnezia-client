#ifndef GEODATA_H
#define GEODATA_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace amnezia
{
    namespace routing
    {
        // Reader for the V2Ray/Xray geo data files (geosite.dat, geoip.dat).
        // The files are protobuf encoded:
        //   GeoSiteList { repeated GeoSite entry = 1; }
        //   GeoSite     { string country_code = 1; repeated Domain domain = 2; }
        //   Domain      { Type type = 1; string value = 2; repeated Attribute attribute = 3; }
        //               Type: Plain = 0, Regex = 1, Domain = 2, Full = 3
        //   Attribute   { string key = 1; ... }
        //   GeoIPList   { repeated GeoIP entry = 1; }
        //   GeoIP       { string country_code = 1; repeated CIDR cidr = 2; bool reverse_match = 3; }
        //   CIDR        { bytes ip = 1; uint32 prefix = 2; }
        class GeoData
        {
        public:
            // Lower-case list of the categories (tags) contained in a file.
            static QStringList listTags(const QString &path, QString *error = nullptr);

            // Expands "geosite:<tag>[@attr]" entries to router/Xray domain rules
            // ("full:", "domain:", "keyword:", "regexp:"). Tags are given without
            // the "geosite:" prefix. Unknown tags are reported in missingTags.
            static QStringList expandSites(const QString &path, const QStringList &tags, QStringList *missingTags = nullptr,
                                           QString *error = nullptr);

            // Expands "geoip:<code>" (and "geoip:!<code>") to CIDR strings.
            static QStringList expandIps(const QString &path, const QStringList &codes, QStringList *missingCodes = nullptr,
                                         QString *error = nullptr);

            // Checks that a file looks like a valid geo data file of the given kind.
            static bool validate(const QByteArray &data, bool geoIp, QString *error = nullptr);
        };
    } // namespace routing
} // namespace amnezia

#endif // GEODATA_H
