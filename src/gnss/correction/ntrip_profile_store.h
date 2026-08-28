/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_NTRIP_PROFILE_STORE_H
#define OPENORIENTEERING_NTRIP_PROFILE_STORE_H

#include <optional>

#include <QString>
#include <QStringList>

#include "ntrip_profile.h"

namespace OpenOrienteering {

/**
 * Persists non-secret NTRIP profile fields in application preferences.
 *
 * Passwords go to the native credential store on Apple platforms (Keychain).
 * Elsewhere they are kept in application preferences, scrambled — obfuscation
 * rather than encryption; see the implementation for the rationale.
 *
 * Existing prototype profiles are migrated opportunistically: a legacy
 * plaintext password is copied into the password store and removed from its
 * old preferences key after the write succeeds.
 */
class NtripProfileStore
{
public:
	static QStringList profileNames();
	static std::optional<NtripProfile> load(const QString& name,
	                                       QString* error = nullptr);
	static bool save(const NtripProfile& profile, QString* error = nullptr);
	static bool rename(const QString& old_name,
	                   const NtripProfile& profile,
	                   QString* error = nullptr);
	static bool remove(const QString& name, QString* error = nullptr);

private:
	static QString settingsPrefix(const QString& name);
	static QString credentialAccount(const QString& name);
};

}  // namespace OpenOrienteering

#endif
