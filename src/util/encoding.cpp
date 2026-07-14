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

#include "encoding.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include <QByteArray>
#include <QLocale>
#include <QString>
#include <QStringView>

#include <unicode/ucnv.h>


namespace {

struct ConverterDeleter
{
	void operator()(UConverter* converter) const noexcept
	{
		ucnv_close(converter);
	}
};

using Converter = std::unique_ptr<UConverter, ConverterDeleter>;

Converter openConverter(QByteArrayView name)
{
	const QByteArray null_terminated_name{name};
	UErrorCode status = U_ZERO_ERROR;
	Converter converter{ucnv_open(null_terminated_name.constData(), &status)};
	if (U_FAILURE(status))
		return {};
	return converter;
}

int32_t checkedSize(qsizetype size)
{
	if (size > std::numeric_limits<int32_t>::max())
		throw std::length_error{"Text is too large for ICU conversion"};
	return static_cast<int32_t>(size);
}

struct LanguageMapping
{
	const char* languages;
	const char* codepage;
};

// Languages are listed as two letters, separated by single char
const LanguageMapping mappings[] = {
	{ "cs hu pl", "Windows-1250" }, // Central European
	{ "bg ru uk", "Windows-1251" }, // Cyrillic
	{ "et lt lv", "Windows-1257" }, // Baltic
	{ "el", "Windows-1253" }, // Greek
	{ "he", "Windows-1255" }, // Hebrew
#ifdef ENABLE_ALL_LANGUAGE_MAPPINGS
	{ "da de en es fi fr id it nb nl pt sv", "Windows-1252" }, // Western European (default)
	{ "tr", "Windows-1254" }, // Turkish
	{ "ar", "Windows-1256" }, // Arabic
	{ "vi", "Windows-1258" }, // Vietnamese
#endif
};

}  // namespace



namespace OpenOrienteering {

const char* Util::codepageForLanguage(const QString& language_name)
{
	const auto language = QStringView{language_name}.first(2).toLatin1();
	for (const auto& mapping : mappings)
	{
		auto len = qstrlen(mapping.languages);
		for (decltype(len) i = 0; i < len; i += 3)
		{
			if (language == QByteArray::fromRawData(mapping.languages+i, 2))
				return mapping.codepage;
		}
	}
	return "Windows-1252";
}


Util::TextEncoding::TextEncoding(QByteArray canonical_name)
 : canonical_name{std::move(canonical_name)}
{}

QByteArray Util::TextEncoding::encode(QStringView text) const
{
	auto converter = openConverter(canonical_name);
	if (!converter)
		return {};

	UErrorCode status = U_ZERO_ERROR;
	ucnv_setFallback(converter.get(), false);
	ucnv_setSubstChars(converter.get(), "?", 1, &status);
	if (U_FAILURE(status))
		return {};

	const auto source_size = checkedSize(text.size());
	const auto* source = reinterpret_cast<const UChar*>(text.utf16());
	status = U_ZERO_ERROR;
	const auto output_size = ucnv_fromUChars(converter.get(), nullptr, 0, source, source_size, &status);
	if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))
		return {};

	QByteArray output{output_size, Qt::Uninitialized};
	status = U_ZERO_ERROR;
	ucnv_fromUChars(converter.get(), output.data(), output.size(), source, source_size, &status);
	return U_SUCCESS(status) ? output : QByteArray{};
}

QString Util::TextEncoding::decode(QByteArrayView bytes) const
{
	auto converter = openConverter(canonical_name);
	if (!converter)
		return {};

	const auto source_size = checkedSize(bytes.size());
	UErrorCode status = U_ZERO_ERROR;
	const auto output_size = ucnv_toUChars(converter.get(), nullptr, 0, bytes.data(), source_size, &status);
	if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))
		return {};

	QString output{output_size, Qt::Uninitialized};
	status = U_ZERO_ERROR;
	ucnv_toUChars(
	  converter.get(), reinterpret_cast<UChar*>(output.data()), output.size(),
	  bytes.data(), source_size, &status);
	return U_SUCCESS(status) ? output : QString{};
}

std::optional<Util::TextEncoding> Util::encodingForName(QByteArrayView name)
{
	QByteArray resolved_name{name};
	if (name == QByteArrayView{"Default"})
		resolved_name = codepageForLanguage(QLocale{}.name());

	auto converter = openConverter(resolved_name);
	if (!converter)
		return std::nullopt;

	UErrorCode status = U_ZERO_ERROR;
	const auto* canonical_name = ucnv_getName(converter.get(), &status);
	if (U_FAILURE(status) || !canonical_name)
		return std::nullopt;
	return TextEncoding{QByteArray{canonical_name}};
}

QList<QByteArray> Util::availableEncodingNames()
{
	QList<QByteArray> names;
	const auto count = ucnv_countAvailable();
	names.reserve(count);
	for (int32_t i = 0; i < count; ++i)
		names.emplaceBack(ucnv_getAvailableName(i));
	return names;
}


}  // namespace OpenOrienteering
