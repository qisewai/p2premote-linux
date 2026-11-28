#pragma once

#include <QString>

#include <optional>
#include <string>

struct Credentials {
    QString server;
    QString user;
    std::wstring password;
    bool remember{};

    ~Credentials();
};

class SecretStore {
public:
    static std::optional<Credentials> readAccount(const QString& server);
    static bool saveAccount(const Credentials& credentials);
    static bool deleteAccount(const QString& server);
    static QString readLan(const QString& key);
    static bool saveLan(const QString& key, const QString& secret);
    static bool deleteLan(const QString& key);
    static void eraseTemporary(QByteArray& secret);
};
