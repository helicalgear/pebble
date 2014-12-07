#include <QDateTime>
#include <QMetaEnum>

#include "watchconnector.h"
#include "unpacker.h"

static const int RECONNECT_TIMEOUT = 500; //ms
static const bool PROTOCOL_DEBUG = false;

using std::function;

WatchConnector::WatchConnector(QObject *parent) :
    QObject(parent), socket(nullptr), is_connected(false)
{
    reconnectTimer.setSingleShot(true);
    connect(&reconnectTimer, SIGNAL(timeout()), SLOT(reconnect()));
}

WatchConnector::~WatchConnector()
{
}

void WatchConnector::deviceDiscovered(const QBluetoothDeviceInfo &device)
{
    //FIXME TODO: Configurable
    if (device.name().startsWith("Pebble")) {
        logger()->debug() << "Found Pebble:" << device.name() << '(' << device.address().toString() << ')';
        handleWatch(device.name(), device.address().toString());
    } else {
        logger()->debug() << "Found other device:" << device.name() << '(' << device.address().toString() << ')';
    }
}

void WatchConnector::deviceConnect(const QString &name, const QString &address)
{
    if (name.startsWith("Pebble")) handleWatch(name, address);
}

void WatchConnector::reconnect()
{
    logger()->debug() << "reconnect" << _last_name;
    if (!_last_name.isEmpty() && !_last_address.isEmpty()) {
        deviceConnect(_last_name, _last_address);
    }
}

void WatchConnector::disconnect()
{
    logger()->debug() << __FUNCTION__;
    socket->close();
    socket->deleteLater();
    reconnectTimer.stop();
    logger()->debug() << "Stopped reconnect timer";
}

void WatchConnector::handleWatch(const QString &name, const QString &address)
{
    logger()->debug() << "handleWatch" << name << address;
    reconnectTimer.stop();
    if (socket != nullptr && socket->isOpen()) {
        socket->close();
        socket->deleteLater();
    }

    bool emit_name = (_last_name != name);
    _last_name = name;
    _last_address = address;
    if (emit_name) emit nameChanged();

    logger()->debug() << "Creating socket";
#if QT_VERSION < QT_VERSION_CHECK(5, 2, 0)
    socket = new QBluetoothSocket(QBluetoothSocket::RfcommSocket);
#else
    socket = new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol);
#endif
    connect(socket, SIGNAL(readyRead()), SLOT(onReadSocket()));
    connect(socket, SIGNAL(bytesWritten(qint64)), SLOT(onBytesWritten(qint64)));
    connect(socket, SIGNAL(connected()), SLOT(onConnected()));
    connect(socket, SIGNAL(disconnected()), SLOT(onDisconnected()));
    connect(socket, SIGNAL(error(QBluetoothSocket::SocketError)), this, SLOT(onError(QBluetoothSocket::SocketError)));

    // FIXME: Assuming port 1 (with Pebble)
    socket->connectToService(QBluetoothAddress(address), 1);
}

QString WatchConnector::decodeEndpoint(uint val)
{
    QMetaEnum Endpoints = staticMetaObject.enumerator(staticMetaObject.indexOfEnumerator("Endpoint"));
    const char *endpoint = Endpoints.valueToKey(val);
    return endpoint ? QString(endpoint) : QString("watchUNKNOWN_%1").arg(val);
}

void WatchConnector::setEndpointHandler(uint endpoint, const EndpointHandlerFunc &func)
{
    if (func) {
        handlers.insert(endpoint, func);
    } else {
        handlers.remove(endpoint);
    }
}

void WatchConnector::clearEndpointHandler(uint endpoint)
{
    handlers.remove(endpoint);
}

bool WatchConnector::dispatchMessage(uint endpoint, const QByteArray &data)
{
    auto tmp_it = tmpHandlers.find(endpoint);
    if (tmp_it != tmpHandlers.end()) {
        QList<EndpointHandlerFunc>& funcs = tmp_it.value();
        bool ok = false;
        for (int i = 0; i < funcs.size(); i++) {
            if (funcs[i](data)) {
                // This handler accepted this message
                ok = true;
                // Since it is a temporary handler, remove it.
                funcs.removeAt(i);
                break;
            }
        }
        if (funcs.empty()) {
            // "Garbage collect" the tmpHandlers entry.
            tmpHandlers.erase(tmp_it);
        }
        if (ok) {
            return true;
        }
    }

    auto it = handlers.find(endpoint);
    if (it != handlers.end()) {
        if (it.value() && it.value()(data)) {
            return true;
        }
    }

    logger()->info() << "message to endpoint" << decodeEndpoint(endpoint) << "was not dispatched";
    logger()->debug() << data.toHex();
    return false;
}

void WatchConnector::onReadSocket()
{
    static const int header_length = 4;

    logger()->debug() << "readyRead bytesAvailable =" << socket->bytesAvailable();

    QBluetoothSocket *socket = qobject_cast<QBluetoothSocket *>(sender());
    Q_ASSERT(socket && socket == this->socket);

    while (socket->bytesAvailable() >= header_length) {
        // Do nothing if there is no message to read.
        if (socket->bytesAvailable() < header_length) {
            if (socket->bytesAvailable() > 0) {
                logger()->debug() << "incomplete header in read buffer";
            }
            return;
        }

        uchar header[header_length];
        socket->peek(reinterpret_cast<char*>(header), header_length);

        quint16 message_length, endpoint;
        message_length = qFromBigEndian<quint16>(&header[0]);
        endpoint = qFromBigEndian<quint16>(&header[sizeof(quint16)]);

        if (message_length > 8 * 1024) {
            // Protocol does not allow messages more than 8K long, seemingly.
            logger()->warn() << "received message size too long: " << message_length;
            socket->readAll(); // drop input buffer
            return;
        }

        // Now wait for the entire message
        if (socket->bytesAvailable() < header_length + message_length) {
            logger()->debug() << "incomplete msg body in read buffer";
            return;
        }

        socket->read(header_length); // Skip the header

        QByteArray data = socket->read(message_length);

        logger()->debug() << "received message of length" << message_length << "to endpoint" << decodeEndpoint(endpoint);
        if (PROTOCOL_DEBUG) logger()->debug() << data.toHex();

        dispatchMessage(endpoint, data);
    }
}

void WatchConnector::onConnected()
{
    logger()->debug() << "Connected!";
    bool was_connected = is_connected;
    is_connected = true;
    reconnectTimer.stop();
    reconnectTimer.setInterval(0);
    if (not was_connected) {
        if (not writeData.isEmpty()) {
            logger()->info() << "Found" << writeData.length() << "bytes in write buffer - resending";
            sendData(writeData);
        }
        emit connectedChanged();
    }
}

void WatchConnector::onDisconnected()
{
    logger()->debug() << "Disconnected!";

    bool was_connected = is_connected;
    is_connected = false;

    if (was_connected) emit connectedChanged();

    QBluetoothSocket *socket = qobject_cast<QBluetoothSocket *>(sender());
    if (!socket) return;

    socket->deleteLater();

    if (not writeData.isEmpty() && reconnectTimer.interval() > RECONNECT_TIMEOUT) {
        writeData.clear(); // 3rd time around - user is not here, do not bother with resending last message
    }

    if (reconnectTimer.interval() < 10 * RECONNECT_TIMEOUT) {
        reconnectTimer.setInterval(reconnectTimer.interval() + RECONNECT_TIMEOUT);
    }
    reconnectTimer.start();
    logger()->debug() << "will reconnect in" << reconnectTimer.interval() << "ms";
}

void WatchConnector::onError(QBluetoothSocket::SocketError error)
{
    if (error == QBluetoothSocket::UnknownSocketError) {
        logger()->info() << error << socket->errorString();
    } else {
        logger()->error() << "error connecting Pebble:" << error << socket->errorString();
    }
}

void WatchConnector::sendData(const QByteArray &data)
{
    writeData.append(data);
    if (socket == nullptr) {
        logger()->debug() << "no socket - reconnecting";
        reconnect();
    } else if (is_connected) {
        logger()->debug() << "writing" << data.length() << "bytes to socket";
        if (PROTOCOL_DEBUG) logger()->debug() << data.toHex();
        socket->write(data);
    }
}

void WatchConnector::onBytesWritten(qint64 bytes)
{
    writeData.remove(0, bytes);
    logger()->debug() << "socket written" << bytes << "bytes," << writeData.length() << "left";
}

void WatchConnector::sendMessage(uint endpoint, const QByteArray &data, const EndpointHandlerFunc &callback)
{
    logger()->debug() << "sending message to endpoint" << decodeEndpoint(endpoint);
    QByteArray msg;

    // First send the length
    msg.append((data.length() & 0xFF00) >> 8);
    msg.append(data.length() & 0xFF);

    // Then the endpoint
    msg.append((endpoint & 0xFF00) >> 8);
    msg.append(endpoint & 0xFF);

    // Finally the data
    msg.append(data);

    sendData(msg);

    if (callback) {
        tmpHandlers[endpoint].append(callback);
    }
}

void WatchConnector::buildData(QByteArray &res, QStringList data)
{
    for (QString d : data)
    {
        QByteArray tmp = d.left(0xEF).toUtf8();
        res.append((tmp.length() + 1) & 0xFF);
        res.append(tmp);
        res.append('\0');
    }
}

QByteArray WatchConnector::buildMessageData(uint lead, QStringList data)
{
    QByteArray res;
    res.append(lead & 0xFF);
    buildData(res, data);

    return res;
}

void WatchConnector::sendPhoneVersion()
{
    unsigned int sessionCap = sessionCapGAMMA_RAY;
    unsigned int remoteCap = remoteCapTELEPHONY | remoteCapSMS | osANDROID;
    QByteArray res;

    //Prefix
    res.append(0x01);
    res.append(0xff);
    res.append(0xff);
    res.append(0xff);
    res.append(0xff);

    //Session Capabilities
    res.append((char)((sessionCap >> 24) & 0xff));
    res.append((char)((sessionCap >> 16) & 0xff));
    res.append((char)((sessionCap >> 8) & 0xff));
    res.append((char)(sessionCap & 0xff));

    //Remote Capabilities
    res.append((char)((remoteCap >> 24) & 0xff));
    res.append((char)((remoteCap >> 16) & 0xff));
    res.append((char)((remoteCap >> 8) & 0xff));
    res.append((char)(remoteCap & 0xff));

    //Version Magic
    res.append((char)0x02);

    //Append Version
    res.append((char)0x02); //Major
    res.append((char)0x00); //Minor
    res.append((char)0x00); //Bugfix

    sendMessage(watchPHONE_VERSION, res);
}

void WatchConnector::ping(uint val)
{
    QByteArray res;
    res.append((char)0);

    res.append((char)((val >> 24) & 0xff));
    res.append((char)((val >> 16) & 0xff));
    res.append((char)((val >> 8) & 0xff));
    res.append((char)(val & 0xff));

    sendMessage(watchPING, res);
}

void WatchConnector::time()
{
    QByteArray res;
    QDateTime UTC(QDateTime::currentDateTimeUtc());
    QDateTime local(UTC.toLocalTime());
    local.setTimeSpec(Qt::UTC);
    int offset = UTC.secsTo(local);
    uint val = (local.toMSecsSinceEpoch() + offset) / 1000;

    res.append(0x02); //SET_TIME_REQ
    res.append((char)((val >> 24) & 0xff));
    res.append((char)((val >> 16) & 0xff));
    res.append((char)((val >> 8) & 0xff));
    res.append((char)(val & 0xff));
    sendMessage(watchTIME, res);
}

QString WatchConnector::timeStamp()
{
    return QString::number(QDateTime::currentMSecsSinceEpoch());
}

void WatchConnector::sendNotification(uint lead, QString sender, QString data, QString subject)
{
    QStringList tmp;
    tmp.append(sender);
    tmp.append(data);
    tmp.append(timeStamp());
    if (lead == leadEMAIL) tmp.append(subject);

    QByteArray res = buildMessageData(lead, tmp);

    sendMessage(watchNOTIFICATION, res);
}

void WatchConnector::sendSMSNotification(QString sender, QString data)
{
    sendNotification(leadSMS, sender, data, "");
}

void WatchConnector::sendFacebookNotification(QString sender, QString data)
{
    sendNotification(leadFACEBOOK, sender, data, "");
}

void WatchConnector::sendTwitterNotification(QString sender, QString data)
{
    sendNotification(leadTWITTER, sender, data, "");
}

void WatchConnector::sendEmailNotification(QString sender, QString data, QString subject)
{
    sendNotification(leadEMAIL, sender, data, subject);
}

void WatchConnector::sendMusicNowPlaying(QString track, QString album, QString artist)
{
    QStringList tmp;
    tmp.append(track.left(30));
    tmp.append(album.left(30));
    tmp.append(artist.left(30));

    QByteArray res = buildMessageData(leadNOW_PLAYING_DATA, tmp);

    sendMessage(watchMUSIC_CONTROL, res);
}

void WatchConnector::phoneControl(char act, uint cookie, QStringList datas)
{
    QByteArray head;
    head.append((char)act);
    head.append((cookie >> 24)& 0xFF);
    head.append((cookie >> 16)& 0xFF);
    head.append((cookie >> 8)& 0xFF);
    head.append(cookie & 0xFF);
    if (datas.length()>0) buildData(head, datas);

    sendMessage(watchPHONE_CONTROL, head);
}

void WatchConnector::ring(QString number, QString name, bool incoming, uint cookie)
{
    QStringList tmp;
    tmp.append(number);
    tmp.append(name);

    char act = callINCOMING;
    if (!incoming) {
        act = callOUTGOING;
    }

    phoneControl(act, cookie, tmp);
}

void WatchConnector::startPhoneCall(uint cookie)
{
    phoneControl(callSTART, cookie, QStringList());
}

void WatchConnector::endPhoneCall(uint cookie)
{
    phoneControl(callEND, cookie, QStringList());
}
