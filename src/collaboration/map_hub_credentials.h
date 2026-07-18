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

#ifndef OPENORIENTEERING_MAP_HUB_CREDENTIALS_H
#define OPENORIENTEERING_MAP_HUB_CREDENTIALS_H

#include <QString>

namespace OpenOrienteering {

/**
 * Stores Map Hub bearer credentials outside QSettings and map documents.
 *
 * macOS and Windows use their native credential stores; Android encrypts with
 * a non-exportable Keystore key. Unix desktops use Secret Service through
 * secret-tool when available, with an owner-only file as a deliberately
 * visible fallback for minimal systems. The fallback is never placed next to
 * an .omap file.
 */
class MapHubCredentials {
public:
  struct Result {
    QString token;
    QString error;
    bool used_fallback = false;

    explicit operator bool() const { return error.isEmpty(); }
  };

  static Result readToken(const QString &server_url);
  static Result writeToken(const QString &server_url, const QString &token);
  static Result removeToken(const QString &server_url);

  /** Stable, non-secret account name used by native credential stores. */
  static QString accountName(const QString &server_url);
  /** Credential namespace for an editing lease, kept separate from the API
   * token. */
  static QString workspaceLeaseKey(const QString &server_url,
                                   const QString &workspace_id);

private:
  static QString fallbackPath(const QString &server_url);
};

} // namespace OpenOrienteering

#endif
