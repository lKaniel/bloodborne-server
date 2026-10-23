// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QtEndian>
#include "bbcoop_revision.h"
#include "config.h"
#include "coop/build_fingerprint.h"
#include "link_server.h"

namespace {

// This server's release: BB_RELEASE_VERSION, else the release's VERSION.txt (beside the server's
// folder, as the launcher reads it), else "" (a dev build).
QString ReleaseVersion() {
    const QString given = qEnvironmentVariable("BB_RELEASE_VERSION");
    if (!given.isEmpty()) {
        return given;
    }
    const QDir folder{QCoreApplication::applicationDirPath()};
    for (const QString& path : {folder.filePath(QStringLiteral("../VERSION.txt")),
                                folder.filePath(QStringLiteral("VERSION.txt"))}) {
        QFile file{path};
        if (file.open(QIODevice::ReadOnly)) {
            return QString::fromStdString(
                BloodborneCoop::ReleaseVersionOf(file.read(4096).toStdString()));
        }
    }
    return {};
}

} // namespace

LinkServer::LinkServer(SharedState* shared, const QString& db_path, quint16 tcp_port,
                       QObject* parent)
    : QObject(parent), m_socket(new QUdpSocket(this)), m_shared(shared),
      m_linkTimer(new QTimer(this)),
      m_world(shared->config ? shared->config->GetBloodborneWorldPath() : QString{}),
      m_worldTimer(new QTimer(this)), m_db(QStringLiteral("link_sign_in")), m_tcpPort(tcp_port),
      m_rpcInstance(QRandomGenerator::global()->generate() | 1u) {
    if (!m_db.Open(db_path))
        qCritical() << "Link: the accounts did not open:" << m_db.lastError();
    // Phase 11: the hello signs the player in (its id's account is made the first time).
    m_link.SetSignIn([this](const QByteArray& online_id, const QByteArray& secret) {
        return SignIn(online_id, secret);
    });
    // Build check on join: a game runs this server's release, or it is refused.
    const auto own = BloodborneCoop::EncodeBuildFingerprint(
        BloodborneCoop::ServerBuildFingerprint(ReleaseVersion().toStdString(), BB_SCM_REV));
    if (!shared->config || shared->config->IsServerBuildChecked()) {
        m_link.SetServerBuild(QByteArray::fromStdString(own));
        qInfo().noquote() << "Link: games must run this server's build:"
                          << QString::fromStdString(own);
    } else {
        qInfo().noquote() << "Link: this server's build (" << QString::fromStdString(own)
                          << ") is not checked (CheckServerBuild=false): the games here only";
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &LinkServer::OnReadyRead);
    connect(m_linkTimer, &QTimer::timeout, this, &LinkServer::OnLinkTick);
    connect(m_worldTimer, &QTimer::timeout, this, &LinkServer::OnWorldTick);
    // Phase 7: the link relays only between games of one team (the party of their hello).
    m_link.SetPartyOf([this](const ServerLink::Endpoint& link) { return m_world.PartyOf(link); });
    // Phase 9: and forwards a game packet only to a teammate that needs it.
    m_link.SetPacketRoutes(
        [this](const ServerLink::Endpoint& from, const ServerLink::Endpoint& to, quint32 channel) {
            return m_world.RoutesPacket(from, to, channel);
        });
    m_clock.start();
    m_linkTimer->start(500);
    // The worlds' clock is the wall clock: their journals keep it across restarts.
    Apply(m_world.Load(QDateTime::currentMSecsSinceEpoch()));
    m_worldTimer->start(50);
}

bool LinkServer::Start(const QHostAddress& addr, uint16_t port) {
    if (!m_socket->bind(addr, port)) {
        qCritical() << "Link UDP bind failed on" << addr.toString() << ":" << port
                    << m_socket->errorString();
        return false;
    }
    qInfo().nospace().noquote() << "Link UDP listener on: " << addr.toString() << ":" << port;
    return true;
}

void LinkServer::OnReadyRead() {
    while (m_socket->hasPendingDatagrams()) {
        QByteArray raw;
        raw.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        QHostAddress sender;
        uint16_t senderPort = 0;
        m_socket->readDatagram(raw.data(), raw.size(), &sender, &senderPort);

        if (raw.isEmpty())
            continue;

        // The games' link only (phase 9: no STUN, nothing else on this port).
        if (ServerLink::KindOf(raw)) {
            HandleLink(raw, sender, senderPort);
        }
    }
}

void LinkServer::HandleLink(const QByteArray& datagram, const QHostAddress& sender,
                            uint16_t senderPort) {
    bool ipv4 = false;
    const quint32 address = sender.toIPv4Address(&ipv4);
    if (!ipv4)
        return;
    Apply(m_link.Receive({address, senderPort}, datagram, m_clock.elapsed()));
}

void LinkServer::OnLinkTick() {
    Apply(m_link.Tick(m_clock.elapsed()));
}

void LinkServer::OnWorldTick() {
    Apply(m_world.Tick(QDateTime::currentMSecsSinceEpoch()));
    // The Rpc tunnels' resends and keepalives.
    for (auto& [id, end] : m_rpc)
        FlushRpc(*end);
}

bool LinkServer::SignIn(const QByteArray& online_id, const QByteArray& secret) {
    // A hello comes again every second until it is welcomed, and after every outage: the
    // password's hash (100k rounds) only the first time.
    const QByteArray hash = QCryptographicHash::hash(secret, QCryptographicHash::Sha256);
    const auto known = m_signedIn.constFind(online_id);
    if (known != m_signedIn.constEnd())
        return *known == hash;
    const bool make = !m_shared->config || m_shared->config->IsMakingAccountsAtSignIn();
    const auto user =
        m_db.SignIn(QString::fromUtf8(online_id), QString::fromUtf8(secret), {}, false, make);
    if (!user)
        return false;
    m_signedIn.insert(online_id, hash);
    return true;
}

void LinkServer::OnRpc(const ServerLink::Endpoint& from, const QByteArray& online_id,
                       const QByteArray& body) {
    auto& slot = m_rpc[online_id];
    if (!slot) {
        slot = std::make_unique<RpcEnd>();
        slot->tunnel = std::make_unique<BloodborneCoop::RpcTunnel>("shadnet", m_rpcInstance);
    }
    RpcEnd& end = *slot;
    end.to = from;
    const auto bytes =
        std::span{reinterpret_cast<const u8*>(body.constData()), static_cast<size_t>(body.size())};
    for (auto& event : end.tunnel->Receive(bytes, m_clock.elapsed())) {
        using Kind = BloodborneCoop::RpcTunnel::Event::Kind;
        switch (event.kind) {
        case Kind::Opened:
            OpenRpc(online_id, end);
            break;
        case Kind::Data: {
            const QByteArray data(reinterpret_cast<const char*>(event.bytes.data()),
                                  static_cast<qsizetype>(event.bytes.size()));
            if (end.socket && end.socket->state() == QAbstractSocket::ConnectedState)
                end.socket->write(data);
            else
                end.pending.append(data);
            break;
        }
        case Kind::Closed:
            CloseRpc(end);
            break;
        }
    }
    FlushRpc(end);
}

void LinkServer::OpenRpc(const QByteArray& online_id, RpcEnd& end) {
    CloseRpc(end);
    QTcpSocket* socket = new QTcpSocket(this);
    end.socket = socket;
    // The game's end, while this socket is still its connection (it may be gone by then).
    const auto mine = [this, online_id, socket]() -> RpcEnd* {
        const auto found = m_rpc.find(online_id);
        return found != m_rpc.end() && found->second->socket == socket ? found->second.get()
                                                                       : nullptr;
    };
    connect(socket, &QTcpSocket::connected, this, [socket, mine] {
        if (RpcEnd* at = mine(); at && !at->pending.isEmpty())
            socket->write(std::exchange(at->pending, {}));
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket, mine] {
        const QByteArray data = socket->readAll();
        RpcEnd* at = mine();
        if (!at)
            return;
        at->tunnel->Write(std::span{reinterpret_cast<const u8*>(data.constData()),
                                    static_cast<size_t>(data.size())},
                          m_clock.elapsed());
        FlushRpc(*at);
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket, mine] {
        RpcEnd* at = mine();
        socket->deleteLater();
        if (!at)
            return;
        at->socket = nullptr;
        at->tunnel->Close(m_clock.elapsed());
        FlushRpc(*at);
    });
    socket->connectToHost(QHostAddress::LocalHost, m_tcpPort);
}

void LinkServer::CloseRpc(RpcEnd& end) {
    end.pending.clear();
    if (QTcpSocket* socket = std::exchange(end.socket, nullptr)) {
        // Its disconnected signal finds it no longer the game's connection, and deletes it.
        socket->disconnectFromHost();
        if (socket->state() == QAbstractSocket::UnconnectedState)
            socket->deleteLater();
    }
}

void LinkServer::FlushRpc(RpcEnd& end) {
    for (const auto& body : end.tunnel->Flush(m_clock.elapsed())) {
        m_socket->writeDatagram(
            ServerLink::EncodeRpc(QByteArray(reinterpret_cast<const char*>(body.data()),
                                             static_cast<qsizetype>(body.size()))),
            QHostAddress(end.to.ipv4), end.to.port);
    }
}

void LinkServer::Apply(const ServerLink::Hub::Result& result) {
    for (const auto& send : result.sends)
        m_socket->writeDatagram(send.datagram, QHostAddress(send.to.ipv4), send.to.port);
    for (const auto& line : result.log)
        qInfo().noquote() << line;
    for (const auto& gone : result.gone) {
        m_world.Forget(gone);
        // A game gone (or moved to another address): its connection to the TCP port is over.
        for (auto it = m_rpc.begin(); it != m_rpc.end();) {
            if (it->second->to == gone) {
                CloseRpc(*it->second);
                it = m_rpc.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Phase 7: a linked game's address for the rooms and signaling is its virtual one, known from
    // its hello.
    for (const auto& linked : result.linked) {
        QWriteLocker lk(&m_shared->matching.udpLock);
        m_shared->matching.udpExt[QString::fromUtf8(linked.online_id)] = {
            QHostAddress(linked.virtual_endpoint.ipv4).toString(), linked.virtual_endpoint.port};
    }
    // Phase 11: a game silent too long (died, or down for longer than a blip): its TCP side ends,
    // and the game is told when it is back (its client signs in again by itself).
    for (const auto& lost : result.lost) {
        for (auto& [id, end] : m_rpc) {
            if (end->to == lost && end->socket) {
                CloseRpc(*end);
                end->tunnel->Close(m_clock.elapsed());
            }
        }
    }
    for (const auto& coop : result.coop)
        Apply(m_world.Receive(coop.from, coop.online_id, coop.body,
                              QDateTime::currentMSecsSinceEpoch()));
    for (const auto& rpc : result.rpc)
        OnRpc(rpc.from, rpc.online_id, rpc.body);
}

void LinkServer::Apply(const BloodborneWorld::Service::Result& result) {
    for (const auto& send : result.sends)
        m_socket->writeDatagram(ServerLink::EncodeCoop(send.body), QHostAddress(send.to.ipv4),
                                send.to.port);
    for (const auto& line : result.log)
        qInfo().noquote() << line;
}
