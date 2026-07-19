/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 */

#include "map_hub_credentials.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#ifdef Q_OS_MACOS
#include <Security/Security.h>
#elif defined(Q_OS_ANDROID)
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <wincred.h>
#endif

namespace OpenOrienteering {

namespace {

constexpr auto service_name = "org.openorienteering.Mapper.MapHub";

QString credentialTr(const char *source) {
  return QCoreApplication::translate("MapHubCredentials", source);
}

QByteArray serverKey(const QString &server_url) {
  auto url = QUrl::fromUserInput(server_url);
  auto credential_scope = url.fragment();
  url.setPath(QString{});
  url.setQuery(QString{});
  url.setFragment(QString{});
  auto normalized =
      url.adjusted(QUrl::RemoveUserInfo | QUrl::StripTrailingSlash)
          .toString(QUrl::FullyEncoded) +
      QLatin1Char('#') + credential_scope;
  return QCryptographicHash::hash(normalized.toUtf8(),
                                  QCryptographicHash::Sha256)
      .toHex();
}

[[maybe_unused]] MapHubCredentials::Result readFallback(const QString &path) {
  QFile file(path);
  if (!file.exists())
    return {};
  const QFileInfo info(file);
  const auto unsafe_permissions =
      QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
      QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
  if (info.size() < 1 || info.size() > 4096 ||
      (info.permissions() & unsafe_permissions))
    return {
        {},
        credentialTr("The local Map Hub credential has an unsafe size or file "
                     "permission mode; reconnect in Settings."),
        true};
  if (!file.open(QIODevice::ReadOnly))
    return {{},
            credentialTr("Cannot read the local Map Hub credential store: %1")
                .arg(file.errorString()),
            true};
  const auto bytes = file.read(4097);
  if (bytes.size() > 4096 || bytes.contains('\0') || bytes.contains('\r') ||
      bytes.contains('\n'))
    return {{}, credentialTr("The local Map Hub credential is invalid."), true};
  auto token = QString::fromUtf8(bytes).trimmed();
  if (token.isEmpty())
    return {{}, credentialTr("The local Map Hub credential is empty."), true};
  return {token, {}, true};
}

[[maybe_unused]] MapHubCredentials::Result writeFallback(const QString &path,
                                                         const QString &token) {
  QDir directory;
  if (!directory.mkpath(QFileInfo(path).absolutePath()))
    return {
        {},
        credentialTr("Cannot create the local Map Hub credential directory."),
        true};
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return {{},
            credentialTr("Cannot write the local Map Hub credential store: %1")
                .arg(file.errorString()),
            true};
  if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
    return {{},
            credentialTr("Cannot secure the local Map Hub credential store: "
                         "%1")
                .arg(file.errorString()),
            true};
  const auto bytes = token.toUtf8();
  if (file.write(bytes) != bytes.size() || !file.commit())
    return {{},
            credentialTr("Cannot commit the local Map Hub credential store: %1")
                .arg(file.errorString()),
            true};
  if (!QFile::setPermissions(path,
                             QFileDevice::ReadOwner | QFileDevice::WriteOwner))
    return {{},
            credentialTr("Cannot verify owner-only permissions on the local "
                         "Map Hub credential store."),
            true};
  return {token, {}, true};
}

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN) && !defined(Q_OS_ANDROID)
QString secretTool() {
  return QStandardPaths::findExecutable(QStringLiteral("secret-tool"));
}

MapHubCredentials::Result readSecretService(const QString &account) {
  auto program = secretTool();
  if (program.isEmpty())
    return {{}, QStringLiteral("unavailable"), false};
  QProcess process;
  process.start(program,
                {QStringLiteral("lookup"), QStringLiteral("application"),
                 QString::fromLatin1(service_name), QStringLiteral("account"),
                 account});
  if (!process.waitForFinished(10000) ||
      process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return {};
  return {
      QString::fromUtf8(process.readAllStandardOutput()).trimmed(), {}, false};
}

MapHubCredentials::Result writeSecretService(const QString &account,
                                             const QString &token) {
  auto program = secretTool();
  if (program.isEmpty())
    return {{}, QStringLiteral("unavailable"), false};
  QProcess process;
  process.start(program,
                {QStringLiteral("store"),
                 QStringLiteral("--label=OpenOrienteering Mapper Map Hub"),
                 QStringLiteral("application"),
                 QString::fromLatin1(service_name), QStringLiteral("account"),
                 account});
  if (!process.waitForStarted(5000))
    return {{}, QStringLiteral("unavailable"), false};
  process.write(token.toUtf8());
  process.closeWriteChannel();
  if (!process.waitForFinished(30000) ||
      process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return {{},
            credentialTr("The desktop secret service rejected the credential."),
            false};
  return {token, {}, false};
}

MapHubCredentials::Result removeSecretService(const QString &account) {
  auto program = secretTool();
  if (program.isEmpty())
    return {{}, QStringLiteral("unavailable"), false};
  QProcess process;
  process.start(program,
                {QStringLiteral("clear"), QStringLiteral("application"),
                 QString::fromLatin1(service_name), QStringLiteral("account"),
                 account});
  if (!process.waitForStarted(5000) || !process.waitForFinished(10000) ||
      process.exitStatus() != QProcess::NormalExit)
    return {{},
            credentialTr("The desktop secret service could not remove the "
                         "credential."),
            false};
  if (process.exitCode() != 0 &&
      !process.readAllStandardError().trimmed().isEmpty())
    return {{},
            credentialTr("The desktop secret service rejected credential "
                         "removal."),
            false};
  return {};
}
#endif

#ifdef Q_OS_MACOS
CFMutableDictionaryRef macQuery(const QByteArray &account) {
  auto *query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                          &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks);
  auto *service = CFStringCreateWithCString(kCFAllocatorDefault, service_name,
                                            kCFStringEncodingUTF8);
  auto *account_string = CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(account.constData()),
      account.size(), kCFStringEncodingUTF8, false);
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account_string);
  CFRelease(service);
  CFRelease(account_string);
  return query;
}

CFDataRef macData(const QByteArray &bytes) {
  return CFDataCreate(kCFAllocatorDefault,
                      reinterpret_cast<const UInt8 *>(bytes.constData()),
                      bytes.size());
}
#endif

} // namespace

QString MapHubCredentials::accountName(const QString &server_url) {
  return QString::fromLatin1(serverKey(server_url));
}

QString MapHubCredentials::workspaceLeaseKey(const QString &server_url,
                                             const QString &workspace_id) {
  auto url = QUrl::fromUserInput(server_url);
  url.setFragment(QStringLiteral("workspace-lease/%1").arg(workspace_id));
  return url.toString(QUrl::FullyEncoded);
}

QString MapHubCredentials::fallbackPath(const QString &server_url) {
  return QDir(QStandardPaths::writableLocation(
                  QStandardPaths::AppConfigLocation))
      .filePath(QStringLiteral("credentials/map-hub-%1.token")
                    .arg(accountName(server_url)));
}

MapHubCredentials::Result
MapHubCredentials::readToken(const QString &server_url) {
  auto account = accountName(server_url).toUtf8();
#ifdef Q_OS_MACOS
  auto *query = macQuery(account);
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
  CFTypeRef result = nullptr;
  auto status = SecItemCopyMatching(query, &result);
  CFRelease(query);
  if (status == errSecItemNotFound)
    return {};
  if (status != errSecSuccess)
    return {{},
            credentialTr(
                "macOS Keychain could not read the Map Hub credential (%1).")
                .arg(status),
            false};
  auto *data = static_cast<CFDataRef>(result);
  QString token =
      QString::fromUtf8(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                        CFDataGetLength(data));
  CFRelease(data);
  return {token, {}, false};
#elif defined(Q_OS_WIN)
  const QString target = QString::fromLatin1(service_name) + QLatin1Char('/') +
                         QString::fromUtf8(account);
  PCREDENTIALW credential = nullptr;
  if (!CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC,
                 0, &credential)) {
    if (GetLastError() == ERROR_NOT_FOUND)
      return {};
    return {{},
            credentialTr("Windows Credential Manager could not read the Map "
                         "Hub credential."),
            false};
  }
  QString token = QString::fromUtf8(
      reinterpret_cast<const char *>(credential->CredentialBlob),
      int(credential->CredentialBlobSize));
  CredFree(credential);
  return {token, {}, false};
#elif defined(Q_OS_ANDROID)
  auto context = QNativeInterface::QAndroidApplication::context();
  auto key = QJniObject::fromString(QString::fromUtf8(account));
  auto value = QJniObject::callStaticObjectMethod(
      "org/openorienteering/mapper/SecureCredentialStore", "read",
      "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;",
      context.object<jobject>(),
      key.object<jstring>());
  QJniEnvironment environment;
  if (environment.checkAndClearExceptions())
    return {
        {},
        credentialTr("Android Keystore could not read the Map Hub credential."),
        false};
  return {value.isValid() ? value.toString() : QString{}, {}, false};
#else
  auto secret = readSecretService(QString::fromUtf8(account));
  if (secret.error != QLatin1String("unavailable"))
    return secret;
  return readFallback(fallbackPath(server_url));
#endif
}

MapHubCredentials::Result
MapHubCredentials::writeToken(const QString &server_url, const QString &token) {
  auto bytes = token.toUtf8();
  if (token.trimmed().isEmpty())
    return {{}, credentialTr("The Map Hub token is empty."), false};
  if (bytes.size() > 4096 || bytes.contains('\0') || bytes.contains('\r') ||
      bytes.contains('\n'))
    return {{}, credentialTr("The Map Hub token is invalid."), false};
  auto account = accountName(server_url).toUtf8();
#ifdef Q_OS_MACOS
  auto *query = macQuery(account);
  auto *data = macData(bytes);
  auto *update = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                           &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(update, kSecValueData, data);
  auto status = SecItemUpdate(query, update);
  CFRelease(update);
  if (status == errSecItemNotFound) {
    CFDictionarySetValue(query, kSecValueData, data);
    status = SecItemAdd(query, nullptr);
  }
  CFRelease(data);
  CFRelease(query);
  if (status != errSecSuccess)
    return {{},
            credentialTr(
                "macOS Keychain could not store the Map Hub credential (%1).")
                .arg(status),
            false};
  return {token, {}, false};
#elif defined(Q_OS_WIN)
  QString target = QString::fromLatin1(service_name) + QLatin1Char('/') +
                   QString::fromUtf8(account);
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = reinterpret_cast<LPWSTR>(target.data());
  credential.CredentialBlobSize = DWORD(bytes.size());
  credential.CredentialBlob = reinterpret_cast<LPBYTE>(bytes.data());
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
  if (!CredWriteW(&credential, 0))
    return {{},
            credentialTr("Windows Credential Manager could not store the Map "
                         "Hub credential."),
            false};
  return {token, {}, false};
#elif defined(Q_OS_ANDROID)
  auto context = QNativeInterface::QAndroidApplication::context();
  auto key = QJniObject::fromString(QString::fromUtf8(account));
  auto value = QJniObject::fromString(token);
  auto stored = QJniObject::callStaticMethod<jboolean>(
      "org/openorienteering/mapper/SecureCredentialStore", "write",
      "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z",
      context.object<jobject>(),
      key.object<jstring>(), value.object<jstring>());
  QJniEnvironment environment;
  if (!stored || environment.checkAndClearExceptions())
    return {{},
            credentialTr(
                "Android Keystore could not store the Map Hub credential."),
            false};
  return {token, {}, false};
#else
  auto secret = writeSecretService(QString::fromUtf8(account), token);
  if (secret.error != QLatin1String("unavailable")) {
    if (secret && QFileInfo::exists(fallbackPath(server_url)) &&
        !QFile::remove(fallbackPath(server_url)))
      return {{},
              credentialTr("The token is in the desktop secret service, but "
                           "an older local fallback could not be removed."),
              true};
    return secret;
  }
  return writeFallback(fallbackPath(server_url), token);
#endif
}

MapHubCredentials::Result
MapHubCredentials::removeToken(const QString &server_url) {
  auto account = accountName(server_url).toUtf8();
#ifdef Q_OS_MACOS
  auto *query = macQuery(account);
  auto status = SecItemDelete(query);
  CFRelease(query);
  if (status != errSecSuccess && status != errSecItemNotFound)
    return {{},
            credentialTr(
                "macOS Keychain could not remove the Map Hub credential (%1).")
                .arg(status),
            false};
#elif defined(Q_OS_WIN)
  const QString target = QString::fromLatin1(service_name) + QLatin1Char('/') +
                         QString::fromUtf8(account);
  if (!CredDeleteW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC,
                   0) &&
      GetLastError() != ERROR_NOT_FOUND)
    return {{},
            credentialTr("Windows Credential Manager could not remove the Map "
                         "Hub credential."),
            false};
#elif defined(Q_OS_ANDROID)
  {
    auto context = QNativeInterface::QAndroidApplication::context();
    auto key = QJniObject::fromString(QString::fromUtf8(account));
    auto removed = QJniObject::callStaticMethod<jboolean>(
        "org/openorienteering/mapper/SecureCredentialStore", "remove",
        "(Landroid/content/Context;Ljava/lang/String;)Z",
        context.object<jobject>(),
        key.object<jstring>());
    QJniEnvironment environment;
    if (!removed || environment.checkAndClearExceptions())
      return {{},
              credentialTr(
                  "Android Keystore could not remove the Map Hub credential."),
              false};
  }
#else
  auto removed = removeSecretService(QString::fromUtf8(account));
  if (removed.error != QLatin1String("unavailable") && !removed)
    return removed;
#endif
  auto fallback = fallbackPath(server_url);
  if (QFileInfo::exists(fallback) && !QFile::remove(fallback))
    return {{},
            credentialTr("The local fallback credential could not be removed."),
            true};
  return {};
}

} // namespace OpenOrienteering
