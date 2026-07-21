#ifndef ROUTINGPROFILESUICONTROLLER_H
#define ROUTINGPROFILESUICONTROLLER_H

#include <QObject>

#include "core/repositories/secureAppSettingsRepository.h"
#include "ui/models/routingProfilesModel.h"

// Backs the routing-profile UI. All persistence lives in SecureAppSettingsRepository;
// this controller exposes an editable "draft" of one profile via Q_PROPERTY so QML can
// bind form fields, then serializes the draft back into a RoutingProfile on save.
class RoutingProfilesUiController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool isRoutingEnabled READ isRoutingEnabled WRITE setRoutingEnabled NOTIFY isRoutingEnabledChanged)

    Q_PROPERTY(QString editName READ editName WRITE setEditName NOTIFY draftChanged)
    Q_PROPERTY(bool editGlobalProxy READ editGlobalProxy WRITE setEditGlobalProxy NOTIFY draftChanged)
    Q_PROPERTY(int editOrder READ editOrder WRITE setEditOrder NOTIFY draftChanged)
    Q_PROPERTY(int editDomainStrategy READ editDomainStrategy WRITE setEditDomainStrategy NOTIFY draftChanged)
    Q_PROPERTY(bool editFakeDns READ editFakeDns WRITE setEditFakeDns NOTIFY draftChanged)

    Q_PROPERTY(QString editProxySites READ editProxySites WRITE setEditProxySites NOTIFY draftChanged)
    Q_PROPERTY(QString editProxyIp READ editProxyIp WRITE setEditProxyIp NOTIFY draftChanged)
    Q_PROPERTY(QString editDirectSites READ editDirectSites WRITE setEditDirectSites NOTIFY draftChanged)
    Q_PROPERTY(QString editDirectIp READ editDirectIp WRITE setEditDirectIp NOTIFY draftChanged)
    Q_PROPERTY(QString editBlockSites READ editBlockSites WRITE setEditBlockSites NOTIFY draftChanged)
    Q_PROPERTY(QString editBlockIp READ editBlockIp WRITE setEditBlockIp NOTIFY draftChanged)

    Q_PROPERTY(int editRemoteDnsMode READ editRemoteDnsMode WRITE setEditRemoteDnsMode NOTIFY draftChanged)
    Q_PROPERTY(QString editRemoteDnsDomain READ editRemoteDnsDomain WRITE setEditRemoteDnsDomain NOTIFY draftChanged)
    Q_PROPERTY(QString editRemoteDnsIp READ editRemoteDnsIp WRITE setEditRemoteDnsIp NOTIFY draftChanged)
    Q_PROPERTY(int editDomesticDnsMode READ editDomesticDnsMode WRITE setEditDomesticDnsMode NOTIFY draftChanged)
    Q_PROPERTY(QString editDomesticDnsDomain READ editDomesticDnsDomain WRITE setEditDomesticDnsDomain NOTIFY draftChanged)
    Q_PROPERTY(QString editDomesticDnsIp READ editDomesticDnsIp WRITE setEditDomesticDnsIp NOTIFY draftChanged)

    Q_PROPERTY(QString editGeoipUrl READ editGeoipUrl WRITE setEditGeoipUrl NOTIFY draftChanged)
    Q_PROPERTY(QString editGeositeUrl READ editGeositeUrl WRITE setEditGeositeUrl NOTIFY draftChanged)

public:
    explicit RoutingProfilesUiController(SecureAppSettingsRepository *appSettingsRepository,
                                         RoutingProfilesModel *model, QObject *parent = nullptr);

    bool isRoutingEnabled() const;
    void setRoutingEnabled(bool enabled);

    QString editName() const { return m_draft.name; }
    void setEditName(const QString &v);
    bool editGlobalProxy() const { return m_draft.globalProxy; }
    void setEditGlobalProxy(bool v);
    int editOrder() const { return static_cast<int>(m_draft.order); }
    void setEditOrder(int v);
    int editDomainStrategy() const { return static_cast<int>(m_draft.domainStrategy); }
    void setEditDomainStrategy(int v);
    bool editFakeDns() const { return m_draft.fakeDns; }
    void setEditFakeDns(bool v);

    QString editProxySites() const { return m_draft.proxySites.join('\n'); }
    void setEditProxySites(const QString &v);
    QString editProxyIp() const { return m_draft.proxyIp.join('\n'); }
    void setEditProxyIp(const QString &v);
    QString editDirectSites() const { return m_draft.directSites.join('\n'); }
    void setEditDirectSites(const QString &v);
    QString editDirectIp() const { return m_draft.directIp.join('\n'); }
    void setEditDirectIp(const QString &v);
    QString editBlockSites() const { return m_draft.blockSites.join('\n'); }
    void setEditBlockSites(const QString &v);
    QString editBlockIp() const { return m_draft.blockIp.join('\n'); }
    void setEditBlockIp(const QString &v);

    int editRemoteDnsMode() const { return static_cast<int>(m_draft.remoteDns.mode); }
    void setEditRemoteDnsMode(int v);
    QString editRemoteDnsDomain() const { return m_draft.remoteDns.domain; }
    void setEditRemoteDnsDomain(const QString &v);
    QString editRemoteDnsIp() const { return m_draft.remoteDns.ip; }
    void setEditRemoteDnsIp(const QString &v);
    int editDomesticDnsMode() const { return static_cast<int>(m_draft.domesticDns.mode); }
    void setEditDomesticDnsMode(int v);
    QString editDomesticDnsDomain() const { return m_draft.domesticDns.domain; }
    void setEditDomesticDnsDomain(const QString &v);
    QString editDomesticDnsIp() const { return m_draft.domesticDns.ip; }
    void setEditDomesticDnsIp(const QString &v);

    QString editGeoipUrl() const { return m_draft.geoipUrl; }
    void setEditGeoipUrl(const QString &v);
    QString editGeositeUrl() const { return m_draft.geositeUrl; }
    void setEditGeositeUrl(const QString &v);

public slots:
    void updateModel();

    // Prepare the draft for a new profile (index < 0) or load an existing one.
    void beginNewProfile();
    void loadProfile(int index);
    // Persist the current draft. If editing an existing profile (matched by original
    // name) it is replaced, otherwise a new profile is appended. Returns false with
    // errorOccurred on validation failure (empty/duplicate name).
    void saveDraft();

    void removeProfile(int index);
    void setActiveProfile(int index);

    // Import a happ://routing/add|onadd/<base64> deeplink as a new/updated profile.
    void importDeeplink(const QString &deeplink);
    QString exportActiveDeeplink() const;

    // Validation helper for the UI: returns how many lines in the given text are not
    // valid split-tunneling rules of the requested kind (domain vs ip).
    int countInvalidRules(const QString &text, bool domainSide) const;

signals:
    void isRoutingEnabledChanged();
    void draftChanged();
    void profilesChanged();
    void errorOccurred(const QString &message);
    void finished(const QString &message);

private:
    QStringList splitLines(const QString &text) const;
    bool hasProfileNamed(const QString &name, int exceptIndex) const;

    // Resolve the plain domains of a stored profile's proxySites/directSites to IPs
    // (async DNS) and cache them on the profile, so non-xray protocols can route by
    // those IPs. When refresh is true the cache is rebuilt from scratch; when false
    // it is only topped up (used at startup to avoid an empty-cache connect window).
    void resolveProfileDomains(const QString &profileName, bool refresh = true);
    // Returns the single hostname a rule resolves to, or empty if the rule is not a
    // single resolvable host (keyword:/regexp:/dotless:/geosite:/ext:/ip rules).
    static QString resolvableHost(const QString &rule);
    // Ask the privileged service to expand geosite: tokens into resolvable hostnames
    // (the service bundles geosite.dat). Desktop only; returns empty elsewhere or when
    // the service is unreachable, in which case geosite rules resolve once it is up.
    QStringList expandGeositeViaService(const QStringList &tokens) const;

    SecureAppSettingsRepository *m_appSettingsRepository;
    RoutingProfilesModel *m_model;

    RoutingProfile m_draft;
    QString m_editingOriginalName;
};

#endif // ROUTINGPROFILESUICONTROLLER_H
