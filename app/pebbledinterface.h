#ifndef PEBBLEDINTERFACE_H
#define PEBBLEDINTERFACE_H

#include <QObject>
#include <QUrl>
#include <QHash>
#include <QUuid>
#include <QImage>
#include <QDBusInterface>

class OrgPebbledWatchInterface;

class PebbledInterface : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QString name READ name NOTIFY nameChanged)
    Q_PROPERTY(QString address READ address NOTIFY addressChanged)
    Q_PROPERTY(QVariantMap info READ info NOTIFY infoChanged)
    Q_PROPERTY(QString appUuid READ appUuid NOTIFY appUuidChanged)

    Q_PROPERTY(QStringList appSlots READ appSlots NOTIFY appSlotsChanged)
    Q_PROPERTY(QVariantList allApps READ allApps NOTIFY allAppsChanged)

public:
    explicit PebbledInterface(QObject *parent = 0);

    bool enabled() const;
    bool active() const;
    bool connected() const;
    QString name() const;
    QString address() const;
    QVariantMap info() const;
    QString appUuid() const;

    QStringList appSlots() const;
    QVariantList allApps() const;

    Q_INVOKABLE static bool registerAppFile(const QString& filePath);
    Q_INVOKABLE static bool unregisterAppFile(const QString& filePath);

    Q_INVOKABLE QVariantMap appInfoByUuid(const QString& uuid) const;

    Q_INVOKABLE QUrl configureApp(const QString &uuid);

    Q_INVOKABLE bool isAppInstalled(const QString &uuid) const;

    QImage menuIconForApp(const QUuid &uuid) const;

signals:
    void enabledChanged();
    void activeChanged();
    void connectedChanged();
    void nameChanged();
    void addressChanged();
    void infoChanged();
    void appUuidChanged();
    void appSlotsChanged();
    void allAppsChanged();

public slots:
    void setEnabled(bool);
    void setActive(bool);
    void ping(uint cookie = 66);
    void time();
    void disconnect();
    void reconnect();

    void setAppConfiguration(const QString &uuid, const QString &data);

    void launchApp(const QString &uuid);
    void uploadApp(const QString &uuid, int slot);
    void unloadApp(int slot);

    void notifyFirmware(bool ok);
    void uploadFirmware(const QString &file);

private slots:
    void onWatchConnectedChanged();
    void getUnitProperties();
    void onPropertiesChanged(QString interface, QMap<QString, QVariant> changed, QStringList invalidated);
    void refreshAppSlots();
    void refreshAllApps();

private:
    QDBusInterface *systemd;
    OrgPebbledWatchInterface *watch;
    QDBusObjectPath unitPath;
    QVariantMap unitProperties;

    // Cached properties
    QStringList _appSlots;
    QVariantList _apps;
    QHash<QUuid, int> _appsByUuid;
    QHash<QUuid, QImage> _appMenuIcons;
};

#endif // PEBBLEDINTERFACE_H
