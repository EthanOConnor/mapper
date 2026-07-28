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
 * Persists non-secret NTRIP profile fields in application preferences while
 * keeping passwords in the native credential store.
 *
 * Existing prototype profiles are migrated opportunistically: a legacy
 * plaintext password is copied into Keychain and removed from QSettings after
 * the secure write succeeds.
 */
class NtripProfileStore
{
public:
	static QStringList profileNames();
	static std::optional<NtripProfile> load(const QString& name,
	                                       QString* error = nullptr);
	static bool save(const NtripProfile& profile, QString* error = nullptr);
	static bool remove(const QString& name, QString* error = nullptr);

private:
	static QString settingsPrefix(const QString& name);
	static QString credentialAccount(const QString& name);
};

}  // namespace OpenOrienteering

#endif
