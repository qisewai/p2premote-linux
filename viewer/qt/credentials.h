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

std::optional<Credentials> readCredentials(const QString& server);
bool saveCredentials(const Credentials& credentials);
bool deleteCredentials(const QString& server);
