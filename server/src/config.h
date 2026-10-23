// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QReadLocker>
#include <QReadWriteLock>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWriteLocker>

// The game's own online services inside shadNet (bloodborne_backend_server.h).
struct BackendConfig {
    bool enabled = true;
    quint16 port = 20443;
    QString address;                               // empty: the address each game connects to
    QString folder = QStringLiteral("../backend"); // notice.txt, broadcast.txt, login_message.txt
    QString store;                                 // the content store's SQLite file; empty: none
    QString showOwn;                               // deaths, ghosts, lanterns or all
    int ghostTtlMinutes = 30;
    int lanternTtlMinutes = 10;
    int tombCap = 100;
    int summonPoll = 5;
};

class ConfigManager {
public:
    bool Load(const QString& path = "shadnet.cfg");
    void Reload(const QString& path = "shadnet.cfg");

    QString GetHost() const {
        QReadLocker lk(&m_lock);
        return m_host;
    }
    QString GetUnsecuredPort() const {
        QReadLocker lk(&m_lock);
        return m_unsecured_port;
    }
    QString GetMatchingUdpPort() const {
        QReadLocker lk(&m_lock);
        return m_matchingUdpPort;
    }
    QString GetWebApiPort() const {
        QReadLocker lk(&m_lock);
        return m_webapiPort;
    }
    bool IsStatsEnabled() const {
        QReadLocker lk(&m_lock);
        return m_statsEnabled;
    }
    QString GetStatsPort() const {
        QReadLocker lk(&m_lock);
        return m_statsPort;
    }
    // Where the parties' worlds are kept (bloodborne_world.h): one folder per party.
    QString GetBloodborneWorldPath() const {
        QReadLocker lk(&m_lock);
        return m_bloodborneWorldPath;
    }
    QString GetStatsPath() const {
        QReadLocker lk(&m_lock);
        return m_statsPath;
    }
    int GetStatsCacheLife() const {
        QReadLocker lk(&m_lock);
        return m_statsCacheLife;
    }
    bool IsMatching2Enabled() const {
        QReadLocker lk(&m_lock);
        return m_matching2Enabled;
    }
    bool IsBloodborneSeamlessCoopEnabled() const {
        QReadLocker lk(&m_lock);
        return m_bloodborneSeamlessCoop;
    }
    // Build check on join: whether a game must run this server's own release (CheckServerBuild,
    // on by default; off for a dev or rig server built apart from the games).
    bool IsServerBuildChecked() const {
        QReadLocker lk(&m_lock);
        return m_checkServerBuild;
    }

    BackendConfig GetBackend() const {
        QReadLocker lk(&m_lock);
        return m_backend;
    }

    bool IsEmailValidated() const {
        QReadLocker lk(&m_lock);
        return m_emailValidated;
    }
    // Phase 11: a player id never seen before gets its account at its first sign-in (no
    // registering; the launcher makes the id).
    bool IsMakingAccountsAtSignIn() const {
        QReadLocker lk(&m_lock);
        return m_makeAccountsAtSignIn;
    }
    // Phase 11, one port: the TCP port listens on 127.0.0.1 only (the games' calls come through
    // the UDP link's Rpc tunnel). Off: on Host, for games that connect to it themselves.
    bool IsTcpLocalOnly() const {
        QReadLocker lk(&m_lock);
        return m_tcpLocalOnly;
    }
    bool IsBannedDomain(const QString& d) const {
        QReadLocker lk(&m_lock);
        return m_bannedDomains.contains(d.toLower());
    }
    bool IsAdmin(const QString& npid) const {
        QReadLocker lk(&m_lock);
        return m_adminsList.contains(npid);
    }

    // Returns true if registration is allowed for the given secret_key.
    // When RegistrationSecretKey is empty, all registrations are allowed.
    // When set, only requests carrying the matching key are accepted.
    bool IsRegistrationAllowed(const QString& key) const {
        QReadLocker lk(&m_lock);
        return m_registrationSecretKey.isEmpty() ||
               (!key.isEmpty() && key == m_registrationSecretKey);
    }

    void SetHost(const QString& v) {
        QWriteLocker lk(&m_lock);
        m_host = v;
    }
    void SetUnsecuredPort(const QString& v) {
        QWriteLocker lk(&m_lock);
        m_unsecured_port = v;
    }
    void SetEmailValidated(bool v) {
        QWriteLocker lk(&m_lock);
        m_emailValidated = v;
    }
    void SetAdminsList(const QStringList& v) {
        QWriteLocker lk(&m_lock);
        m_adminsList = v;
    }
    void LoadBannedDomains();

private:
    void Parse(const QString& path);

    mutable QReadWriteLock m_lock;
    QString m_path;

    // config values
    QString m_host = "0.0.0.0";
    QString m_unsecured_port = "31313";
    QString m_matchingUdpPort = "31314";
    QString m_webapiPort = "31315";
    bool m_statsEnabled = true;
    bool m_matching2Enabled = true;
    bool m_bloodborneSeamlessCoop = false;
    bool m_checkServerBuild = true;
    BackendConfig m_backend;
    QString m_statsPort = "31320";
    QString m_statsPath = "stats";
    QString m_bloodborneWorldPath = "bloodborne_worlds";
    int m_statsCacheLife = 30; // seconds the stats JSON is cached before recompute
    bool m_emailValidated = false;
    bool m_makeAccountsAtSignIn = true;
    bool m_tcpLocalOnly = true;
    QStringList m_adminsList;
    QSet<QString> m_bannedDomains;
    // When non-empty, registrations must supply this key or they are rejected.
    QString m_registrationSecretKey;
};
