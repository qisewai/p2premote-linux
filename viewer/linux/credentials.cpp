#include <libsecret/secret.h>

#include "../qt/credentials.h"
#include <QByteArray>

#include <openssl/crypto.h>

#include <memory>

namespace {

const auto schema = [] {
    SecretSchema value{};
    value.name = "org.p2premote.Desktop";
    value.attributes[0] = {"key", SECRET_SCHEMA_ATTRIBUTE_STRING};
    return value;
}();

using Secret = std::unique_ptr<char, decltype(&secret_password_free)>;

Secret lookup(const QByteArray& key) {
    return Secret(secret_password_lookup_sync(&schema, nullptr, nullptr, "key", key.constData(), nullptr),
        secret_password_free);
}

bool store(const QByteArray& key, const QByteArray& value) {
    return secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, "P2P Remote",
        value.constData(), nullptr, nullptr, "key", key.constData(), nullptr);
}

bool clear(const QByteArray& key) {
    GError* error{};
    secret_password_clear_sync(&schema, nullptr, &error, "key", key.constData(), nullptr);
    if (!error) return true;
    g_error_free(error);
    return false;
}

} // namespace

Credentials::~Credentials() { OPENSSL_cleanse(password.data(), password.size() * sizeof(wchar_t)); }

std::optional<Credentials> SecretStore::readAccount(const QString& server) {
    const auto secret = lookup("account:" + server.toUtf8());
    if (!secret) return {};
    const auto value = QString::fromUtf8(secret.get());
    const auto separator = value.indexOf(u'\n');
    if (separator < 1) return {};
    return Credentials{server, value.left(separator), value.mid(separator + 1).toStdWString(), true};
}

bool SecretStore::saveAccount(const Credentials& credentials) {
    auto secret = credentials.user.toUtf8() + '\n' + QString::fromStdWString(credentials.password).toUtf8();
    const auto saved = store("account:" + credentials.server.toUtf8(), secret);
    OPENSSL_cleanse(secret.data(), secret.size());
    return saved;
}

bool SecretStore::deleteAccount(const QString& server) { return clear("account:" + server.toUtf8()); }

QString SecretStore::readLan(const QString& key) {
    const auto secret = lookup("lan:" + key.toUtf8());
    return secret ? QString::fromUtf8(secret.get()) : QString{};
}

bool SecretStore::saveLan(const QString& key, const QString& value) { return store("lan:" + key.toUtf8(), value.toUtf8()); }
bool SecretStore::deleteLan(const QString& key) { return clear("lan:" + key.toUtf8()); }

void SecretStore::eraseTemporary(QByteArray& secret) { OPENSSL_cleanse(secret.data(), secret.size()); }
