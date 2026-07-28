/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "ntrip_profile_store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSettings>

#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
#  include <Security/Security.h>
#  include <TargetConditionals.h>
#endif

namespace OpenOrienteering {

namespace {

constexpr auto keychain_service = "org.openorienteering.Mapper.NTRIP";

QString storeError(const char* message)
{
	return QCoreApplication::translate("NtripProfileStore", message);
}

#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
CFMutableDictionaryRef keychainQuery(const QString& account)
{
	auto* query = CFDictionaryCreateMutable(
	  kCFAllocatorDefault, 0,
	  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	auto* service = CFStringCreateWithCString(
	  kCFAllocatorDefault, keychain_service, kCFStringEncodingUTF8);
	auto account_bytes = account.toUtf8();
	auto* account_string = CFStringCreateWithBytes(
	  kCFAllocatorDefault,
	  reinterpret_cast<const UInt8*>(account_bytes.constData()),
	  account_bytes.size(), kCFStringEncodingUTF8, false);
	CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(query, kSecAttrService, service);
	CFDictionarySetValue(query, kSecAttrAccount, account_string);
	CFRelease(service);
	CFRelease(account_string);
	return query;
}

bool readPassword(const QString& account, QString& password, QString* error)
{
	auto* query = keychainQuery(account);
	CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
	CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
	CFTypeRef result = nullptr;
	auto status = SecItemCopyMatching(query, &result);
	CFRelease(query);
	if (status == errSecItemNotFound)
		return true;
	if (status != errSecSuccess)
	{
		if (error)
			*error = storeError("Keychain could not read this NTRIP password (%1).")
			           .arg(status);
		return false;
	}
	auto* data = static_cast<CFDataRef>(result);
	password = QString::fromUtf8(
	  reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
	  CFDataGetLength(data));
	CFRelease(data);
	return true;
}

bool writePassword(const QString& account, const QString& password, QString* error)
{
	if (password.isEmpty())
	{
		auto* query = keychainQuery(account);
		auto status = SecItemDelete(query);
		CFRelease(query);
		if (status == errSecSuccess || status == errSecItemNotFound)
			return true;
		if (error)
			*error = storeError("Keychain could not remove this NTRIP password (%1).")
			           .arg(status);
		return false;
	}

	auto bytes = password.toUtf8();
	auto* data = CFDataCreate(
	  kCFAllocatorDefault,
	  reinterpret_cast<const UInt8*>(bytes.constData()), bytes.size());
	auto* query = keychainQuery(account);
	auto* update = CFDictionaryCreateMutable(
	  kCFAllocatorDefault, 0,
	  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(update, kSecValueData, data);
	auto status = SecItemUpdate(query, update);
	CFRelease(update);
	if (status == errSecItemNotFound)
	{
		CFDictionarySetValue(query, kSecValueData, data);
#if TARGET_OS_IPHONE
		CFDictionarySetValue(query, kSecAttrAccessible,
		                     kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
#endif
		status = SecItemAdd(query, nullptr);
	}
	CFRelease(query);
	CFRelease(data);
	if (status == errSecSuccess)
		return true;
	if (error)
		*error = storeError("Keychain could not store this NTRIP password (%1).")
		           .arg(status);
	return false;
}
#else
bool readPassword(const QString&, QString&, QString*)
{
	return true;
}

bool writePassword(const QString&, const QString& password, QString* error)
{
	if (password.isEmpty())
		return true;
	if (error)
		*error = storeError(
		  "Secure NTRIP password storage is unavailable on this platform.");
	return false;
}
#endif

}  // namespace

QStringList NtripProfileStore::profileNames()
{
	auto names = QSettings().value(
	  QStringLiteral("Gnss/ntrip_profiles")).toStringList();
	names.removeAll(QString{});
	names.removeDuplicates();
	return names;
}

QString NtripProfileStore::settingsPrefix(const QString& name)
{
	return QStringLiteral("Gnss/ntrip_profile/%1/").arg(name);
}

QString NtripProfileStore::credentialAccount(const QString& name)
{
	return QString::fromLatin1(QCryptographicHash::hash(
	  name.normalized(QString::NormalizationForm_C).toUtf8(),
	  QCryptographicHash::Sha256).toHex());
}

std::optional<NtripProfile> NtripProfileStore::load(
  const QString& name, QString* error)
{
	auto clean_name = name.trimmed();
	if (clean_name.isEmpty() || !profileNames().contains(clean_name))
	{
		if (error)
			*error = storeError("The selected NTRIP profile no longer exists.");
		return std::nullopt;
	}

	QSettings settings;
	auto prefix = settingsPrefix(clean_name);
	NtripProfile profile;
	profile.name = clean_name;
	profile.casterHost = settings.value(
	  prefix + QStringLiteral("host")).toString();
	profile.casterPort = static_cast<quint16>(settings.value(
	  prefix + QStringLiteral("port"), 2101).toUInt());
	profile.mountpoint = settings.value(
	  prefix + QStringLiteral("mountpoint")).toString();
	profile.username = settings.value(
	  prefix + QStringLiteral("username")).toString();
	profile.version = static_cast<NtripVersion>(settings.value(
	  prefix + QStringLiteral("version"),
	  static_cast<int>(NtripVersion::Auto)).toInt());
	profile.useTls = settings.value(
	  prefix + QStringLiteral("tls"), false).toBool();
	profile.sendGga = settings.value(
	  prefix + QStringLiteral("send_gga"), true).toBool();
	profile.sendEmptyBasicAuth = settings.value(
	  prefix + QStringLiteral("empty_basic_auth"), false).toBool();
	profile.ggaIntervalSec = settings.value(
	  prefix + QStringLiteral("gga_interval"), 10).toInt();

	QString password_error;
	if (!readPassword(credentialAccount(clean_name),
	                  profile.password, &password_error))
	{
		if (error)
			*error = password_error;
		return std::nullopt;
	}

	// One-way migration from the research prototype's plaintext preference.
	auto legacy_key = prefix + QStringLiteral("password");
	auto legacy_password = settings.value(legacy_key).toString();
	if (profile.password.isEmpty() && !legacy_password.isEmpty())
	{
		if (writePassword(credentialAccount(clean_name),
		                  legacy_password, &password_error))
		{
			profile.password = legacy_password;
			settings.remove(legacy_key);
			settings.sync();
		}
		else if (error)
		{
			*error = password_error;
			return std::nullopt;
		}
	}

	return ntripProfileNormalized(profile);
}

bool NtripProfileStore::save(const NtripProfile& input, QString* error)
{
	auto profile = ntripProfileNormalized(input);
	if (profile.name.isEmpty() || profile.name.contains(QLatin1Char('/'))
	    || profile.casterHost.isEmpty() || profile.mountpoint.isEmpty())
	{
		if (error)
			*error = storeError(
			  "Enter a profile name, caster host, and mountpoint.");
		return false;
	}

	if (!writePassword(credentialAccount(profile.name),
	                   profile.password, error))
		return false;

	QSettings settings;
	auto prefix = settingsPrefix(profile.name);
	settings.setValue(prefix + QStringLiteral("host"), profile.casterHost);
	settings.setValue(prefix + QStringLiteral("port"), profile.casterPort);
	settings.setValue(prefix + QStringLiteral("mountpoint"), profile.mountpoint);
	settings.setValue(prefix + QStringLiteral("username"), profile.username);
	settings.setValue(prefix + QStringLiteral("version"),
	                  static_cast<int>(profile.version));
	settings.setValue(prefix + QStringLiteral("tls"), profile.useTls);
	settings.setValue(prefix + QStringLiteral("send_gga"), profile.sendGga);
	settings.setValue(prefix + QStringLiteral("empty_basic_auth"),
	                  profile.sendEmptyBasicAuth);
	settings.setValue(prefix + QStringLiteral("gga_interval"),
	                  profile.ggaIntervalSec);
	settings.remove(prefix + QStringLiteral("password"));

	auto names = profileNames();
	if (!names.contains(profile.name))
		names.append(profile.name);
	settings.setValue(QStringLiteral("Gnss/ntrip_profiles"), names);
	settings.sync();
	if (settings.status() == QSettings::NoError)
		return true;
	if (error)
		*error = storeError("Mapper could not save this NTRIP profile.");
	return false;
}

bool NtripProfileStore::remove(const QString& name, QString* error)
{
	if (!writePassword(credentialAccount(name), QString{}, error))
		return false;
	QSettings settings;
	settings.remove(QStringLiteral("Gnss/ntrip_profile/%1").arg(name));
	auto names = profileNames();
	names.removeAll(name);
	settings.setValue(QStringLiteral("Gnss/ntrip_profiles"), names);
	settings.sync();
	if (settings.status() == QSettings::NoError)
		return true;
	if (error)
		*error = storeError("Mapper could not remove this NTRIP profile.");
	return false;
}

}  // namespace OpenOrienteering
