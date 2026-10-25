// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QFile>
#include <QSettings>
#include <QTextStream>
#include "config.h"

void ConfigManager::LoadBannedDomains() {
    m_bannedDomains.clear();
    QFile f("domains_banlist.txt");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString d = in.readLine().trimmed().toLower();
        if (!d.isEmpty() && !d.startsWith('#'))
            m_bannedDomains.insert(d);
    }
    qInfo() << "Loaded" << m_bannedDomains.size() << "banned domains";
}

void ConfigManager::Parse(const QString& path) {
    QWriteLocker lk(&m_lock);

    m_path = path;

    QSettings s(path, QSettings::IniFormat);

    auto str = [&](const QString& key, const QString& def) -> QString {
        if (!s.contains(key))
            s.setValue(key, def);
        return s.value(key, def).toString();
    };
    auto boolean = [&](const QString& key, bool def) -> bool {
        if (!s.contains(key))
            s.setValue(key, def);
        return s.value(key, def).toBool();
    };
    auto strList = [&](const QString& key) -> QStringList {
        if (!s.contains(key))
            s.setValue(key, QString());
        QString raw = s.value(key).toString();
        return raw.isEmpty() ? QStringList{} : raw.split(',', Qt::SkipEmptyParts);
    };

    m_host = str("Host", "127.0.0.1");
    m_unsecured_port = str("UnsecuredPort", "31313");
    m_matchingUdpPort = str("MatchingUdpPort", "31314");
    m_webapiPort = str("WebApiPort", "31315");
    m_statsEnabled = boolean("StatsEnabled", true);
    m_matching2Enabled = boolean("Matching2Enabled", true);
    m_bloodborneSeamlessCoop = boolean("BloodborneSeamlessCoop", false);
    m_checkServerBuild = boolean("CheckServerBuild", true);
    m_backend.enabled = boolean("BloodborneBackend", true);
    m_backend.port = str("BackendPort", "20443").toUShort();
    m_backend.address = str("BackendAddress", "").trimmed();
    m_backend.folder = str("BackendFolder", "../backend");
    m_backend.store = str("BackendStore", "");
    m_backend.showOwn = str("BackendShowOwn", "");
    m_backend.ghostTtlMinutes = str("BackendGhostTtlMinutes", "30").toInt();
    m_backend.lanternTtlMinutes = str("BackendLanternTtlMinutes", "10").toInt();
    m_backend.tombCap = str("BackendTombCap", "100").toInt();
    m_backend.summonPoll = str("BackendSummonPoll", "5").toInt();
    m_hosted.enabled = boolean("Hosted", false);
    m_hosted.build = str("HostedBuild", "").trimmed();
    m_hosted.banList = str("HostedBanList", "banned.txt");
    m_hosted.linksPerAddress = str("HostedLinksPerAddress", "8").toInt();
    m_hosted.hellosPerSecond = str("HostedHellosPerSecond", "4").toDouble();
    m_hosted.newAccountsPerHour = str("HostedNewAccountsPerHour", "5").toInt();
    m_hosted.signupWorkBits = str("HostedSignupWorkBits", "20").toInt();
    m_hosted.signupWorkMaxBits = str("HostedSignupWorkMaxBits", "20").toInt();
    m_hosted.signupsPerMinute = str("HostedSignupsPerMinute", "60").toInt();
    m_hosted.datagramsPerSecond = str("HostedDatagramsPerSecond", "1500").toDouble();
    m_hosted.bytesPerSecond = str("HostedBytesPerSecond", "400000").toDouble();
    m_hosted.lobbyPlayers = str("HostedLobbyPlayers", "32").toInt();
    m_hosted.lobbyIntervalMs = str("HostedLobbyIntervalMs", "250").toInt();
    m_hosted.worldsPerPlayer = str("HostedWorldsPerPlayer", "16").toInt();
    m_hosted.partyWorlds = str("HostedPartyWorlds", "2048").toInt();
    m_hosted.stats = boolean("HostedStats", false);
    m_hosted.reports = boolean("HostedReports", false);
    m_hosted.adminSocket = str("HostedAdminSocket", "admin.sock").trimmed();
    m_hosted.retireDays = str("HostedRetireDays", "0").toInt();
    m_httpLinkedOnly = boolean("HttpLinkedOnly", false);
    m_statsPort = str("StatsPort", "31320");
    m_statsPath = str("StatsPath", "stats");
    m_bloodborneWorldPath = str("BloodborneWorldPath", "bloodborne_worlds");
    m_linkKeyFile = str("LinkKeyFile", "link_key.bin").trimmed();
    if (m_linkKeyFile.isEmpty()) // a new key each start would break every game's pin
        m_linkKeyFile = QStringLiteral("link_key.bin");
    m_statsCacheLife = str("StatsCacheLife", "30").toInt();
    m_emailValidated = boolean("EmailValidated", false);
    m_makeAccountsAtSignIn = boolean("MakeAccountsAtSignIn", true);
    m_tcpLocalOnly = boolean("TcpLocalOnly", true);
    m_adminsList = strList("AdminsList");
    m_registrationSecretKey = str("RegistrationSecretKey", "");

    s.sync();

    if (s.status() != QSettings::NoError)
        qWarning() << "QSettings error reading" << path;
    else
        qInfo() << "Config loaded from" << path;

    if (!m_registrationSecretKey.isEmpty())
        qInfo() << "Registration requires secret key";
    else
        qInfo() << "Registration is open (no secret key set)";

    LoadBannedDomains();
}

bool ConfigManager::Load(const QString& path) {
    Parse(path);
    return true;
}

void ConfigManager::Reload(const QString& path) {
    Parse(path.isEmpty() ? m_path : path);
    qInfo() << "Config reloaded";
}
