/*
 *    Copyright 2026 The OpenOrienteering developers
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef OPENORIENTEERING_UTIL_LEGACY_CODEC_H
#define OPENORIENTEERING_UTIL_LEGACY_CODEC_H

#include <QByteArray>
#include <QList>
#include <QString>

namespace OpenOrienteering {

/**
 * Lightweight replacement for QTextCodec, supporting only the codecs used
 * by this codebase: Windows-1250 through 1258, UTF-8, UTF-16LE, Latin-1,
 * and System locale.
 *
 * This eliminates the dependency on Qt's Core5Compat module.
 */
class LegacyCodec
{
public:
	/// Returns a codec for the given name, or nullptr if unsupported.
	/// Name matching is case-insensitive.
	static const LegacyCodec* forName(const QByteArray& name);

	/// Returns a codec for the system locale.
	static const LegacyCodec* forLocale();

	/// Returns the list of supported codec names.
	static QList<QByteArray> availableCodecs();

	/// Returns the codec's canonical name.
	QByteArray name() const;

	/// Decodes bytes to QString.
	QString toUnicode(const char* data, int len) const;
	QString toUnicode(const QByteArray& data) const;

	/// Encodes QString to bytes.
	QByteArray fromUnicode(const QString& str) const;

	/// Flags for encoder/decoder behavior.
	enum Flag {
		DefaultConversion = 0,
		ConvertInvalidToNull = 1,
		IgnoreHeader = 2,
	};

	/// Stateful decoder (for flag-dependent decoding).
	class Decoder
	{
	public:
		Decoder(const LegacyCodec* codec, int flags = DefaultConversion);
		QString toUnicode(const char* data, int len);
	private:
		const LegacyCodec* m_codec;
		int m_flags;
		bool m_bom_skipped = false;
	};

	/// Stateful encoder (for flag-dependent encoding).
	class Encoder
	{
	public:
		Encoder(const LegacyCodec* codec, int flags = DefaultConversion);
		QByteArray fromUnicode(const QString& str);
	private:
		const LegacyCodec* m_codec;
		int m_flags;
		bool m_bom_written = false;
	};

	// Internal type tag; public only to allow static initialization in the
	// implementation file.  Use forName() / forLocale() to obtain instances.
	enum Type { WindowsCodepage, Utf8, Utf16LE, Latin1, SystemLocale };
	LegacyCodec(Type type, int codepage_index = -1);

private:
	Type m_type;
	int m_codepage_index;
};

}  // namespace OpenOrienteering

#endif
