/*
 *    Copyright 2016 Kai Pastor
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

#ifndef OPENORIENTEERING_UTIL_ENCODING_H
#define OPENORIENTEERING_UTIL_ENCODING_H

#include <optional>

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringView>

namespace OpenOrienteering {

namespace Util {

/** A validated ICU character encoding. */
class TextEncoding final
{
public:
	QByteArray encode(QStringView text) const;
	QString decode(QByteArrayView bytes) const;

	const QByteArray& name() const noexcept { return canonical_name; }
	bool operator==(const TextEncoding&) const = default;

private:
	explicit TextEncoding(QByteArray canonical_name);

	QByteArray canonical_name;

	friend std::optional<TextEncoding> encodingForName(QByteArrayView name);
};

/**
 * Determines the name of the 8-bit legacy codepage for a language.
 * 
 * This function accepts language names as returned by QLocale::name().
 * Characters after the two letter language code are ignored.
 * 
 * If the language is unknown, it returns "Windows-1252".
 * 
 * @param language_name A lowercase, two-letter ISO 639 language code
 * @return A Windows codepage name.
 */
const char* codepageForLanguage(const QString& language_name);

/**
 * Determines the encoding for a given name.
 * 
 * ICU aliases are accepted. If the name is "Default" (case sensitive), the
 * legacy codepage associated with the current locale is used.
 */
std::optional<TextEncoding> encodingForName(QByteArrayView name);

/** Returns the canonical names of all ICU encodings available at runtime. */
QList<QByteArray> availableEncodingNames();


}  // namespace Util

}  // namespace OpenOrienteering

#endif
