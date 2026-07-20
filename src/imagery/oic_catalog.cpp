/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    any later version.
 */

#include "imagery/oic_catalog.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

namespace OpenOrienteering::imagery {

namespace {

constexpr auto catalog_format = "org.openorienteering.imagery-catalog";
constexpr auto web_mercator_quad_host = "www.opengis.net";
constexpr auto web_mercator_quad_path =
	"/def/tilematrixset/OGC/1.0/WebMercatorQuad";
constexpr double maximum_exact_integer = 9007199254740991.0;

bool isValidUtf8(const QByteArray& bytes)
{
	auto const* data = reinterpret_cast<const unsigned char*>(bytes.constData());
	for (qsizetype index = 0; index < bytes.size(); ++index)
	{
		auto const first = data[index];
		if (first <= 0x7f)
			continue;

		int continuation_count = 0;
		uint codepoint = 0;
		if (first >= 0xc2 && first <= 0xdf)
		{
			continuation_count = 1;
			codepoint = first & 0x1f;
		}
		else if (first >= 0xe0 && first <= 0xef)
		{
			continuation_count = 2;
			codepoint = first & 0x0f;
		}
		else if (first >= 0xf0 && first <= 0xf4)
		{
			continuation_count = 3;
			codepoint = first & 0x07;
		}
		else
		{
			return false;
		}

		if (index + continuation_count >= bytes.size())
			return false;
		for (int offset = 0; offset < continuation_count; ++offset)
		{
			auto const next = data[++index];
			if ((next & 0xc0) != 0x80)
				return false;
			codepoint = (codepoint << 6) | (next & 0x3f);
		}
		if ((continuation_count == 1 && codepoint < 0x80)
		    || (continuation_count == 2 && codepoint < 0x800)
		    || (continuation_count == 3 && codepoint < 0x10000)
		    || codepoint > 0x10ffff
		    || (codepoint >= 0xd800 && codepoint <= 0xdfff))
		{
			return false;
		}
	}
	return true;
}

class JsonPreflight
{
public:
	explicit JsonPreflight(const QByteArray& input)
	: input(input)
	{}

	bool validate()
	{
		if (!isValidUtf8(input))
			return fail(QStringLiteral("Catalog is not valid UTF-8"));
		skipSpace();
		if (!parseValue(1))
			return false;
		skipSpace();
		if (position != input.size())
			return fail(QStringLiteral("Unexpected data after the JSON document"));
		return true;
	}

	QString errorString() const { return error; }

private:
	bool parseValue(int depth)
	{
		if (depth > OicCatalogReader::maximum_nesting_depth)
		{
			return fail(QStringLiteral("JSON nesting exceeds %1 levels")
			            .arg(OicCatalogReader::maximum_nesting_depth));
		}
		if (position >= input.size())
			return fail(QStringLiteral("Unexpected end of JSON input"));

		switch (input.at(position))
		{
		case '{': return parseObject(depth);
		case '[': return parseArray(depth);
		case '"': return parseString(nullptr);
		case 't': return parseLiteral("true");
		case 'f': return parseLiteral("false");
		case 'n': return parseLiteral("null");
		default: return parseNumber();
		}
	}

	bool parseObject(int depth)
	{
		++position;
		skipSpace();
		QSet<QString> keys;
		if (consume('}'))
			return true;
		while (position < input.size())
		{
			QString key;
			if (!parseString(&key))
				return false;
			if (keys.contains(key))
				return fail(QStringLiteral("Duplicate JSON object member: %1").arg(key));
			keys.insert(key);
			skipSpace();
			if (!consume(':'))
				return fail(QStringLiteral("Expected ':' after an object member name"));
			skipSpace();
			if (!parseValue(depth + 1))
				return false;
			skipSpace();
			if (consume('}'))
				return true;
			if (!consume(','))
				return fail(QStringLiteral("Expected ',' or '}' in an object"));
			skipSpace();
		}
		return fail(QStringLiteral("Unterminated JSON object"));
	}

	bool parseArray(int depth)
	{
		++position;
		skipSpace();
		if (consume(']'))
			return true;
		while (position < input.size())
		{
			if (!parseValue(depth + 1))
				return false;
			skipSpace();
			if (consume(']'))
				return true;
			if (!consume(','))
				return fail(QStringLiteral("Expected ',' or ']' in an array"));
			skipSpace();
		}
		return fail(QStringLiteral("Unterminated JSON array"));
	}

	bool parseString(QString* output)
	{
		if (!consume('"'))
			return fail(QStringLiteral("Expected a JSON string"));
		QString decoded;
		while (position < input.size())
		{
			auto const byte = static_cast<unsigned char>(input.at(position++));
			if (byte == '"')
			{
				if (decoded.size() > OicCatalogReader::maximum_string_length)
				{
					return fail(QStringLiteral("JSON string exceeds %1 characters")
					            .arg(OicCatalogReader::maximum_string_length));
				}
				if (output)
					*output = decoded;
				return true;
			}
			if (byte < 0x20)
				return fail(QStringLiteral("Unescaped control character in JSON string"));
			if (byte == '\\')
			{
				if (position >= input.size())
					return fail(QStringLiteral("Unterminated JSON escape"));
				auto const escaped = input.at(position++);
				switch (escaped)
				{
				case '"': decoded.append(QLatin1Char('"')); break;
				case '\\': decoded.append(QLatin1Char('\\')); break;
				case '/': decoded.append(QLatin1Char('/')); break;
				case 'b': decoded.append(QLatin1Char('\b')); break;
				case 'f': decoded.append(QLatin1Char('\f')); break;
				case 'n': decoded.append(QLatin1Char('\n')); break;
				case 'r': decoded.append(QLatin1Char('\r')); break;
				case 't': decoded.append(QLatin1Char('\t')); break;
				case 'u':
				{
					uint codepoint = 0;
					if (!parseHexQuad(&codepoint))
						return false;
					if (codepoint >= 0xd800 && codepoint <= 0xdbff)
					{
						if (position + 2 > input.size()
						    || input.at(position) != '\\'
						    || input.at(position + 1) != 'u')
						{
							return fail(QStringLiteral("Unpaired high surrogate in JSON string"));
						}
						position += 2;
						uint low = 0;
						if (!parseHexQuad(&low))
							return false;
						if (low < 0xdc00 || low > 0xdfff)
							return fail(QStringLiteral("Invalid low surrogate in JSON string"));
						codepoint = 0x10000
						            + ((codepoint - 0xd800) << 10)
						            + (low - 0xdc00);
					}
					else if (codepoint >= 0xdc00 && codepoint <= 0xdfff)
					{
						return fail(QStringLiteral("Unpaired low surrogate in JSON string"));
					}
					auto const scalar = char32_t(codepoint);
					decoded.append(QString::fromUcs4(&scalar, 1));
					break;
				}
				default:
					return fail(QStringLiteral("Invalid JSON escape"));
				}
			}
			else if (byte < 0x80)
			{
				decoded.append(QChar(ushort(byte)));
			}
			else
			{
				auto const start = position - 1;
				auto const length = byte < 0xe0 ? 2 : (byte < 0xf0 ? 3 : 4);
				position = start + length;
				decoded.append(QString::fromUtf8(input.constData() + start, length));
			}
		}
		return fail(QStringLiteral("Unterminated JSON string"));
	}

	bool parseHexQuad(uint* value)
	{
		if (position + 4 > input.size())
			return fail(QStringLiteral("Incomplete Unicode escape"));
		uint result = 0;
		for (int index = 0; index < 4; ++index)
		{
			auto const character = input.at(position++);
			result <<= 4;
			if (character >= '0' && character <= '9')
				result += uint(character - '0');
			else if (character >= 'a' && character <= 'f')
				result += uint(character - 'a' + 10);
			else if (character >= 'A' && character <= 'F')
				result += uint(character - 'A' + 10);
			else
				return fail(QStringLiteral("Invalid Unicode escape"));
		}
		*value = result;
		return true;
	}

	bool parseNumber()
	{
		auto const start = position;
		auto const negative = consume('-');
		if (negative && position >= input.size())
			return fail(QStringLiteral("Incomplete JSON number"));
		if (consume('0'))
		{
			if (position < input.size()
			    && input.at(position) >= '0' && input.at(position) <= '9')
			{
				return fail(QStringLiteral("Leading zero in JSON number"));
			}
		}
		else
		{
			if (position >= input.size()
			    || input.at(position) < '1' || input.at(position) > '9')
			{
				return fail(QStringLiteral("Invalid JSON value"));
			}
			while (position < input.size()
			       && input.at(position) >= '0' && input.at(position) <= '9')
			{
				++position;
			}
		}
		if (consume('.'))
		{
			if (position >= input.size()
			    || input.at(position) < '0' || input.at(position) > '9')
			{
				return fail(QStringLiteral("Missing fraction digits in JSON number"));
			}
			while (position < input.size()
			       && input.at(position) >= '0' && input.at(position) <= '9')
			{
				++position;
			}
		}
		if (position < input.size()
		    && (input.at(position) == 'e' || input.at(position) == 'E'))
		{
			++position;
			if (position < input.size()
			    && (input.at(position) == '+' || input.at(position) == '-'))
			{
				++position;
			}
			if (position >= input.size()
			    || input.at(position) < '0' || input.at(position) > '9')
			{
				return fail(QStringLiteral("Missing exponent digits in JSON number"));
			}
			while (position < input.size()
			       && input.at(position) >= '0' && input.at(position) <= '9')
			{
				++position;
			}
		}

		bool ok = false;
		auto const token = QString::fromLatin1(input.mid(start, position - start));
		auto const value = QLocale::c().toDouble(token, &ok);
		if (!ok || !std::isfinite(value))
			return fail(QStringLiteral("JSON number is outside the finite IEEE 754 range"));
		if (negative && value == 0)
			return fail(QStringLiteral("Negative zero is not permitted in OIC JSON"));
		if (std::floor(value) == value && std::abs(value) > maximum_exact_integer)
		{
			return fail(QStringLiteral("JSON integer exceeds the exact IEEE 754 range"));
		}
		return true;
	}

	bool parseLiteral(const char* literal)
	{
		auto const length = qsizetype(qstrlen(literal));
		if (input.mid(position, length) != literal)
			return fail(QStringLiteral("Invalid JSON literal"));
		position += length;
		return true;
	}

	void skipSpace()
	{
		while (position < input.size())
		{
			auto const character = input.at(position);
			if (character != ' ' && character != '\t'
			    && character != '\r' && character != '\n')
			{
				break;
			}
			++position;
		}
	}

	bool consume(char character)
	{
		if (position < input.size() && input.at(position) == character)
		{
			++position;
			return true;
		}
		return false;
	}

	bool fail(const QString& message)
	{
		error = QStringLiteral("%1 at byte %2").arg(message).arg(position);
		return false;
	}

	const QByteArray& input;
	qsizetype position = 0;
	QString error;
};

bool utf16Less(const QString& first, const QString& second)
{
	auto const common = std::min(first.size(), second.size());
	for (qsizetype index = 0; index < common; ++index)
	{
		auto const left = first.at(index).unicode();
		auto const right = second.at(index).unicode();
		if (left != right)
			return left < right;
	}
	return first.size() < second.size();
}

class CanonicalJsonEncoder
{
public:
	bool encode(const QJsonValue& value)
	{
		return encode(value, 0);
	}

	QByteArray result() const { return output; }
	QString errorString() const { return error; }

private:
	bool encode(const QJsonValue& value, int depth)
	{
		if (depth > 256)
			return fail(QStringLiteral("Canonical JSON nesting limit exceeded"));
		switch (value.type())
		{
		case QJsonValue::Null:
			output += "null";
			return true;
		case QJsonValue::Bool:
			output += value.toBool() ? "true" : "false";
			return true;
		case QJsonValue::Double:
			return encodeNumber(value.toDouble());
		case QJsonValue::String:
			return encodeString(value.toString());
		case QJsonValue::Array:
		{
			output += '[';
			auto const array = value.toArray();
			for (qsizetype index = 0; index < array.size(); ++index)
			{
				if (index)
					output += ',';
				if (!encode(array.at(index), depth + 1))
					return false;
			}
			output += ']';
			return true;
		}
		case QJsonValue::Object:
		{
			auto const object = value.toObject();
			auto keys = object.keys();
			std::sort(keys.begin(), keys.end(), utf16Less);
			output += '{';
			for (qsizetype index = 0; index < keys.size(); ++index)
			{
				if (index)
					output += ',';
				if (!encodeString(keys.at(index)))
					return false;
				output += ':';
				if (!encode(object.value(keys.at(index)), depth + 1))
					return false;
			}
			output += '}';
			return true;
		}
		case QJsonValue::Undefined:
			return fail(QStringLiteral("Undefined is not a JSON value"));
		}
		return fail(QStringLiteral("Unknown JSON value type"));
	}

	bool encodeString(const QString& string)
	{
		output += '"';
		for (qsizetype index = 0; index < string.size(); ++index)
		{
			auto const code_unit = string.at(index).unicode();
			switch (code_unit)
			{
			case 0x08: output += "\\b"; continue;
			case 0x09: output += "\\t"; continue;
			case 0x0a: output += "\\n"; continue;
			case 0x0c: output += "\\f"; continue;
			case 0x0d: output += "\\r"; continue;
			case '"': output += "\\\""; continue;
			case '\\': output += "\\\\"; continue;
			default: break;
			}
			if (code_unit < 0x20)
			{
				static const char hex[] = "0123456789abcdef";
				output += "\\u00";
				output += hex[(code_unit >> 4) & 0x0f];
				output += hex[code_unit & 0x0f];
				continue;
			}
			if (QChar::isHighSurrogate(code_unit))
			{
				if (index + 1 >= string.size()
				    || !QChar::isLowSurrogate(string.at(index + 1).unicode()))
				{
					return fail(QStringLiteral("String contains an unpaired high surrogate"));
				}
				QString pair;
				pair.append(string.at(index));
				pair.append(string.at(++index));
				output += pair.toUtf8();
				continue;
			}
			if (QChar::isLowSurrogate(code_unit))
				return fail(QStringLiteral("String contains an unpaired low surrogate"));
			output += QString(string.at(index)).toUtf8();
		}
		output += '"';
		return true;
	}

	bool encodeNumber(double number)
	{
		if (!std::isfinite(number))
			return fail(QStringLiteral("Canonical JSON number is nonfinite"));
		if (number == 0)
		{
			output += '0';
			return true;
		}

		auto shortest =
			QJsonDocument(QJsonArray { number }).toJson(QJsonDocument::Compact);
		shortest = shortest.mid(1, shortest.size() - 2);
		auto const negative = shortest.startsWith('-');
		if (negative)
			shortest.remove(0, 1);

		int exponent = 0;
		auto exponent_position = shortest.indexOf('e');
		if (exponent_position < 0)
			exponent_position = shortest.indexOf('E');
		auto mantissa = shortest;
		if (exponent_position >= 0)
		{
			bool ok = false;
			exponent = shortest.mid(exponent_position + 1).toInt(&ok);
			if (!ok)
				return fail(QStringLiteral("Unable to canonicalize a number exponent"));
			mantissa = shortest.left(exponent_position);
		}

		auto decimal_position = mantissa.indexOf('.');
		if (decimal_position < 0)
			decimal_position = mantissa.size();
		else
			mantissa.remove(decimal_position, 1);
		auto leading_zeroes = 0;
		while (leading_zeroes < mantissa.size()
		       && mantissa.at(leading_zeroes) == '0')
		{
			++leading_zeroes;
		}
		mantissa.remove(0, leading_zeroes);
		decimal_position -= leading_zeroes;
		while (mantissa.size() > 1 && mantissa.endsWith('0'))
			mantissa.chop(1);
		if (mantissa.isEmpty())
			return fail(QStringLiteral("Unable to canonicalize number digits"));

		auto const decimal_point = decimal_position + exponent;
		if (negative)
			output += '-';
		if (decimal_point > 0 && decimal_point <= 21)
		{
			if (decimal_point >= mantissa.size())
			{
				output += mantissa;
				output += QByteArray(decimal_point - mantissa.size(), '0');
			}
			else
			{
				output += mantissa.left(decimal_point);
				output += '.';
				output += mantissa.mid(decimal_point);
			}
		}
		else if (decimal_point <= 0 && decimal_point > -6)
		{
			output += "0.";
			output += QByteArray(-decimal_point, '0');
			output += mantissa;
		}
		else
		{
			output += mantissa.at(0);
			if (mantissa.size() > 1)
			{
				output += '.';
				output += mantissa.mid(1);
			}
			output += 'e';
			auto const scientific_exponent = decimal_point - 1;
			if (scientific_exponent >= 0)
				output += '+';
			output += QByteArray::number(scientific_exponent);
		}
		return true;
	}

	bool fail(const QString& message)
	{
		error = message;
		return false;
	}

	QByteArray output;
	QString error;
};

QByteArray canonicalJson(const QJsonValue& value, QString* error = nullptr)
{
	CanonicalJsonEncoder encoder;
	if (!encoder.encode(value))
	{
		if (error)
			*error = encoder.errorString();
		return {};
	}
	if (error)
		error->clear();
	return encoder.result();
}

QByteArray sha256(const QByteArray& value)
{
	return QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex();
}

QString normalizeUrl(QString value)
{
	value.replace(QStringLiteral("${z}"), QStringLiteral("{z}"));
	value.replace(QStringLiteral("${x}"), QStringLiteral("{x}"));
	value.replace(QStringLiteral("${y}"), QStringLiteral("{y}"));

	auto const scheme_end = value.indexOf(QStringLiteral("://"));
	if (scheme_end < 0)
		return value;
	auto const authority_start = scheme_end + 3;
	auto authority_end = value.size();
	for (auto const separator : {
		QLatin1Char('/'), QLatin1Char('?'), QLatin1Char('#')
	})
	{
		auto const position = value.indexOf(separator, authority_start);
		if (position >= 0)
			authority_end = std::min(authority_end, position);
	}

	auto probe = value;
	probe.replace(QStringLiteral("{z}"), QStringLiteral("0"));
	probe.replace(QStringLiteral("{x}"), QStringLiteral("0"));
	probe.replace(QStringLiteral("{y}"), QStringLiteral("0"));
	auto const parsed = QUrl(probe, QUrl::StrictMode);
	auto host = QString::fromLatin1(QUrl::toAce(parsed.host())).toLower();
	if (host.contains(QLatin1Char(':')))
		host = QLatin1Char('[') + host + QLatin1Char(']');
	auto const scheme = value.left(scheme_end).toLower();
	auto const port = parsed.port(-1);
	auto const default_port =
		(scheme == QLatin1String("http") && port == 80)
		|| (scheme == QLatin1String("https") && port == 443);
	QString authority = host;
	if (port >= 0 && !default_port)
		authority += QLatin1Char(':') + QString::number(port);
	return scheme + QStringLiteral("://") + authority + value.mid(authority_end);
}

QJsonArray sortedStrings(QStringList values)
{
	values.removeDuplicates();
	std::sort(values.begin(), values.end());
	return QJsonArray::fromStringList(values);
}

QJsonArray sortedStatusCodes(QVector<int> values)
{
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
	QJsonArray result;
	for (auto const value : values)
		result.push_back(value);
	return result;
}

QJsonObject normalizedRequest(const OicSourceDefinition& source)
{
	QJsonObject request;
	if (!source.request.referer.isEmpty())
	{
		request.insert(
			QStringLiteral("referer"),
			normalizeUrl(source.request.referer.toString(QUrl::FullyEncoded))
		);
	}
	if (!source.request.empty_http_status_codes.isEmpty())
	{
		request.insert(
			QStringLiteral("emptyHttpStatusCodes"),
			sortedStatusCodes(source.request.empty_http_status_codes)
		);
	}
	return request;
}

QJsonObject normalizedMatrixSet(const OicSourceDefinition& source)
{
	QJsonArray matrices;
	for (auto const& definition : source.tile_matrix_set.matrices)
	{
		auto const& matrix = definition.matrix;
		QJsonObject object {
			{ QStringLiteral("id"), matrix.id },
			{ QStringLiteral("scaleDenominator"), definition.scale_denominator },
			{ QStringLiteral("cellSize"), matrix.cell_size },
			{ QStringLiteral("pointOfOrigin"), QJsonArray {
				matrix.point_of_origin.x(), matrix.point_of_origin.y()
			} },
			{ QStringLiteral("cornerOfOrigin"), definition.corner_of_origin },
			{ QStringLiteral("tileWidth"), matrix.tile_size.width() },
			{ QStringLiteral("tileHeight"), matrix.tile_size.height() },
			{ QStringLiteral("matrixWidth"), double(matrix.matrix_width) },
			{ QStringLiteral("matrixHeight"), double(matrix.matrix_height) },
		};
		if (definition.has_variable_matrix_widths)
		{
			object.insert(
				QStringLiteral("variableMatrixWidths"),
				definition.original_object.value(QStringLiteral("variableMatrixWidths"))
			);
		}
		matrices.push_back(object);
	}
	QJsonObject result {
		{ QStringLiteral("id"), source.tile_matrix_set.matrix_set.id },
		{ QStringLiteral("crs"), source.tile_matrix_set.matrix_set.crs },
		{ QStringLiteral("tileMatrices"), matrices },
	};
	if (!source.tile_matrix_set.ordered_axes.isEmpty())
	{
		result.insert(
			QStringLiteral("orderedAxes"),
			QJsonArray::fromStringList(source.tile_matrix_set.ordered_axes)
		);
	}
	return result;
}

QJsonObject normalizedFullMatrixSet(const OicSourceDefinition& source)
{
	auto result = source.tile_matrix_set.original_object;
	result.insert(
		QStringLiteral("crs"),
		source.tile_matrix_set.matrix_set.crs
	);
	return result;
}

QJsonArray normalizedLimits(const OicSourceDefinition& source)
{
	auto limits = source.tile_limit_definitions;
	auto matrix_index = [&source](const QString& id) {
		auto const& matrices = source.tile_matrix_set.matrix_set.matrices;
		for (qsizetype index = 0; index < matrices.size(); ++index)
		{
			if (matrices.at(index).id == id)
				return int(index);
		}
		return std::numeric_limits<int>::max();
	};
	std::sort(
		limits.begin(), limits.end(),
		[&matrix_index](auto const& first, auto const& second) {
			auto const first_index = matrix_index(first.tile_matrix);
			auto const second_index = matrix_index(second.tile_matrix);
			if (first_index != second_index)
				return first_index < second_index;
			if (first.tile_matrix != second.tile_matrix)
				return first.tile_matrix < second.tile_matrix;
			if (first.min_row != second.min_row)
				return first.min_row < second.min_row;
			if (first.max_row != second.max_row)
				return first.max_row < second.max_row;
			if (first.min_column != second.min_column)
				return first.min_column < second.min_column;
			return first.max_column < second.max_column;
		}
	);
	QJsonArray result;
	for (auto const& limit : limits)
	{
		result.push_back(QJsonObject {
			{ QStringLiteral("tileMatrix"), limit.tile_matrix },
			{ QStringLiteral("minTileRow"), double(limit.min_row) },
			{ QStringLiteral("maxTileRow"), double(limit.max_row) },
			{ QStringLiteral("minTileCol"), double(limit.min_column) },
			{ QStringLiteral("maxTileCol"), double(limit.max_column) },
		});
	}
	return result;
}

QStringList operationalCapabilities(const OicSourceDefinition& source)
{
	auto result = source.required_capabilities;
	result.push_back(QStringLiteral("tile-matrix-set.ogc-2.0"));
	if (source.tile_matrix_set.dyadic_top_left)
		result.push_back(QStringLiteral("tile-matrix-set.dyadic.v1"));
	else if (!source.tile_matrix_set_uri.isEmpty()
	         && source.tile_matrix_set.matrix_set.matrices.isEmpty())
		result.push_back(QStringLiteral("tile-matrix-set.external.v1"));
	else
		result.push_back(QStringLiteral("tile-matrix-set.nondyadic.v1"));
	switch (source.registration.kind)
	{
	case OicRegistrationKind::None:
		break;
	case OicRegistrationKind::Translation2d:
		result.push_back(QStringLiteral("registration.translation2d.v1"));
		break;
	case OicRegistrationKind::Affine2d:
		result.push_back(QStringLiteral("registration.affine2d.v1"));
		break;
	case OicRegistrationKind::GridShift:
		result.push_back(QStringLiteral("registration.grid-shift.v1"));
		break;
	}
	return result;
}

QJsonObject normalizedRegistration(const OicSourceDefinition& source)
{
	auto registration = source.registration.original_object;
	if (registration.isEmpty())
		return {};
	auto source_frame =
		registration.value(QStringLiteral("sourceFrame")).toObject();
	source_frame.insert(QStringLiteral("crs"), source.registration.source_crs);
	registration.insert(QStringLiteral("sourceFrame"), source_frame);
	auto target_frame =
		registration.value(QStringLiteral("targetFrame")).toObject();
	target_frame.insert(QStringLiteral("crs"), source.registration.target_crs);
	registration.insert(QStringLiteral("targetFrame"), target_frame);
	return registration;
}

QJsonObject normalizedOperationalSource(const OicSourceDefinition& source)
{
	QJsonArray tiles;
	for (auto const& tile : source.tile_urls)
		tiles.push_back(normalizeUrl(tile.value));
	QJsonObject result {
		{ QStringLiteral("fingerprintVersion"), 1 },
		{ QStringLiteral("type"), source.type },
		{ QStringLiteral("tiles"), tiles },
		{ QStringLiteral("scheme"), tileRowSchemeName(source.row_scheme) },
		{ QStringLiteral("format"), source.media_type },
		{ QStringLiteral("minTileMatrix"), source.min_tile_matrix },
		{ QStringLiteral("maxTileMatrix"), source.max_tile_matrix },
		{ QStringLiteral("tileMatrixLimits"), normalizedLimits(source) },
		{ QStringLiteral("request"), normalizedRequest(source) },
		{ QStringLiteral("registration"),
		  source.registration.kind == OicRegistrationKind::None
		    ? QJsonValue(QJsonValue::Null)
		    : QJsonValue(normalizedRegistration(source)) },
		{ QStringLiteral("requires"),
		  sortedStrings(operationalCapabilities(source)) },
	};
	if (!source.tile_matrix_set.matrix_set.matrices.isEmpty())
		result.insert(QStringLiteral("tileMatrixSet"), normalizedMatrixSet(source));
	else
	{
		result.insert(
			QStringLiteral("tileMatrixSetURI"),
			normalizeUrl(source.tile_matrix_set_uri)
		);
	}
	return result;
}

QJsonObject normalizedFullSource(const OicSourceDefinition& source)
{
	auto result = source.original_object;
	QJsonArray tiles;
	for (auto const& tile : source.tile_urls)
		tiles.push_back(normalizeUrl(tile.value));
	result.insert(QStringLiteral("tiles"), tiles);
	result.insert(QStringLiteral("format"), source.media_type);
	result.insert(QStringLiteral("minTileMatrix"), source.min_tile_matrix);
	result.insert(QStringLiteral("maxTileMatrix"), source.max_tile_matrix);
	if (!source.tile_matrix_set.matrix_set.matrices.isEmpty())
	{
		result.remove(QStringLiteral("tileMatrixSetURI"));
		result.insert(
			QStringLiteral("tileMatrixSet"),
			normalizedFullMatrixSet(source)
		);
	}
	else
	{
		result.insert(
			QStringLiteral("tileMatrixSetURI"),
			normalizeUrl(source.tile_matrix_set_uri)
		);
	}
	if (result.contains(QStringLiteral("requires")))
	{
		result.insert(
			QStringLiteral("requires"),
			sortedStrings(source.required_capabilities)
		);
	}
	if (result.contains(QStringLiteral("request")))
		result.insert(QStringLiteral("request"), normalizedRequest(source));
	if (result.contains(QStringLiteral("tileMatrixLimits")))
		result.insert(QStringLiteral("tileMatrixLimits"), normalizedLimits(source));
	if (result.contains(QStringLiteral("registration")))
		result.insert(QStringLiteral("registration"), normalizedRegistration(source));
	if (result.value(QStringLiteral("notices")).isObject())
	{
		auto notices = result.value(QStringLiteral("notices")).toObject();
		for (auto const& name : {
			QStringLiteral("attributionUrl"), QStringLiteral("sourceUrl"),
			QStringLiteral("termsUrl"), QStringLiteral("privacyUrl")
		})
		{
			if (notices.value(name).isString())
				notices.insert(name, normalizeUrl(notices.value(name).toString()));
		}
		result.insert(QStringLiteral("notices"), notices);
	}
	return result;
}

bool calculateFingerprints(
	OicSourceDefinition& source,
	QString* error)
{
	auto const full_object = QJsonObject {
		{ QStringLiteral("fingerprintVersion"), 1 },
		{ QStringLiteral("source"), normalizedFullSource(source) },
	};
	auto const full = canonicalJson(full_object, error);
	if (full.isEmpty())
		return false;
	auto const operational =
		canonicalJson(normalizedOperationalSource(source), error);
	if (operational.isEmpty())
		return false;
	source.full_fingerprint = sha256(full);
	source.operational_fingerprint = sha256(operational);
	return true;
}

bool containsControl(const QString& value)
{
	for (auto const character : value)
	{
		auto const code = character.unicode();
		if (code < 0x20 || code == 0x7f)
			return true;
	}
	return false;
}

bool containsUrlWhitespaceOrControl(const QString& value)
{
	for (auto const character : value)
	{
		auto const code = character.unicode();
		if (code < 0x20 || code == 0x7f || character.isSpace())
			return true;
	}
	return false;
}

bool urlAuthorityContainsUserInfo(const QString& value)
{
	auto const scheme_end = value.indexOf(QStringLiteral("://"));
	if (scheme_end < 0)
		return false;
	auto authority_end = value.size();
	for (auto const separator : {
		QLatin1Char('/'), QLatin1Char('?'), QLatin1Char('#')
	})
	{
		auto const position =
			value.indexOf(separator, scheme_end + 3);
		if (position >= 0)
			authority_end = std::min(authority_end, position);
	}
	return value.mid(
		scheme_end + 3,
		authority_end - (scheme_end + 3)
	).contains(QLatin1Char('@'));
}

class CatalogValidator
{
public:
	explicit CatalogValidator(OicCatalogReadResult& result)
	: result(result)
	{}

	void validate(const QJsonObject& root, const QByteArray& bytes)
	{
		static const QSet<QString> fields {
			QStringLiteral("$schema"), QStringLiteral("format"),
			QStringLiteral("version"), QStringLiteral("id"),
			QStringLiteral("revision"), QStringLiteral("name"),
			QStringLiteral("description"), QStringLiteral("publisher"),
			QStringLiteral("created"), QStringLiteral("updated"),
			QStringLiteral("catalogLicense"), QStringLiteral("requires"),
			QStringLiteral("resources"), QStringLiteral("extensions"),
			QStringLiteral("sources"),
		};
		checkUnknownFields(root, fields, QStringLiteral("$"), true);

		auto& catalog = result.catalog;
		catalog.original_bytes = bytes;
		catalog.original_object = root;
		catalog.format =
			requiredString(root, QStringLiteral("format"),
			               QStringLiteral("$.format"), true);
		if (catalog.format != QLatin1String(catalog_format))
		{
			catalogError(
				QStringLiteral("unsupported-format"),
				QStringLiteral("$.format"),
				QStringLiteral("Unsupported catalog format")
			);
		}
		catalog.version =
			int(requiredInteger(root, QStringLiteral("version"),
			                    QStringLiteral("$.version"), true,
			                    1, std::numeric_limits<int>::max()));
		if (catalog.version != 1)
		{
			catalogError(
				QStringLiteral("unsupported-version"),
				QStringLiteral("$.version"),
				QStringLiteral("Unsupported catalog version")
			);
		}
		catalog.id =
			requiredId(root, QStringLiteral("id"),
			           QStringLiteral("$.id"), true);
		catalog.revision =
			int(requiredInteger(root, QStringLiteral("revision"),
			                    QStringLiteral("$.revision"), true,
			                    1, std::numeric_limits<int>::max()));
		catalog.name =
			requiredText(root, QStringLiteral("name"),
			             QStringLiteral("$.name"), true,
			             OicCatalogReader::maximum_string_length);
		catalog.description =
			optionalText(root, QStringLiteral("description"),
			             QStringLiteral("$.description"), true,
			             OicCatalogReader::maximum_string_length);
		catalog.created =
			optionalDate(root, QStringLiteral("created"),
			             QStringLiteral("$.created"), true);
		catalog.updated =
			optionalDate(root, QStringLiteral("updated"),
			             QStringLiteral("$.updated"), true);
		if (catalog.created.isValid() && catalog.updated.isValid()
		    && catalog.created > catalog.updated)
		{
			catalogError(
				QStringLiteral("date-order"),
				QStringLiteral("$.updated"),
				QStringLiteral("Catalog update date precedes its creation date")
			);
		}
		catalog.catalog_license =
			optionalText(root, QStringLiteral("catalogLicense"),
			             QStringLiteral("$.catalogLicense"), true,
			             4096);
		if (root.contains(QStringLiteral("$schema")))
		{
			absoluteUrl(
				requiredString(root, QStringLiteral("$schema"),
				               QStringLiteral("$.$schema"), true),
				QStringLiteral("$.$schema"), true
			);
		}
		if (root.contains(QStringLiteral("publisher")))
			catalog.publisher = validatePublisher(
				root.value(QStringLiteral("publisher")),
				QStringLiteral("$.publisher")
			);
		if (root.contains(QStringLiteral("requires")))
		{
			catalog.required_capabilities =
				validateCapabilities(
					root.value(QStringLiteral("requires")),
					QStringLiteral("$.requires"), true
				);
			for (auto const& capability : catalog.required_capabilities)
			{
				if (!runtimeCapabilities().contains(capability))
				{
					catalogError(
						QStringLiteral("unsupported-capability"),
						QStringLiteral("$.requires"),
						QStringLiteral(
							"Required catalog capability is unsupported: %1"
						).arg(capability)
					);
				}
			}
		}
		if (root.contains(QStringLiteral("resources")))
		{
			validateResources(
				root.value(QStringLiteral("resources")),
				QStringLiteral("$.resources")
			);
		}
		if (root.contains(QStringLiteral("extensions")))
		{
			catalog.extensions =
				objectValue(
					root.value(QStringLiteral("extensions")),
					QStringLiteral("$.extensions"), true
				);
			validateExtensions(
				catalog.extensions, QStringLiteral("$.extensions"), true
			);
		}

		if (!root.value(QStringLiteral("sources")).isArray())
		{
			catalogError(
				QStringLiteral("missing-sources"),
				QStringLiteral("$.sources"),
				QStringLiteral("Required member must be an array")
			);
			return;
		}
		auto const sources = root.value(QStringLiteral("sources")).toArray();
		if (sources.isEmpty())
		{
			catalogError(
				QStringLiteral("empty-sources"),
				QStringLiteral("$.sources"),
				QStringLiteral("Catalog must contain at least one source")
			);
		}
		if (sources.size() > OicCatalogReader::maximum_sources)
		{
			catalogError(
				QStringLiteral("source-limit"),
				QStringLiteral("$.sources"),
				QStringLiteral("Catalog exceeds the %1 source limit")
					.arg(OicCatalogReader::maximum_sources)
			);
		}

		QSet<QString> source_ids;
		auto const count =
			std::min(sources.size(), qsizetype(OicCatalogReader::maximum_sources));
		for (qsizetype index = 0; index < count; ++index)
		{
			current_source = int(index);
			auto const path = QStringLiteral("$.sources[%1]").arg(index);
			if (!sources.at(index).isObject())
			{
				sourceError(
					QStringLiteral("source-type"), path,
					QStringLiteral("Source must be an object")
				);
				result.catalog.sources.push_back({});
				continue;
			}

			auto source = validateSource(sources.at(index).toObject(), path);
			if (!source.metadata.id.isEmpty())
			{
				if (source_ids.contains(source.metadata.id))
				{
					catalogError(
						QStringLiteral("duplicate-source-id"),
						path + QStringLiteral(".id"),
						QStringLiteral("Duplicate source ID: %1")
							.arg(source.metadata.id)
					);
				}
				source_ids.insert(source.metadata.id);
			}
			result.catalog.sources.push_back(std::move(source));
		}
		current_source = -1;
	}

private:
	OicSourceDefinition validateSource(
		const QJsonObject& object,
		const QString& path)
	{
		static const QSet<QString> fields {
			QStringLiteral("id"), QStringLiteral("name"),
			QStringLiteral("description"), QStringLiteral("type"),
			QStringLiteral("tiles"), QStringLiteral("scheme"),
			QStringLiteral("format"), QStringLiteral("minTileMatrix"),
			QStringLiteral("maxTileMatrix"),
			QStringLiteral("tileMatrixSetURI"),
			QStringLiteral("tileMatrixSet"),
			QStringLiteral("tileMatrixLimits"),
			QStringLiteral("request"), QStringLiteral("requires"),
			QStringLiteral("category"), QStringLiteral("startDate"),
			QStringLiteral("endDate"), QStringLiteral("coverage"),
			QStringLiteral("notices"), QStringLiteral("registration"),
			QStringLiteral("extensions"),
		};
		auto const diagnostic_start = result.diagnostics.size();
		checkUnknownFields(object, fields, path, false);

		OicSourceDefinition source;
		source.original_object = object;
		source.metadata.id =
			requiredId(object, QStringLiteral("id"),
			           path + QStringLiteral(".id"), false);
		if (!source.metadata.id.isEmpty()
		    && !validRuntimeId(source.metadata.id))
		{
			addUnsupported(
				source, QStringLiteral("source-id.runtime.v1"),
				path + QStringLiteral(".id"),
				QStringLiteral(
					"Source ID exceeds the resolved runtime identifier profile"
				)
			);
		}
		if (!result.catalog.id.isEmpty()
		    && !validRuntimeId(result.catalog.id))
		{
			addUnsupported(
				source, QStringLiteral("catalog-id.runtime.v1"),
				QStringLiteral("$.id"),
				QStringLiteral(
					"Catalog ID exceeds the resolved runtime identifier profile"
				)
			);
		}
		source.metadata.name =
			requiredText(object, QStringLiteral("name"),
			             path + QStringLiteral(".name"), false, 512);
		source.metadata.description =
			optionalText(object, QStringLiteral("description"),
			             path + QStringLiteral(".description"), false, 4096);
		source.type =
			requiredString(object, QStringLiteral("type"),
			               path + QStringLiteral(".type"), false);
		if (source.type != QLatin1String("raster-tiles"))
		{
			sourceError(
				QStringLiteral("unsupported-source-type"),
				path + QStringLiteral(".type"),
				QStringLiteral("Source type must be raster-tiles")
			);
		}

		validateTiles(
			object.value(QStringLiteral("tiles")),
			path + QStringLiteral(".tiles"), source
		);
		auto const scheme =
			requiredString(object, QStringLiteral("scheme"),
			               path + QStringLiteral(".scheme"), false);
		auto const parsed_scheme = tileRowSchemeFromName(scheme);
		if (!parsed_scheme)
		{
			sourceError(
				QStringLiteral("invalid-scheme"),
				path + QStringLiteral(".scheme"),
				QStringLiteral("Scheme must be xyz or tms")
			);
		}
		else
		{
			source.row_scheme = *parsed_scheme;
		}

		if (object.contains(QStringLiteral("format")))
		{
			source.media_type =
				requiredString(object, QStringLiteral("format"),
				               path + QStringLiteral(".format"), false);
		}
		static const QRegularExpression media_type_pattern(
			QStringLiteral("^image/[A-Za-z0-9!#$&^_.+-]{1,96}$")
		);
		if (!media_type_pattern.match(source.media_type).hasMatch())
		{
			sourceError(
				QStringLiteral("invalid-media-type"),
				path + QStringLiteral(".format"),
				QStringLiteral("Format must be a supported image media type")
			);
		}

		source.min_tile_matrix =
			optionalString(object, QStringLiteral("minTileMatrix"),
			               path + QStringLiteral(".minTileMatrix"), false);
		source.max_tile_matrix =
			optionalString(object, QStringLiteral("maxTileMatrix"),
			               path + QStringLiteral(".maxTileMatrix"), false);
		if ((!source.min_tile_matrix.isEmpty()
		     && !validMatrixId(source.min_tile_matrix))
		    || (!source.max_tile_matrix.isEmpty()
		        && !validMatrixId(source.max_tile_matrix)))
		{
			sourceError(
				QStringLiteral("invalid-matrix-id"), path,
				QStringLiteral("Tile matrix identifiers are invalid")
			);
		}

		auto const has_uri = object.contains(QStringLiteral("tileMatrixSetURI"));
		auto const has_inline = object.contains(QStringLiteral("tileMatrixSet"));
		if (has_uri == has_inline)
		{
			sourceError(
				QStringLiteral("matrix-set-choice"), path,
				QStringLiteral(
					"Source must contain exactly one of tileMatrixSetURI "
					"or tileMatrixSet"
				)
			);
		}
		else if (has_uri)
		{
			validateMatrixSetUri(
				object.value(QStringLiteral("tileMatrixSetURI")),
				path + QStringLiteral(".tileMatrixSetURI"), source
			);
		}
		else
		{
			validateTileMatrixSet(
				object.value(QStringLiteral("tileMatrixSet")),
				path + QStringLiteral(".tileMatrixSet"), source
			);
		}

		if (object.contains(QStringLiteral("tileMatrixLimits")))
		{
			validateTileMatrixLimits(
				object.value(QStringLiteral("tileMatrixLimits")),
				path + QStringLiteral(".tileMatrixLimits"), source
			);
		}
		validateMatrixRange(source, path);
		if (object.contains(QStringLiteral("request")))
		{
			validateRequest(
				object.value(QStringLiteral("request")),
				path + QStringLiteral(".request"), source
			);
		}
		if (object.contains(QStringLiteral("requires")))
		{
			source.required_capabilities =
				validateCapabilities(
					object.value(QStringLiteral("requires")),
					path + QStringLiteral(".requires"), false
				);
			for (auto const& capability : source.required_capabilities)
			{
				if (!runtimeCapabilities().contains(capability))
				{
					addUnsupported(
						source, capability,
						path + QStringLiteral(".requires"),
						QStringLiteral("Required source capability is unsupported")
					);
				}
			}
		}
		validatePresentation(object, path, source);
		if (object.contains(QStringLiteral("registration")))
		{
			validateRegistration(
				object.value(QStringLiteral("registration")),
				path + QStringLiteral(".registration"), source
			);
		}
		if (object.contains(QStringLiteral("extensions")))
		{
			source.extensions =
				objectValue(
					object.value(QStringLiteral("extensions")),
					path + QStringLiteral(".extensions"), false
				);
			validateExtensions(
				source.extensions,
				path + QStringLiteral(".extensions"), false
			);
		}

		source.valid = !hasSourceErrors(diagnostic_start);
		source.supported =
			source.valid && source.unsupported_capabilities.isEmpty();
		if (source.valid)
		{
			QString fingerprint_error;
			if (!calculateFingerprints(source, &fingerprint_error))
			{
				sourceError(
					QStringLiteral("fingerprint"),
					path,
					QStringLiteral("Unable to fingerprint source: %1")
						.arg(fingerprint_error)
				);
				source.valid = false;
				source.supported = false;
			}
		}
		if (source.supported)
			resolveSource(source, path);
		return source;
	}

	void validateTiles(
		const QJsonValue& value,
		const QString& path,
		OicSourceDefinition& source)
	{
		if (!value.isArray())
		{
			sourceError(
				QStringLiteral("tiles-type"), path,
				QStringLiteral("Tiles must be a nonempty array")
			);
			return;
		}
		auto const array = value.toArray();
		if (array.isEmpty()
		    || array.size() > OicCatalogReader::maximum_tiles_per_source)
		{
			sourceError(
				QStringLiteral("tiles-count"), path,
				QStringLiteral("Tiles must contain between 1 and %1 templates")
					.arg(OicCatalogReader::maximum_tiles_per_source)
			);
		}
		QSet<QString> unique;
		auto const count = std::min(
			array.size(),
			qsizetype(OicCatalogReader::maximum_tiles_per_source)
		);
		for (qsizetype index = 0; index < count; ++index)
		{
			auto const item_path = path + QStringLiteral("[%1]").arg(index);
			if (!array.at(index).isString())
			{
				sourceError(
					QStringLiteral("template-type"), item_path,
					QStringLiteral("Tile template must be a string")
				);
				continue;
			}
			auto const published_template = array.at(index).toString();
			auto runtime_template = published_template;
			runtime_template.replace(
				QStringLiteral("${z}"), QStringLiteral("{z}")
			);
			runtime_template.replace(
				QStringLiteral("${x}"), QStringLiteral("{x}")
			);
			runtime_template.replace(
				QStringLiteral("${y}"), QStringLiteral("{y}")
			);
			TileUrlTemplate tile { std::move(runtime_template) };
			QString error;
			if (containsUrlWhitespaceOrControl(published_template)
			    || urlAuthorityContainsUserInfo(published_template))
			{
				sourceError(
					QStringLiteral("invalid-template"), item_path,
					QStringLiteral(
						"Tile URL template contains raw whitespace, "
						"a control character, or user information"
					)
				);
				continue;
			}
			if (!tile.validate(&error))
			{
				sourceError(
					QStringLiteral("invalid-template"), item_path, error
				);
				continue;
			}
			if (unique.contains(tile.value))
			{
				sourceError(
					QStringLiteral("duplicate-template"), item_path,
					QStringLiteral("Duplicate tile URL template")
				);
				continue;
			}
			unique.insert(tile.value);
			source.tile_urls.push_back(std::move(tile));
		}
	}

	void validateMatrixSetUri(
		const QJsonValue& value,
		const QString& path,
		OicSourceDefinition& source)
	{
		if (!value.isString())
		{
			sourceError(
				QStringLiteral("matrix-uri-type"), path,
				QStringLiteral("Tile matrix set URI must be a string")
			);
			return;
		}
		source.tile_matrix_set_uri = value.toString();
		auto const uri =
			httpUrl(source.tile_matrix_set_uri, path, false);
		if (!uri.isValid())
			return;

		auto const scheme = uri.scheme().toLower();
		auto const port = uri.port(-1);
		auto const default_port =
			port < 0
			|| (scheme == QLatin1String("http") && port == 80)
			|| (scheme == QLatin1String("https") && port == 443);
		auto const known =
			(scheme == QLatin1String("http")
			 || scheme == QLatin1String("https"))
			&& uri.host().compare(
				QLatin1String(web_mercator_quad_host),
				Qt::CaseInsensitive
			) == 0
			&& uri.path(QUrl::FullyEncoded)
			     == QLatin1String(web_mercator_quad_path)
			&& default_port
			&& !uri.hasQuery();
		if (!known)
		{
			addUnsupported(
				source, QStringLiteral("tile-matrix-set.external.v1"),
				path, QStringLiteral("Unknown tile matrix set URI")
			);
			return;
		}

		source.tile_matrix_set.matrix_set =
			TileMatrixSet::webMercatorQuad();
		source.tile_matrix_set.dyadic_top_left = true;
		source.tile_matrix_set.ordered_axes = {
			QStringLiteral("X"), QStringLiteral("Y")
		};
		constexpr auto base_scale_denominator = 559082264.0287178;
		for (auto const& matrix : source.tile_matrix_set.matrix_set.matrices)
		{
			source.tile_matrix_set.matrices.push_back({
				matrix,
				base_scale_denominator / std::ldexp(1.0, matrix.zoom),
				QStringLiteral("topLeft"),
				false,
				{},
			});
		}
		source.tile_matrix_set.original_object =
			normalizedMatrixSet(source);
	}

	void validateTileMatrixSet(
		const QJsonValue& value,
		const QString& path,
		OicSourceDefinition& source)
	{
		if (!value.isObject())
		{
			sourceError(
				QStringLiteral("matrix-set-type"), path,
				QStringLiteral("Tile matrix set must be an object")
			);
			return;
		}
		auto const object = value.toObject();
		auto& definition = source.tile_matrix_set;
		definition.original_object = object;
		static const QSet<QString> fields {
			QStringLiteral("id"), QStringLiteral("title"),
			QStringLiteral("description"), QStringLiteral("keywords"),
			QStringLiteral("uri"), QStringLiteral("orderedAxes"),
			QStringLiteral("crs"), QStringLiteral("wellKnownScaleSet"),
			QStringLiteral("boundingBox"), QStringLiteral("tileMatrices"),
		};
		checkUnknownFields(object, fields, path, false);
		definition.matrix_set.id =
			requiredId(object, QStringLiteral("id"),
			           path + QStringLiteral(".id"), false);
		optionalText(
			object, QStringLiteral("title"),
			path + QStringLiteral(".title"), false, 4096
		);
		optionalText(
			object, QStringLiteral("description"),
			path + QStringLiteral(".description"), false, 4096
		);
		if (object.contains(QStringLiteral("keywords")))
		{
			validateStringArray(
				object.value(QStringLiteral("keywords")),
				path + QStringLiteral(".keywords"), false, false, 64
			);
		}
		if (object.contains(QStringLiteral("uri")))
		{
			absoluteUrl(
				requiredString(
					object, QStringLiteral("uri"),
					path + QStringLiteral(".uri"), false
				),
				path + QStringLiteral(".uri"), false
			);
		}
		if (object.contains(QStringLiteral("wellKnownScaleSet")))
		{
			absoluteUrl(
				requiredString(
					object, QStringLiteral("wellKnownScaleSet"),
					path + QStringLiteral(".wellKnownScaleSet"), false
				),
				path + QStringLiteral(".wellKnownScaleSet"), false
			);
		}
		definition.matrix_set.crs =
			normalizeCrs(
				requiredString(
					object, QStringLiteral("crs"),
					path + QStringLiteral(".crs"), false
				),
				path + QStringLiteral(".crs")
			);
		if (object.contains(QStringLiteral("orderedAxes")))
		{
			definition.ordered_axes =
				validateStringArray(
					object.value(QStringLiteral("orderedAxes")),
					path + QStringLiteral(".orderedAxes"), false, false, 2
				);
			if (definition.ordered_axes.size() != 2)
			{
				sourceError(
					QStringLiteral("axis-count"),
					path + QStringLiteral(".orderedAxes"),
					QStringLiteral("orderedAxes must contain exactly two axes")
				);
			}
			else
			{
				auto const first =
					definition.ordered_axes.at(0).trimmed().toLower();
				auto const second =
					definition.ordered_axes.at(1).trimmed().toLower();
				auto const supported_axes =
					(first == QLatin1String("x")
					 && second == QLatin1String("y"))
					|| (first == QLatin1String("e")
					    && second == QLatin1String("n"))
					|| (first == QLatin1String("easting")
					    && second == QLatin1String("northing"))
					|| (first == QLatin1String("longitude")
					    && second == QLatin1String("latitude"));
				if (!supported_axes)
				{
					addUnsupported(
						source,
						QStringLiteral("tile-matrix-set.axis-order.v1"),
						path + QStringLiteral(".orderedAxes"),
						QStringLiteral(
							"Tile matrix axes must put the east-west "
							"coordinate before the north-south coordinate"
						)
					);
				}
			}
		}
		if (object.contains(QStringLiteral("boundingBox")))
		{
			validateBoundingBox(
				object.value(QStringLiteral("boundingBox")),
				path + QStringLiteral(".boundingBox")
			);
		}

		if (!object.value(QStringLiteral("tileMatrices")).isArray())
		{
			sourceError(
				QStringLiteral("matrices-type"),
				path + QStringLiteral(".tileMatrices"),
				QStringLiteral("Required member must be an array")
			);
			return;
		}
		auto const matrices =
			object.value(QStringLiteral("tileMatrices")).toArray();
		if (matrices.isEmpty()
		    || matrices.size() > OicCatalogReader::maximum_tile_matrices)
		{
			sourceError(
				QStringLiteral("matrix-count"),
				path + QStringLiteral(".tileMatrices"),
				QStringLiteral("Tile matrix count must be between 1 and %1")
					.arg(OicCatalogReader::maximum_tile_matrices)
			);
		}

		QSet<QString> ids;
		auto const count = std::min(
			matrices.size(),
			qsizetype(OicCatalogReader::maximum_tile_matrices)
		);
		auto bottom_left = false;
		auto variable_width = false;
		for (qsizetype index = 0; index < count; ++index)
		{
			auto matrix_definition =
				validateTileMatrix(
					matrices.at(index),
					path + QStringLiteral(".tileMatrices[%1]").arg(index),
					int(index)
				);
			if (!matrix_definition.matrix.id.isEmpty()
			    && ids.contains(matrix_definition.matrix.id))
			{
				sourceError(
					QStringLiteral("duplicate-matrix-id"),
					path + QStringLiteral(".tileMatrices[%1].id").arg(index),
					QStringLiteral("Duplicate tile matrix ID")
				);
			}
			ids.insert(matrix_definition.matrix.id);
			bottom_left =
				bottom_left
				|| matrix_definition.corner_of_origin
				     == QLatin1String("bottomLeft");
			variable_width =
				variable_width
				|| matrix_definition.has_variable_matrix_widths;
			definition.matrix_set.matrices.push_back(
				matrix_definition.matrix
			);
			definition.matrices.push_back(std::move(matrix_definition));
		}

		if (bottom_left)
		{
			addUnsupported(
				source, QStringLiteral("tile-matrix-set.bottom-left.v1"),
				path,
				QStringLiteral("Bottom-left matrix origins are not supported")
			);
		}
		if (variable_width)
		{
			addUnsupported(
				source, QStringLiteral("tile-matrix-set.variable-width.v1"),
				path,
				QStringLiteral("Variable-width tile matrices are not supported")
			);
		}
		if (matrices.size() > 63)
		{
			addUnsupported(
				source, QStringLiteral("tile-matrix-set.runtime-size.v1"),
				path,
				QStringLiteral(
					"Tile matrix set exceeds the resolved runtime zoom count"
				)
			);
		}

		QString dyadic_error;
		definition.dyadic_top_left =
			!bottom_left && !variable_width
			&& matrices.size() <= 63
			&& definition.matrix_set.validateDyadicTopLeft(&dyadic_error);
		if (!definition.dyadic_top_left
		    && !bottom_left && !variable_width && matrices.size() <= 63)
		{
			addUnsupported(
				source, QStringLiteral("tile-matrix-set.nondyadic.v1"),
				path,
				QStringLiteral("Tile matrix set is outside the dyadic "
				               "top-left runtime profile: %1")
					.arg(dyadic_error)
			);
		}
	}

	OicTileMatrixDefinition validateTileMatrix(
		const QJsonValue& value,
		const QString& path,
		int zoom)
	{
		OicTileMatrixDefinition definition;
		definition.matrix.zoom = zoom;
		if (!value.isObject())
		{
			sourceError(
				QStringLiteral("matrix-type"), path,
				QStringLiteral("Tile matrix must be an object")
			);
			return definition;
		}
		auto const object = value.toObject();
		definition.original_object = object;
		static const QSet<QString> fields {
			QStringLiteral("id"), QStringLiteral("title"),
			QStringLiteral("description"), QStringLiteral("keywords"),
			QStringLiteral("scaleDenominator"), QStringLiteral("cellSize"),
			QStringLiteral("pointOfOrigin"),
			QStringLiteral("cornerOfOrigin"), QStringLiteral("tileWidth"),
			QStringLiteral("tileHeight"), QStringLiteral("matrixWidth"),
			QStringLiteral("matrixHeight"),
			QStringLiteral("variableMatrixWidths"),
		};
		checkUnknownFields(object, fields, path, false);
		definition.matrix.id =
			requiredString(object, QStringLiteral("id"),
			               path + QStringLiteral(".id"), false);
		if (!validMatrixId(definition.matrix.id))
		{
			sourceError(
				QStringLiteral("invalid-matrix-id"),
				path + QStringLiteral(".id"),
				QStringLiteral("Invalid tile matrix identifier")
			);
		}
		optionalText(
			object, QStringLiteral("title"),
			path + QStringLiteral(".title"), false, 4096
		);
		optionalText(
			object, QStringLiteral("description"),
			path + QStringLiteral(".description"), false, 4096
		);
		if (object.contains(QStringLiteral("keywords")))
		{
			validateStringArray(
				object.value(QStringLiteral("keywords")),
				path + QStringLiteral(".keywords"), false, false, 64
			);
		}
		definition.scale_denominator =
			requiredPositiveNumber(
				object, QStringLiteral("scaleDenominator"),
				path + QStringLiteral(".scaleDenominator")
			);
		definition.matrix.cell_size =
			requiredPositiveNumber(
				object, QStringLiteral("cellSize"),
				path + QStringLiteral(".cellSize")
			);

		auto const origin = object.value(QStringLiteral("pointOfOrigin"));
		if (!origin.isArray() || origin.toArray().size() != 2
		    || !finiteNumber(origin.toArray().at(0))
		    || !finiteNumber(origin.toArray().at(1)))
		{
			sourceError(
				QStringLiteral("invalid-origin"),
				path + QStringLiteral(".pointOfOrigin"),
				QStringLiteral(
					"Point of origin must contain two finite numbers"
				)
			);
		}
		else
		{
			definition.matrix.point_of_origin = QPointF(
				origin.toArray().at(0).toDouble(),
				origin.toArray().at(1).toDouble()
			);
		}
		if (object.contains(QStringLiteral("cornerOfOrigin")))
		{
			definition.corner_of_origin =
				requiredString(
					object, QStringLiteral("cornerOfOrigin"),
					path + QStringLiteral(".cornerOfOrigin"), false
				);
		}
		if (definition.corner_of_origin != QLatin1String("topLeft")
		    && definition.corner_of_origin != QLatin1String("bottomLeft"))
		{
			sourceError(
				QStringLiteral("invalid-origin-corner"),
				path + QStringLiteral(".cornerOfOrigin"),
				QStringLiteral("cornerOfOrigin must be topLeft or bottomLeft")
			);
		}

		definition.matrix.tile_size.setWidth(int(requiredInteger(
			object, QStringLiteral("tileWidth"),
			path + QStringLiteral(".tileWidth"), false, 1, 65536
		)));
		definition.matrix.tile_size.setHeight(int(requiredInteger(
			object, QStringLiteral("tileHeight"),
			path + QStringLiteral(".tileHeight"), false, 1, 65536
		)));
		definition.matrix.matrix_width =
			requiredInteger(
				object, QStringLiteral("matrixWidth"),
				path + QStringLiteral(".matrixWidth"), false,
				1, qint64(maximum_exact_integer)
			);
		definition.matrix.matrix_height =
			requiredInteger(
				object, QStringLiteral("matrixHeight"),
				path + QStringLiteral(".matrixHeight"), false,
				1, qint64(maximum_exact_integer)
			);
		if (object.contains(QStringLiteral("variableMatrixWidths")))
		{
			definition.has_variable_matrix_widths = true;
			validateVariableMatrixWidths(
				object.value(QStringLiteral("variableMatrixWidths")),
				path + QStringLiteral(".variableMatrixWidths"),
				definition.matrix.matrix_height
			);
		}
		return definition;
	}

	void validateVariableMatrixWidths(
		const QJsonValue& value,
		const QString& path,
		qint64 matrix_height)
	{
		if (!value.isArray() || value.toArray().isEmpty())
		{
			sourceError(
				QStringLiteral("variable-width-type"), path,
				QStringLiteral(
					"Variable matrix widths must be a nonempty array"
				)
			);
			return;
		}
		auto const array = value.toArray();
		for (qsizetype index = 0; index < array.size(); ++index)
		{
			auto const item_path = path + QStringLiteral("[%1]").arg(index);
			if (!array.at(index).isObject())
			{
				sourceError(
					QStringLiteral("variable-width-entry"), item_path,
					QStringLiteral(
						"Variable matrix width entry must be an object"
					)
				);
				continue;
			}
			auto const object = array.at(index).toObject();
			static const QSet<QString> fields {
				QStringLiteral("coalesce"), QStringLiteral("minTileRow"),
				QStringLiteral("maxTileRow"),
			};
			checkUnknownFields(object, fields, item_path, false);
			requiredInteger(
				object, QStringLiteral("coalesce"),
				item_path + QStringLiteral(".coalesce"), false,
				2, qint64(maximum_exact_integer)
			);
			auto const minimum = requiredInteger(
				object, QStringLiteral("minTileRow"),
				item_path + QStringLiteral(".minTileRow"), false,
				0, qint64(maximum_exact_integer)
			);
			auto const maximum = requiredInteger(
				object, QStringLiteral("maxTileRow"),
				item_path + QStringLiteral(".maxTileRow"), false,
				0, qint64(maximum_exact_integer)
			);
			if (minimum > maximum || maximum >= matrix_height)
			{
				sourceError(
					QStringLiteral("variable-width-bounds"), item_path,
					QStringLiteral(
						"Variable matrix width rows are outside the matrix"
					)
				);
			}
		}
	}

	void validateTileMatrixLimits(
		const QJsonValue& value,
		const QString& path,
		OicSourceDefinition& source)
	{
		if (!value.isArray())
		{
			sourceError(
				QStringLiteral("limits-type"), path,
				QStringLiteral("Tile matrix limits must be an array")
			);
			return;
		}
		auto const array = value.toArray();
		if (array.size() > OicCatalogReader::maximum_tile_matrices)
		{
			sourceError(
				QStringLiteral("limits-count"), path,
				QStringLiteral("Too many tile matrix limits")
			);
		}
		QSet<QString> ids;
		auto const count = std::min(
			array.size(),
			qsizetype(OicCatalogReader::maximum_tile_matrices)
		);
		for (qsizetype index = 0; index < count; ++index)
		{
			auto const item_path = path + QStringLiteral("[%1]").arg(index);
			if (!array.at(index).isObject())
			{
				sourceError(
					QStringLiteral("limit-type"), item_path,
					QStringLiteral("Tile matrix limit must be an object")
				);
				continue;
			}
			auto const object = array.at(index).toObject();
			static const QSet<QString> fields {
				QStringLiteral("tileMatrix"), QStringLiteral("minTileRow"),
				QStringLiteral("maxTileRow"), QStringLiteral("minTileCol"),
				QStringLiteral("maxTileCol"),
			};
			checkUnknownFields(object, fields, item_path, false);
			auto const matrix_id =
				requiredString(
					object, QStringLiteral("tileMatrix"),
					item_path + QStringLiteral(".tileMatrix"), false
				);
			if (!validMatrixId(matrix_id))
			{
				sourceError(
					QStringLiteral("invalid-matrix-id"),
					item_path + QStringLiteral(".tileMatrix"),
					QStringLiteral("Invalid tile matrix identifier")
				);
			}
			auto const min_row = requiredInteger(
				object, QStringLiteral("minTileRow"),
				item_path + QStringLiteral(".minTileRow"), false,
				0, qint64(maximum_exact_integer)
			);
			auto const max_row = requiredInteger(
				object, QStringLiteral("maxTileRow"),
				item_path + QStringLiteral(".maxTileRow"), false,
				0, qint64(maximum_exact_integer)
			);
			auto const min_column = requiredInteger(
				object, QStringLiteral("minTileCol"),
				item_path + QStringLiteral(".minTileCol"), false,
				0, qint64(maximum_exact_integer)
			);
			auto const max_column = requiredInteger(
				object, QStringLiteral("maxTileCol"),
				item_path + QStringLiteral(".maxTileCol"), false,
				0, qint64(maximum_exact_integer)
			);
			if (ids.contains(matrix_id))
			{
				sourceError(
					QStringLiteral("duplicate-limit"),
					item_path + QStringLiteral(".tileMatrix"),
					QStringLiteral("Duplicate tile matrix limit")
				);
			}
			ids.insert(matrix_id);

			source.tile_limit_definitions.push_back({
				matrix_id, min_row, max_row, min_column, max_column
			});
			if (min_row > max_row || min_column > max_column)
			{
				sourceError(
					QStringLiteral("limit-bounds"), item_path,
					QStringLiteral(
						"Tile matrix limit minimum exceeds its maximum"
					)
				);
				continue;
			}

			auto const matrix_index = matrixIndex(source, matrix_id);
			if (matrix_index < 0)
			{
				if (!source.tile_matrix_set.matrix_set.matrices.isEmpty())
				{
					sourceError(
						QStringLiteral("unknown-limit-matrix"),
						item_path + QStringLiteral(".tileMatrix"),
						QStringLiteral(
							"Limit refers to an unknown tile matrix"
						)
					);
				}
				continue;
			}
			auto const& matrix =
				source.tile_matrix_set.matrix_set.matrices.at(matrix_index);
			if (min_row > max_row || min_column > max_column
			    || max_row >= matrix.matrix_height
			    || max_column >= matrix.matrix_width)
			{
				sourceError(
					QStringLiteral("limit-bounds"), item_path,
					QStringLiteral(
						"Tile matrix limit is outside the matrix dimensions"
					)
				);
				continue;
			}
			source.tile_limits.push_back({
				matrix.zoom, min_column, max_column, min_row, max_row
			});
		}
	}

	void validateMatrixRange(
		OicSourceDefinition& source,
		const QString& path)
	{
		if (source.tile_matrix_set.matrix_set.matrices.isEmpty())
			return;
		auto const& matrices = source.tile_matrix_set.matrix_set.matrices;
		if (source.min_tile_matrix.isEmpty())
			source.min_tile_matrix = matrices.first().id;
		if (source.max_tile_matrix.isEmpty())
			source.max_tile_matrix = matrices.last().id;
		auto const minimum = matrixIndex(source, source.min_tile_matrix);
		auto const maximum = matrixIndex(source, source.max_tile_matrix);
		if (minimum < 0)
		{
			sourceError(
				QStringLiteral("unknown-minimum-matrix"),
				path + QStringLiteral(".minTileMatrix"),
				QStringLiteral("Unknown minimum tile matrix")
			);
		}
		if (maximum < 0)
		{
			sourceError(
				QStringLiteral("unknown-maximum-matrix"),
				path + QStringLiteral(".maxTileMatrix"),
				QStringLiteral("Unknown maximum tile matrix")
			);
		}
		if (minimum >= 0 && maximum >= 0 && minimum > maximum)
		{
			sourceError(
				QStringLiteral("matrix-range-order"), path,
				QStringLiteral(
					"Minimum tile matrix follows maximum tile matrix"
				)
			);
		}
		for (auto const& limit : source.tile_limit_definitions)
		{
			auto const limit_index =
				matrixIndex(source, limit.tile_matrix);
			if (minimum >= 0 && maximum >= 0
			    && limit_index >= 0
			    && (limit_index < minimum || limit_index > maximum))
			{
				sourceError(
					QStringLiteral("limit-range"),
					path + QStringLiteral(".tileMatrixLimits"),
					QStringLiteral(
						"Tile matrix limit falls outside the usable range"
					)
				);
			}
		}
	}

	void validateRequest(
		const QJsonValue& value,
		const QString& path,
		OicSourceDefinition& source)
	{
		if (!value.isObject())
		{
			sourceError(
				QStringLiteral("request-type"), path,
				QStringLiteral("Request behavior must be an object")
			);
			return;
		}
		auto const object = value.toObject();
		static const QSet<QString> fields {
			QStringLiteral("referer"),
			QStringLiteral("emptyHttpStatusCodes"),
		};
		checkUnknownFields(object, fields, path, false);
		if (object.contains(QStringLiteral("referer")))
		{
			auto const url = httpUrl(
				requiredString(
					object, QStringLiteral("referer"),
					path + QStringLiteral(".referer"), false
				),
				path + QStringLiteral(".referer"), false
			);
			source.request.referer = url;
		}
		if (!object.contains(QStringLiteral("emptyHttpStatusCodes")))
			return;
		auto const codes =
			object.value(QStringLiteral("emptyHttpStatusCodes"));
		if (!codes.isArray())
		{
			sourceError(
				QStringLiteral("status-codes-type"),
				path + QStringLiteral(".emptyHttpStatusCodes"),
				QStringLiteral("HTTP codes must be an array")
			);
			return;
		}
		auto const array = codes.toArray();
		if (array.size() > OicCatalogReader::maximum_empty_status_codes)
		{
			sourceError(
				QStringLiteral("status-codes-count"),
				path + QStringLiteral(".emptyHttpStatusCodes"),
				QStringLiteral("Too many empty-tile HTTP codes")
			);
		}
		source.request.empty_http_status_codes.clear();
		QSet<int> unique;
		auto const count = std::min(
			array.size(),
			qsizetype(OicCatalogReader::maximum_empty_status_codes)
		);
		for (qsizetype index = 0; index < count; ++index)
		{
			auto const code = int(integerValue(
				array.at(index),
				path + QStringLiteral(".emptyHttpStatusCodes[%1]").arg(index),
				false, 100, 599
			));
			if (unique.contains(code))
			{
				sourceError(
					QStringLiteral("duplicate-status-code"),
					path + QStringLiteral(".emptyHttpStatusCodes[%1]")
						.arg(index),
					QStringLiteral("Duplicate HTTP status code")
				);
			}
			unique.insert(code);
			source.request.empty_http_status_codes.push_back(code);
		}
	}

	void validatePresentation(
		const QJsonObject& object,
		const QString& path,
		OicSourceDefinition& source)
	{
		if (object.contains(QStringLiteral("category")))
		{
			auto const category =
				requiredString(
					object, QStringLiteral("category"),
					path + QStringLiteral(".category"), false
				);
			auto const parsed = categoryFromName(category);
			if (!parsed)
			{
				sourceError(
					QStringLiteral("invalid-category"),
					path + QStringLiteral(".category"),
					QStringLiteral("Unknown source category")
				);
			}
			else
			{
				source.metadata.category = *parsed;
			}
		}
		source.metadata.start_date =
			optionalDate(
				object, QStringLiteral("startDate"),
				path + QStringLiteral(".startDate"), false
			);
		source.metadata.end_date =
			optionalDate(
				object, QStringLiteral("endDate"),
				path + QStringLiteral(".endDate"), false
			);
		if (source.metadata.start_date.isValid()
		    && source.metadata.end_date.isValid()
		    && source.metadata.start_date > source.metadata.end_date)
		{
			sourceError(
				QStringLiteral("date-order"), path,
				QStringLiteral("Source startDate follows endDate")
			);
		}
		if (object.contains(QStringLiteral("coverage")))
		{
			if (object.value(QStringLiteral("coverage")).isObject())
			{
				source.coverage =
					object.value(QStringLiteral("coverage")).toObject();
			}
			int vertices = 0;
			validateGeometry(
				object.value(QStringLiteral("coverage")),
				path + QStringLiteral(".coverage"), vertices
			);
			if (vertices > OicCatalogReader::maximum_coverage_vertices)
			{
				sourceError(
					QStringLiteral("coverage-limit"),
					path + QStringLiteral(".coverage"),
					QStringLiteral("Coverage exceeds the %1 vertex limit")
						.arg(OicCatalogReader::maximum_coverage_vertices)
				);
			}
		}
		if (object.contains(QStringLiteral("notices")))
		{
			validateNotices(
				object.value(QStringLiteral("notices")),
				path + QStringLiteral(".notices"), source.notices
			);
		}
	}

	void validateNotices(
		const QJsonValue& value,
		const QString& path,
		ImageryNotices& notices)
	{
		if (!value.isObject())
		{
			sourceError(
				QStringLiteral("notices-type"), path,
				QStringLiteral("Notices must be an object")
			);
			return;
		}
		auto const object = value.toObject();
		static const QSet<QString> fields {
			QStringLiteral("attributionText"),
			QStringLiteral("attributionUrl"),
			QStringLiteral("sourceUrl"), QStringLiteral("termsUrl"),
			QStringLiteral("privacyUrl"), QStringLiteral("notes"),
		};
		checkUnknownFields(object, fields, path, false);
		notices.attribution_text =
			optionalText(
				object, QStringLiteral("attributionText"),
				path + QStringLiteral(".attributionText"), false, 2048
			);
		notices.notes =
			optionalText(
				object, QStringLiteral("notes"),
				path + QStringLiteral(".notes"), false, 4096
			);
		for (auto const& field : {
			QStringLiteral("attributionUrl"), QStringLiteral("sourceUrl"),
			QStringLiteral("termsUrl"), QStringLiteral("privacyUrl")
		})
		{
			if (!object.contains(field))
				continue;
			auto const url =
				httpUrl(
					requiredString(
						object, field, path + QLatin1Char('.') + field,
						false
					),
					path + QLatin1Char('.') + field, false
				);
			if (field == QLatin1String("attributionUrl"))
				notices.attribution_url = url;
			else if (field == QLatin1String("sourceUrl"))
				notices.source_url = url;
			else if (field == QLatin1String("termsUrl"))
				notices.terms_url = url;
			else
				notices.privacy_url = url;
		}
	}

	void validateRegistration(
		const QJsonValue& value,
		const QString& path,
		OicSourceDefinition& source)
	{
		if (!value.isObject())
		{
			sourceError(
				QStringLiteral("registration-type"), path,
				QStringLiteral("Registration must be an object")
			);
			return;
		}
		auto const object = value.toObject();
		auto& registration = source.registration;
		registration.original_object = object;
		static const QSet<QString> fields {
			QStringLiteral("direction"), QStringLiteral("sourceFrame"),
			QStringLiteral("targetFrame"), QStringLiteral("operation"),
			QStringLiteral("provenance"),
		};
		checkUnknownFields(object, fields, path, false);
		registration.direction =
			requiredString(
				object, QStringLiteral("direction"),
				path + QStringLiteral(".direction"), false
			);
		if (registration.direction
		    != QLatin1String("source-to-corrected"))
		{
			sourceError(
				QStringLiteral("registration-direction"),
				path + QStringLiteral(".direction"),
				QStringLiteral(
					"Registration direction must be source-to-corrected"
				)
			);
		}
		registration.source_crs =
			validateFrame(
				object.value(QStringLiteral("sourceFrame")),
				path + QStringLiteral(".sourceFrame"), nullptr
			);
		registration.target_crs =
			validateFrame(
				object.value(QStringLiteral("targetFrame")),
				path + QStringLiteral(".targetFrame"),
				&registration.target_frame_id
			);
		if (!source.tile_matrix_set.matrix_set.crs.isEmpty()
		    && registration.source_crs
		         != source.tile_matrix_set.matrix_set.crs)
		{
			sourceError(
				QStringLiteral("registration-source-crs"),
				path + QStringLiteral(".sourceFrame.crs"),
				QStringLiteral(
					"Registration source frame must match the tile "
					"matrix set CRS"
				)
			);
		}
		if (registration.source_crs != registration.target_crs)
		{
			sourceError(
				QStringLiteral("registration-target-crs"),
				path + QStringLiteral(".targetFrame.crs"),
				QStringLiteral(
					"Version 1 registration frames must use the same CRS"
				)
			);
		}

		auto const operation_value = object.value(QStringLiteral("operation"));
		if (!operation_value.isObject())
		{
			sourceError(
				QStringLiteral("registration-operation"),
				path + QStringLiteral(".operation"),
				QStringLiteral("Registration operation must be an object")
			);
			return;
		}
		auto const operation = operation_value.toObject();
		auto const operation_path = path + QStringLiteral(".operation");
		auto const type =
			requiredString(
				operation, QStringLiteral("type"),
				operation_path + QStringLiteral(".type"), false
			);
		if (type == QLatin1String("translation2d"))
		{
			validateTranslation(operation, operation_path, source);
		}
		else if (type == QLatin1String("affine2d"))
		{
			validateAffine(operation, operation_path, source);
			addUnsupported(
				source, QStringLiteral("registration.affine2d.v1"),
				operation_path,
				QStringLiteral(
					"Affine registration is parsed but not executable"
				)
			);
		}
		else if (type == QLatin1String("gridShift"))
		{
			validateGridShift(operation, operation_path, source);
			addUnsupported(
				source, QStringLiteral("registration.grid-shift.v1"),
				operation_path,
				QStringLiteral(
					"Grid-shift registration is parsed but not executable"
				)
			);
		}
		else
		{
			sourceError(
				QStringLiteral("unknown-registration"),
				operation_path + QStringLiteral(".type"),
				QStringLiteral("Unknown registration operation")
			);
		}
		if (object.contains(QStringLiteral("provenance")))
		{
			validateProvenance(
				object.value(QStringLiteral("provenance")),
				path + QStringLiteral(".provenance"),
				registration.provenance
			);
		}
	}

	void validateTranslation(
		const QJsonObject& object,
		const QString& path,
		OicSourceDefinition& source)
	{
		static const QSet<QString> fields {
			QStringLiteral("type"), QStringLiteral("unit"),
			QStringLiteral("dx"), QStringLiteral("dy"),
		};
		checkUnknownFields(object, fields, path, false);
		auto& registration = source.registration;
		registration.kind = OicRegistrationKind::Translation2d;
		registration.unit =
			requiredString(
				object, QStringLiteral("unit"),
				path + QStringLiteral(".unit"), false
			);
		if (registration.unit != QLatin1String("crs"))
		{
			sourceError(
				QStringLiteral("registration-unit"),
				path + QStringLiteral(".unit"),
				QStringLiteral("Translation unit must be crs")
			);
		}
		registration.dx =
			requiredNumber(
				object, QStringLiteral("dx"),
				path + QStringLiteral(".dx")
			);
		registration.dy =
			requiredNumber(
				object, QStringLiteral("dy"),
				path + QStringLiteral(".dy")
			);
	}

	void validateAffine(
		const QJsonObject& object,
		const QString& path,
		OicSourceDefinition& source)
	{
		static const QSet<QString> fields {
			QStringLiteral("type"), QStringLiteral("unit"),
			QStringLiteral("xoff"), QStringLiteral("yoff"),
			QStringLiteral("s11"), QStringLiteral("s12"),
			QStringLiteral("s21"), QStringLiteral("s22"),
		};
		checkUnknownFields(object, fields, path, false);
		auto& registration = source.registration;
		registration.kind = OicRegistrationKind::Affine2d;
		registration.unit =
			requiredString(
				object, QStringLiteral("unit"),
				path + QStringLiteral(".unit"), false
			);
		if (registration.unit != QLatin1String("crs"))
		{
			sourceError(
				QStringLiteral("registration-unit"),
				path + QStringLiteral(".unit"),
				QStringLiteral("Affine unit must be crs")
			);
		}
		registration.xoff =
			requiredNumber(
				object, QStringLiteral("xoff"),
				path + QStringLiteral(".xoff")
			);
		registration.yoff =
			requiredNumber(
				object, QStringLiteral("yoff"),
				path + QStringLiteral(".yoff")
			);
		registration.s11 =
			requiredNumber(
				object, QStringLiteral("s11"),
				path + QStringLiteral(".s11")
			);
		registration.s12 =
			requiredNumber(
				object, QStringLiteral("s12"),
				path + QStringLiteral(".s12")
			);
		registration.s21 =
			requiredNumber(
				object, QStringLiteral("s21"),
				path + QStringLiteral(".s21")
			);
		registration.s22 =
			requiredNumber(
				object, QStringLiteral("s22"),
				path + QStringLiteral(".s22")
			);
		auto const determinant =
			registration.s11 * registration.s22
			- registration.s12 * registration.s21;
		if (!std::isfinite(determinant) || determinant == 0)
		{
			sourceError(
				QStringLiteral("singular-affine"), path,
				QStringLiteral("Affine registration must be invertible")
			);
		}
	}

	void validateGridShift(
		const QJsonObject& object,
		const QString& path,
		OicSourceDefinition& source)
	{
		static const QSet<QString> fields {
			QStringLiteral("type"), QStringLiteral("resource"),
			QStringLiteral("domain"), QStringLiteral("gridFrame"),
			QStringLiteral("interpolation"),
		};
		checkUnknownFields(object, fields, path, false);
		auto& registration = source.registration;
		registration.kind = OicRegistrationKind::GridShift;
		registration.resource_id =
			requiredId(
				object, QStringLiteral("resource"),
				path + QStringLiteral(".resource"), false
			);
		if (!result.catalog.resource(registration.resource_id))
		{
			sourceError(
				QStringLiteral("unknown-grid-resource"),
				path + QStringLiteral(".resource"),
				QStringLiteral(
					"Grid shift refers to an undeclared resource"
				)
			);
		}
		registration.grid_domain =
			requiredString(
				object, QStringLiteral("domain"),
				path + QStringLiteral(".domain"), false
			);
		if (!QSet<QString> {
			QStringLiteral("horizontal"), QStringLiteral("vertical"),
			QStringLiteral("horizontal-and-vertical"),
		}.contains(registration.grid_domain))
		{
			sourceError(
				QStringLiteral("grid-domain"),
				path + QStringLiteral(".domain"),
				QStringLiteral("Unknown grid-shift domain")
			);
		}
		registration.grid_crs =
			validateFrame(
				object.value(QStringLiteral("gridFrame")),
				path + QStringLiteral(".gridFrame"), nullptr
			);
		registration.interpolation =
			requiredString(
				object, QStringLiteral("interpolation"),
				path + QStringLiteral(".interpolation"), false
			);
		if (!QSet<QString> {
			QStringLiteral("bilinear"), QStringLiteral("biquadratic"),
			QStringLiteral("bicubic"),
		}.contains(registration.interpolation))
		{
			sourceError(
				QStringLiteral("grid-interpolation"),
				path + QStringLiteral(".interpolation"),
				QStringLiteral("Unknown grid-shift interpolation")
			);
		}
	}

	QString validateFrame(
		const QJsonValue& value,
		const QString& path,
		QString* id)
	{
		if (!value.isObject())
		{
			sourceError(
				QStringLiteral("frame-type"), path,
				QStringLiteral("Frame must be an object")
			);
			return {};
		}
		auto const object = value.toObject();
		static const QSet<QString> fields {
			QStringLiteral("crs"), QStringLiteral("id"),
		};
		checkUnknownFields(object, fields, path, false);
		auto const frame_id =
			optionalId(
				object, QStringLiteral("id"),
				path + QStringLiteral(".id"), false
			);
		if (id)
			*id = frame_id;
		return normalizeCrs(
			requiredString(
				object, QStringLiteral("crs"),
				path + QStringLiteral(".crs"), false
			),
			path + QStringLiteral(".crs")
		);
	}

	void validateProvenance(
		const QJsonValue& value,
		const QString& path,
		ImageryProvenance& provenance)
	{
		if (!value.isObject())
		{
			sourceError(
				QStringLiteral("provenance-type"), path,
				QStringLiteral("Provenance must be an object")
			);
			return;
		}
		auto const object = value.toObject();
		static const QSet<QString> fields {
			QStringLiteral("method"), QStringLiteral("observed"),
			QStringLiteral("author"), QStringLiteral("rmsError"),
			QStringLiteral("notes"),
		};
		checkUnknownFields(object, fields, path, false);
		provenance.method =
			optionalText(
				object, QStringLiteral("method"),
				path + QStringLiteral(".method"), false, 256
			);
		provenance.observed =
			optionalDate(
				object, QStringLiteral("observed"),
				path + QStringLiteral(".observed"), false
			);
		provenance.author =
			optionalText(
				object, QStringLiteral("author"),
				path + QStringLiteral(".author"), false, 512
			);
		provenance.notes =
			optionalText(
				object, QStringLiteral("notes"),
				path + QStringLiteral(".notes"), false, 4096
			);
		if (object.contains(QStringLiteral("rmsError")))
		{
			auto const rms =
				numberValue(
					object.value(QStringLiteral("rmsError")),
					path + QStringLiteral(".rmsError"), false
				);
			if (rms < 0)
			{
				sourceError(
					QStringLiteral("negative-rms"),
					path + QStringLiteral(".rmsError"),
					QStringLiteral("RMS error must be nonnegative")
				);
			}
			provenance.rms_error = rms;
		}
	}

	void validateResources(
		const QJsonValue& value,
		const QString& path)
	{
		if (!value.isObject())
		{
			catalogError(
				QStringLiteral("resources-type"), path,
				QStringLiteral("Resources must be an object")
			);
			return;
		}
		auto const resources = value.toObject();
		if (resources.size() > OicCatalogReader::maximum_resources)
		{
			catalogError(
				QStringLiteral("resource-limit"), path,
				QStringLiteral("Catalog exceeds the %1 resource limit")
					.arg(OicCatalogReader::maximum_resources)
			);
		}
		auto processed = 0;
		for (auto it = resources.begin();
		     it != resources.end()
		     && processed < OicCatalogReader::maximum_resources;
		     ++it, ++processed)
		{
			auto const item_path = path + QLatin1Char('.') + it.key();
			OicResource resource;
			resource.id = it.key();
			if (!validId(resource.id))
			{
				catalogError(
					QStringLiteral("invalid-resource-id"), item_path,
					QStringLiteral("Invalid resource ID")
				);
			}
			if (!it.value().isObject())
			{
				catalogError(
					QStringLiteral("resource-type"), item_path,
					QStringLiteral("Resource must be an object")
				);
				continue;
			}
			auto const object = it.value().toObject();
			resource.original_object = object;
			static const QSet<QString> fields {
				QStringLiteral("href"), QStringLiteral("mediaType"),
				QStringLiteral("sha256"), QStringLiteral("size"),
			};
			checkUnknownFields(object, fields, item_path, true);
			resource.href =
				requiredString(
					object, QStringLiteral("href"),
					item_path + QStringLiteral(".href"), true
				);
			validateResourceHref(
				resource.href, item_path + QStringLiteral(".href")
			);
			resource.media_type =
				requiredText(
					object, QStringLiteral("mediaType"),
					item_path + QStringLiteral(".mediaType"), true, 255
				);
			auto const digest =
				requiredString(
					object, QStringLiteral("sha256"),
					item_path + QStringLiteral(".sha256"), true
				);
			static const QRegularExpression digest_pattern(
				QStringLiteral("^[0-9a-f]{64}$")
			);
			if (!digest_pattern.match(digest).hasMatch())
			{
				catalogError(
					QStringLiteral("resource-digest"),
					item_path + QStringLiteral(".sha256"),
					QStringLiteral(
						"Resource SHA-256 must contain 64 lowercase "
						"hexadecimal characters"
					)
				);
			}
			resource.sha256 = digest.toLatin1();
			resource.size =
				requiredInteger(
					object, QStringLiteral("size"),
					item_path + QStringLiteral(".size"), true,
					1, 1073741824
				);
			result.catalog.resources.push_back(std::move(resource));
		}
	}

	void validateResourceHref(
		const QString& href,
		const QString& path)
	{
		if (href.isEmpty()
		    || href.size() > OicCatalogReader::maximum_url_length
		    || containsUrlWhitespaceOrControl(href))
		{
			catalogError(
				QStringLiteral("resource-href"), path,
				QStringLiteral("Resource href is empty, too long, or unsafe")
			);
			return;
		}
		auto const url = QUrl(href, QUrl::StrictMode);
		if (!url.isValid())
		{
			catalogError(
				QStringLiteral("resource-href"), path,
				QStringLiteral("Resource href is invalid")
			);
			return;
		}
		if (url.hasFragment())
		{
			catalogError(
				QStringLiteral("resource-fragment"), path,
				QStringLiteral("Resource href must not contain a fragment")
			);
			return;
		}
		if (url.isRelative())
		{
			auto const decoded_path = url.path(QUrl::FullyDecoded);
			auto const segments = decoded_path.split(QLatin1Char('/'));
			if (href.startsWith(QLatin1Char('/'))
			    || segments.contains(QStringLiteral(".."))
			    || decoded_path.contains(QLatin1Char('\\')))
			{
				catalogError(
					QStringLiteral("resource-path"), path,
					QStringLiteral("Unsafe relative resource path")
				);
			}
			return;
		}
		if (url.scheme().toLower() != QLatin1String("https")
		    || url.host().isEmpty()
		    || !url.userName().isEmpty() || !url.password().isEmpty())
		{
			catalogError(
				QStringLiteral("resource-url"), path,
				QStringLiteral(
					"Remote resources must use HTTPS without user "
					"information or fragments"
				)
			);
		}
	}

	OicPublisher validatePublisher(
		const QJsonValue& value,
		const QString& path)
	{
		OicPublisher publisher;
		if (!value.isObject())
		{
			catalogError(
				QStringLiteral("publisher-type"), path,
				QStringLiteral("Publisher must be an object")
			);
			return publisher;
		}
		auto const object = value.toObject();
		publisher.original_object = object;
		static const QSet<QString> fields {
			QStringLiteral("name"), QStringLiteral("url"),
			QStringLiteral("contactUrl"),
		};
		checkUnknownFields(object, fields, path, true);
		publisher.name =
			requiredText(
				object, QStringLiteral("name"),
				path + QStringLiteral(".name"), true, 512
			);
		if (object.contains(QStringLiteral("url")))
		{
			publisher.url =
				httpUrl(
					requiredString(
						object, QStringLiteral("url"),
						path + QStringLiteral(".url"), true
					),
					path + QStringLiteral(".url"), true
				);
		}
		if (object.contains(QStringLiteral("contactUrl")))
		{
			publisher.contact_url =
				httpUrl(
					requiredString(
						object, QStringLiteral("contactUrl"),
						path + QStringLiteral(".contactUrl"), true
					),
					path + QStringLiteral(".contactUrl"), true
				);
		}
		return publisher;
	}

	void validateBoundingBox(
		const QJsonValue& value,
		const QString& path)
	{
		if (!value.isObject())
		{
			sourceError(
				QStringLiteral("bounding-box-type"), path,
				QStringLiteral("Bounding box must be an object")
			);
			return;
		}
		auto const object = value.toObject();
		static const QSet<QString> fields {
			QStringLiteral("crs"), QStringLiteral("orderedAxes"),
			QStringLiteral("lowerLeft"), QStringLiteral("upperRight"),
		};
		checkUnknownFields(object, fields, path, false);
		if (object.contains(QStringLiteral("crs")))
		{
			normalizeCrs(
				requiredString(
					object, QStringLiteral("crs"),
					path + QStringLiteral(".crs"), false
				),
				path + QStringLiteral(".crs")
			);
		}
		if (object.contains(QStringLiteral("orderedAxes")))
		{
			auto const axes =
				validateStringArray(
					object.value(QStringLiteral("orderedAxes")),
					path + QStringLiteral(".orderedAxes"),
					false, false, 2
				);
			if (axes.size() != 2)
			{
				sourceError(
					QStringLiteral("axis-count"),
					path + QStringLiteral(".orderedAxes"),
					QStringLiteral(
						"orderedAxes must contain exactly two axes"
					)
				);
			}
		}
		validatePosition(
			object.value(QStringLiteral("lowerLeft")),
			path + QStringLiteral(".lowerLeft")
		);
		validatePosition(
			object.value(QStringLiteral("upperRight")),
			path + QStringLiteral(".upperRight")
		);
	}

	void validateGeometry(
		const QJsonValue& value,
		const QString& path,
		int& vertices)
	{
		if (!value.isObject())
		{
			sourceError(
				QStringLiteral("coverage-type"), path,
				QStringLiteral("Coverage geometry must be an object")
			);
			return;
		}
		auto const object = value.toObject();
		auto const type =
			requiredString(
				object, QStringLiteral("type"),
				path + QStringLiteral(".type"), false
			);
		if (type == QLatin1String("GeometryCollection"))
		{
			static const QSet<QString> fields {
				QStringLiteral("type"), QStringLiteral("geometries"),
			};
			checkUnknownFields(object, fields, path, false);
			auto const geometries =
				object.value(QStringLiteral("geometries"));
			if (!geometries.isArray())
			{
				sourceError(
					QStringLiteral("geometry-collection"),
					path + QStringLiteral(".geometries"),
					QStringLiteral(
						"Geometry collection must contain an array"
					)
				);
				return;
			}
			auto const array = geometries.toArray();
			for (qsizetype index = 0;
			     index < array.size()
			     && vertices <= OicCatalogReader::maximum_coverage_vertices;
			     ++index)
			{
				validateGeometry(
					array.at(index),
					path + QStringLiteral(".geometries[%1]").arg(index),
					vertices
				);
			}
			return;
		}

		static const QSet<QString> fields {
			QStringLiteral("type"), QStringLiteral("coordinates"),
		};
		checkUnknownFields(object, fields, path, false);
		if (!object.contains(QStringLiteral("coordinates")))
		{
			sourceError(
				QStringLiteral("coverage-coordinates"),
				path + QStringLiteral(".coordinates"),
				QStringLiteral("Geometry coordinates are required")
			);
			return;
		}
		auto coordinate_depth = -1;
		if (type == QLatin1String("Point"))
			coordinate_depth = 0;
		else if (type == QLatin1String("MultiPoint")
		         || type == QLatin1String("LineString"))
			coordinate_depth = 1;
		else if (type == QLatin1String("MultiLineString")
		         || type == QLatin1String("Polygon"))
			coordinate_depth = 2;
		else if (type == QLatin1String("MultiPolygon"))
			coordinate_depth = 3;
		else
		{
			sourceError(
				QStringLiteral("geometry-type"),
				path + QStringLiteral(".type"),
				QStringLiteral("Unknown GeoJSON geometry type")
			);
		}
		if (coordinate_depth < 0)
			return;
		auto const coordinates_path = path + QStringLiteral(".coordinates");
		validateCoordinates(
			object.value(QStringLiteral("coordinates")),
			coordinates_path, coordinate_depth, vertices
		);
		if (object.value(QStringLiteral("coordinates")).isArray())
		{
			validateGeometryShape(
				type,
				object.value(QStringLiteral("coordinates")).toArray(),
				coordinates_path
			);
		}
	}

	void validateGeometryShape(
		const QString& type,
		const QJsonArray& coordinates,
		const QString& path)
	{
		if (type == QLatin1String("LineString")
		    && coordinates.size() < 2)
		{
			sourceError(
				QStringLiteral("line-size"), path,
				QStringLiteral(
					"LineString must contain at least two positions"
				)
			);
		}
		else if (type == QLatin1String("MultiLineString"))
		{
			for (qsizetype index = 0; index < coordinates.size(); ++index)
			{
				if (coordinates.at(index).isArray()
				    && coordinates.at(index).toArray().size() < 2)
				{
					sourceError(
						QStringLiteral("line-size"),
						path + QStringLiteral("[%1]").arg(index),
						QStringLiteral(
							"LineString must contain at least two positions"
						)
					);
				}
			}
		}
		else if (type == QLatin1String("Polygon"))
		{
			validatePolygonRings(coordinates, path);
		}
		else if (type == QLatin1String("MultiPolygon"))
		{
			for (qsizetype index = 0; index < coordinates.size(); ++index)
			{
				if (coordinates.at(index).isArray())
				{
					validatePolygonRings(
						coordinates.at(index).toArray(),
						path + QStringLiteral("[%1]").arg(index)
					);
				}
			}
		}
	}

	void validatePolygonRings(
		const QJsonArray& rings,
		const QString& path)
	{
		for (qsizetype index = 0; index < rings.size(); ++index)
		{
			if (!rings.at(index).isArray())
				continue;
			auto const ring = rings.at(index).toArray();
			auto const ring_path =
				path + QStringLiteral("[%1]").arg(index);
			if (ring.size() < 4)
			{
				sourceError(
					QStringLiteral("ring-size"), ring_path,
					QStringLiteral(
						"Polygon ring must contain at least four positions"
					)
				);
			}
			else if (ring.first() != ring.last())
			{
				sourceError(
					QStringLiteral("ring-closure"), ring_path,
					QStringLiteral("Polygon ring must be closed")
				);
			}
		}
	}

	void validateCoordinates(
		const QJsonValue& value,
		const QString& path,
		int depth,
		int& vertices)
	{
		if (!value.isArray())
		{
			sourceError(
				QStringLiteral("coordinate-type"), path,
				QStringLiteral("GeoJSON coordinates must be arrays")
			);
			return;
		}
		auto const array = value.toArray();
		if (array.isEmpty())
		{
			sourceError(
				QStringLiteral("coordinate-empty"), path,
				QStringLiteral(
					"GeoJSON coordinate array must not be empty"
				)
			);
		}
		if (depth == 0)
		{
			if (array.size() < 2 || array.size() > 3
			    || !finiteNumber(array.at(0))
			    || !finiteNumber(array.at(1))
			    || (array.size() == 3 && !finiteNumber(array.at(2))))
			{
				sourceError(
					QStringLiteral("position"), path,
					QStringLiteral(
						"GeoJSON position must contain two or three "
						"finite numbers"
					)
				);
				return;
			}
			auto const longitude = array.at(0).toDouble();
			auto const latitude = array.at(1).toDouble();
			if (longitude < -180 || longitude > 180
			    || latitude < -90 || latitude > 90)
			{
				sourceError(
					QStringLiteral("position-bounds"), path,
					QStringLiteral(
						"Coverage position is outside WGS84 "
						"longitude/latitude bounds"
					)
				);
			}
			++vertices;
			return;
		}
		for (qsizetype index = 0;
		     index < array.size()
		     && vertices <= OicCatalogReader::maximum_coverage_vertices;
		     ++index)
		{
			validateCoordinates(
				array.at(index),
				path + QStringLiteral("[%1]").arg(index),
				depth - 1, vertices
			);
		}
	}

	void validatePosition(
		const QJsonValue& value,
		const QString& path)
	{
		if (!value.isArray() || value.toArray().size() != 2
		    || !finiteNumber(value.toArray().at(0))
		    || !finiteNumber(value.toArray().at(1)))
		{
			sourceError(
				QStringLiteral("position"), path,
				QStringLiteral("Expected a two-number position")
			);
		}
	}

	void resolveSource(
		OicSourceDefinition& definition,
		const QString& path)
	{
		auto const minimum =
			matrixIndex(definition, definition.min_tile_matrix);
		auto const maximum =
			matrixIndex(definition, definition.max_tile_matrix);
		if (minimum < 0 || maximum < minimum)
		{
			sourceError(
				QStringLiteral("runtime-matrix-range"), path,
				QStringLiteral(
					"Supported source has no executable tile matrix range"
				)
			);
			definition.valid = false;
			definition.supported = false;
			return;
		}
		for (auto index = minimum; index <= maximum; ++index)
		{
			if (!runtimeSupportsTileSize(
				    definition.tile_matrix_set.matrix_set.matrices
					    .at(index).tile_size))
			{
				addUnsupported(
					definition,
					QStringLiteral("tile-size.runtime.v1"),
					path + QStringLiteral(".tileMatrixSet"),
					QStringLiteral(
						"Tile dimensions exceed this build's raster execution profile")
				);
				definition.supported = false;
				return;
			}
		}

		ResolvedImagerySource source;
		source.metadata = definition.metadata;
		source.notices = definition.notices;
		source.tile_urls = definition.tile_urls;
		source.row_scheme = definition.row_scheme;
		source.media_type = definition.media_type;
		source.tile_matrix_set = definition.tile_matrix_set.matrix_set;
		source.min_zoom =
			source.tile_matrix_set.matrices.at(minimum).zoom;
		source.max_zoom =
			source.tile_matrix_set.matrices.at(maximum).zoom;
		source.tile_limits = definition.tile_limits;
		source.request = definition.request;
		source.catalog_provenance = CatalogSourceProvenance {
			result.catalog.id,
			result.catalog.revision,
			result.catalog.document_sha256,
			definition.metadata.id,
			definition.full_fingerprint,
			definition.operational_fingerprint,
		};
		if (definition.registration.kind
		    == OicRegistrationKind::Translation2d)
		{
			source.registration = TranslationRegistration {
				definition.registration.source_crs,
				definition.registration.target_crs,
				definition.registration.target_frame_id,
				definition.registration.dx,
				definition.registration.dy,
				definition.registration.provenance,
			};
		}

		QString error;
		if (!source.validate(&error))
		{
			sourceError(
				QStringLiteral("runtime-contract"), path,
				QStringLiteral(
					"Source cannot satisfy the resolved runtime contract: %1"
				).arg(error)
			);
			definition.valid = false;
			definition.supported = false;
			return;
		}
		definition.resolved_source = std::move(source);
	}

	QString normalizeCrs(
		const QString& value,
		const QString& path)
	{
		static const QRegularExpression short_form(
			QStringLiteral("^EPSG:([1-9][0-9]{0,8})$")
		);
		static const QRegularExpression uri_form(
			QStringLiteral(
				"^https?://www\\.opengis\\.net/def/crs/EPSG/"
				"(?:0|[0-9.]+)/([1-9][0-9]{0,8})$"
			)
		);
		auto match = short_form.match(value);
		if (!match.hasMatch())
			match = uri_form.match(value);
		if (!match.hasMatch())
		{
			sourceError(
				QStringLiteral("invalid-crs"), path,
				QStringLiteral(
					"CRS must be an EPSG code or equivalent OGC URI"
				)
			);
			return {};
		}
		return QStringLiteral("EPSG:%1").arg(match.captured(1));
	}

	QStringList validateCapabilities(
		const QJsonValue& value,
		const QString& path,
		bool catalog_level)
	{
		auto const result =
			validateStringArray(
				value, path, catalog_level, true, 64
			);
		for (qsizetype index = 0; index < result.size(); ++index)
		{
			if (!validId(result.at(index)))
			{
				addError(
					catalog_level,
					QStringLiteral("invalid-capability"),
					path + QStringLiteral("[%1]").arg(index),
					QStringLiteral("Invalid capability identifier")
				);
			}
		}
		return result;
	}

	QStringList validateStringArray(
		const QJsonValue& value,
		const QString& path,
		bool catalog_level,
		bool unique,
		int maximum)
	{
		QStringList result;
		if (!value.isArray())
		{
			addError(
				catalog_level, QStringLiteral("string-array"), path,
				QStringLiteral("Expected an array of strings")
			);
			return result;
		}
		auto const array = value.toArray();
		if (array.size() > maximum)
		{
			addError(
				catalog_level, QStringLiteral("array-limit"), path,
				QStringLiteral("Array exceeds the %1 item limit").arg(maximum)
			);
		}
		QSet<QString> seen;
		auto const count =
			std::min(array.size(), qsizetype(maximum));
		for (qsizetype index = 0; index < count; ++index)
		{
			auto const item_path = path + QStringLiteral("[%1]").arg(index);
			if (!array.at(index).isString()
			    || array.at(index).toString().isEmpty()
			    || containsControl(array.at(index).toString()))
			{
				addError(
					catalog_level, QStringLiteral("string-array"),
					item_path,
					QStringLiteral(
						"Expected a nonempty string without controls"
					)
				);
				continue;
			}
			auto const text = array.at(index).toString();
			if (unique && seen.contains(text))
			{
				addError(
					catalog_level, QStringLiteral("duplicate-array-value"),
					item_path, QStringLiteral("Duplicate array value")
				);
			}
			seen.insert(text);
			result.push_back(text);
		}
		return result;
	}

	void validateExtensions(
		const QJsonObject& object,
		const QString& path,
		bool catalog_level)
	{
		if (object.size() > 128)
		{
			addError(
				catalog_level, QStringLiteral("extension-limit"), path,
				QStringLiteral("Extensions exceed the 128-key limit")
			);
		}
		static const QRegularExpression namespaced(
			QStringLiteral("^[A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)+$")
		);
		for (auto it = object.begin(); it != object.end(); ++it)
		{
			if (!namespaced.match(it.key()).hasMatch())
			{
				addError(
					catalog_level, QStringLiteral("extension-namespace"),
					path + QLatin1Char('.') + it.key(),
					QStringLiteral(
						"Extension key must be reverse-DNS namespaced"
					)
				);
			}
		}
	}

	void checkUnknownFields(
		const QJsonObject& object,
		const QSet<QString>& allowed,
		const QString& path,
		bool catalog_level)
	{
		for (auto it = object.begin(); it != object.end(); ++it)
		{
			if (!allowed.contains(it.key()))
			{
				addError(
					catalog_level, QStringLiteral("unknown-member"),
					path + QLatin1Char('.') + it.key(),
					QStringLiteral("Unknown member")
				);
			}
		}
	}

	QString requiredText(
		const QJsonObject& object,
		const QString& name,
		const QString& path,
		bool catalog_level,
		qsizetype maximum)
	{
		auto const text =
			requiredString(object, name, path, catalog_level);
		validateText(text, path, catalog_level, maximum);
		return text;
	}

	QString optionalText(
		const QJsonObject& object,
		const QString& name,
		const QString& path,
		bool catalog_level,
		qsizetype maximum)
	{
		if (!object.contains(name))
			return {};
		auto const text =
			requiredString(object, name, path, catalog_level);
		validateText(text, path, catalog_level, maximum);
		return text;
	}

	void validateText(
		const QString& text,
		const QString& path,
		bool catalog_level,
		qsizetype maximum)
	{
		if (text.isEmpty() || text.size() > maximum || containsControl(text))
		{
			addError(
				catalog_level, QStringLiteral("invalid-text"), path,
				QStringLiteral(
					"Text must be nonempty, bounded, and free of controls"
				)
			);
		}
	}

	QString requiredId(
		const QJsonObject& object,
		const QString& name,
		const QString& path,
		bool catalog_level)
	{
		auto const id =
			requiredString(object, name, path, catalog_level);
		if (!id.isEmpty() && !validId(id))
		{
			addError(
				catalog_level, QStringLiteral("invalid-id"), path,
				QStringLiteral("Invalid identifier")
			);
		}
		return id;
	}

	QString optionalId(
		const QJsonObject& object,
		const QString& name,
		const QString& path,
		bool catalog_level)
	{
		if (!object.contains(name))
			return {};
		return requiredId(object, name, path, catalog_level);
	}

	bool validId(const QString& value) const
	{
		static const QRegularExpression pattern(
			QStringLiteral(
				"^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,253}[A-Za-z0-9])?$"
			)
		);
		return pattern.match(value).hasMatch();
	}

	bool validRuntimeId(const QString& value) const
	{
		static const QRegularExpression pattern(
			QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
		);
		return pattern.match(value).hasMatch();
	}

	bool validMatrixId(const QString& value) const
	{
		return !value.isEmpty() && value.size() <= 64
		       && !containsControl(value);
	}

	QString requiredString(
		const QJsonObject& object,
		const QString& name,
		const QString& path,
		bool catalog_level)
	{
		if (!object.contains(name) || !object.value(name).isString())
		{
			addError(
				catalog_level, QStringLiteral("required-string"), path,
				QStringLiteral("Required member must be a string")
			);
			return {};
		}
		return object.value(name).toString();
	}

	QString optionalString(
		const QJsonObject& object,
		const QString& name,
		const QString& path,
		bool catalog_level)
	{
		if (!object.contains(name))
			return {};
		auto const value =
			requiredString(object, name, path, catalog_level);
		if (value.isEmpty())
		{
			addError(
				catalog_level, QStringLiteral("empty-string"), path,
				QStringLiteral("String must not be empty")
			);
		}
		return value;
	}

	QDate optionalDate(
		const QJsonObject& object,
		const QString& name,
		const QString& path,
		bool catalog_level)
	{
		if (!object.contains(name))
			return {};
		auto const text =
			requiredString(object, name, path, catalog_level);
		auto const date = QDate::fromString(text, Qt::ISODate);
		if (!date.isValid() || date.toString(Qt::ISODate) != text)
		{
			addError(
				catalog_level, QStringLiteral("invalid-date"), path,
				QStringLiteral("Date must use YYYY-MM-DD ISO 8601 form")
			);
		}
		return date;
	}

	QUrl absoluteUrl(
		const QString& text,
		const QString& path,
		bool catalog_level)
	{
		if (text.isEmpty()
		    || text.size() > OicCatalogReader::maximum_url_length
		    || containsUrlWhitespaceOrControl(text)
		    || urlAuthorityContainsUserInfo(text))
		{
			addError(
				catalog_level, QStringLiteral("invalid-url"), path,
				QStringLiteral("URL is empty, too long, or contains controls")
			);
			return {};
		}
		auto const url = QUrl(text, QUrl::StrictMode);
		if (!url.isValid() || url.isRelative()
		    || !url.userName().isEmpty() || !url.password().isEmpty()
		    || url.hasFragment())
		{
			addError(
				catalog_level, QStringLiteral("invalid-url"), path,
				QStringLiteral(
					"URL must be absolute without user information "
					"or a fragment"
				)
			);
			return {};
		}
		return url;
	}

	QUrl httpUrl(
		const QString& text,
		const QString& path,
		bool catalog_level)
	{
		auto const url = absoluteUrl(text, path, catalog_level);
		if (url.isEmpty())
			return {};
		auto const scheme = url.scheme().toLower();
		if ((scheme != QLatin1String("http")
		     && scheme != QLatin1String("https"))
		    || url.host().isEmpty())
		{
			addError(
				catalog_level, QStringLiteral("invalid-http-url"), path,
				QStringLiteral("URL must use HTTP or HTTPS and contain a host")
			);
			return {};
		}
		return url;
	}

	double requiredPositiveNumber(
		const QJsonObject& object,
		const QString& name,
		const QString& path)
	{
		auto const value = requiredNumber(object, name, path);
		if (!(value > 0))
		{
			sourceError(
				QStringLiteral("positive-number"), path,
				QStringLiteral("Number must be positive")
			);
		}
		return value;
	}

	double requiredNumber(
		const QJsonObject& object,
		const QString& name,
		const QString& path)
	{
		if (!object.contains(name))
		{
			sourceError(
				QStringLiteral("required-number"), path,
				QStringLiteral("Required numeric member is missing")
			);
			return 0;
		}
		return numberValue(object.value(name), path, false);
	}

	bool finiteNumber(const QJsonValue& value) const
	{
		return value.isDouble() && std::isfinite(value.toDouble());
	}

	double numberValue(
		const QJsonValue& value,
		const QString& path,
		bool catalog_level)
	{
		if (!finiteNumber(value))
		{
			addError(
				catalog_level, QStringLiteral("finite-number"), path,
				QStringLiteral("Expected a finite number")
			);
			return 0;
		}
		return value.toDouble();
	}

	qint64 requiredInteger(
		const QJsonObject& object,
		const QString& name,
		const QString& path,
		bool catalog_level,
		qint64 minimum,
		qint64 maximum)
	{
		if (!object.contains(name))
		{
			addError(
				catalog_level, QStringLiteral("required-integer"), path,
				QStringLiteral("Required integer member is missing")
			);
			return minimum;
		}
		return integerValue(
			object.value(name), path, catalog_level, minimum, maximum
		);
	}

	qint64 integerValue(
		const QJsonValue& value,
		const QString& path,
		bool catalog_level,
		qint64 minimum,
		qint64 maximum)
	{
		auto const number = numberValue(value, path, catalog_level);
		if (std::floor(number) != number
		    || number < double(minimum) || number > double(maximum)
		    || std::abs(number) > maximum_exact_integer)
		{
			addError(
				catalog_level, QStringLiteral("integer-range"), path,
				QStringLiteral(
					"Integer is outside its permitted exact range"
				)
			);
			return minimum;
		}
		return qint64(number);
	}

	QJsonObject objectValue(
		const QJsonValue& value,
		const QString& path,
		bool catalog_level)
	{
		if (!value.isObject())
		{
			addError(
				catalog_level, QStringLiteral("object-type"), path,
				QStringLiteral("Expected an object")
			);
			return {};
		}
		return value.toObject();
	}

	int matrixIndex(
		const OicSourceDefinition& source,
		const QString& id) const
	{
		auto const& matrices = source.tile_matrix_set.matrix_set.matrices;
		for (qsizetype index = 0; index < matrices.size(); ++index)
		{
			if (matrices.at(index).id == id)
				return int(index);
		}
		return -1;
	}

	bool hasSourceErrors(qsizetype start) const
	{
		for (qsizetype index = start;
		     index < result.diagnostics.size(); ++index)
		{
			if (result.diagnostics.at(index).kind
			    == OicDiagnosticKind::SourceError)
			{
				return true;
			}
		}
		return false;
	}

	void addUnsupported(
		OicSourceDefinition& source,
		const QString& capability,
		const QString& path,
		const QString& message)
	{
		if (!source.unsupported_capabilities.contains(capability))
			source.unsupported_capabilities.push_back(capability);
		result.diagnostics.push_back({
			OicDiagnosticKind::UnsupportedSource,
			QStringLiteral("unsupported-source"),
			path,
			message + QStringLiteral(": ") + capability,
			current_source,
		});
	}

	void addError(
		bool catalog_level,
		const QString& code,
		const QString& path,
		const QString& message)
	{
		if (catalog_level)
			catalogError(code, path, message);
		else
			sourceError(code, path, message);
	}

	void catalogError(
		const QString& code,
		const QString& path,
		const QString& message)
	{
		result.diagnostics.push_back({
			OicDiagnosticKind::CatalogError,
			code,
			path,
			message,
			current_source,
		});
	}

	void sourceError(
		const QString& code,
		const QString& path,
		const QString& message)
	{
		result.diagnostics.push_back({
			OicDiagnosticKind::SourceError,
			code,
			path,
			message,
			current_source,
		});
	}

	static const QSet<QString>& runtimeCapabilities()
	{
		static const QSet<QString> capabilities {
			QStringLiteral("tile-matrix-set.ogc-2.0"),
			QStringLiteral("tile-matrix-set.dyadic.v1"),
			QStringLiteral("registration.translation2d.v1"),
		};
		return capabilities;
	}

	OicCatalogReadResult& result;
	int current_source = -1;
};

}  // namespace

QString OicDiagnostic::displayText() const
{
	if (path.isEmpty())
		return message;
	return QStringLiteral("%1: %2").arg(path, message);
}

const OicResource* OicCatalog::resource(const QString& id) const noexcept
{
	for (auto const& candidate : resources)
	{
		if (candidate.id == id)
			return &candidate;
	}
	return nullptr;
}

bool OicCatalogReadResult::accepted() const noexcept
{
	return !hasCatalogErrors() && validSourceCount() > 0;
}

bool OicCatalogReadResult::hasCatalogErrors() const noexcept
{
	for (auto const& diagnostic : diagnostics)
	{
		if (diagnostic.kind == OicDiagnosticKind::CatalogError)
			return true;
	}
	return false;
}

int OicCatalogReadResult::validSourceCount() const noexcept
{
	return int(std::count_if(
		catalog.sources.begin(), catalog.sources.end(),
		[](auto const& source) { return source.valid; }
	));
}

int OicCatalogReadResult::supportedSourceCount() const noexcept
{
	return int(std::count_if(
		catalog.sources.begin(), catalog.sources.end(),
		[](auto const& source) { return source.supported; }
	));
}

QVector<ResolvedImagerySource> OicCatalogReadResult::resolvedSources() const
{
	QVector<ResolvedImagerySource> sources;
	if (hasCatalogErrors())
		return sources;
	sources.reserve(supportedSourceCount());
	for (auto const& definition : catalog.sources)
	{
		if (definition.resolved_source)
			sources.push_back(*definition.resolved_source);
	}
	return sources;
}

QString OicCatalogReader::fileExtension()
{
	return QStringLiteral("oic");
}

OicCatalogReadResult OicCatalogReader::read(const QByteArray& bytes)
{
	OicCatalogReadResult result;
	result.catalog.document_sha256 = sha256(bytes);
	if (bytes.size() > maximum_document_size)
	{
		result.diagnostics.push_back({
			OicDiagnosticKind::CatalogError,
			QStringLiteral("document-size"),
			QStringLiteral("$"),
			QStringLiteral("Catalog exceeds the %1-byte limit")
				.arg(maximum_document_size),
			-1,
		});
		return result;
	}

	JsonPreflight preflight(bytes);
	if (!preflight.validate())
	{
		result.diagnostics.push_back({
			OicDiagnosticKind::CatalogError,
			QStringLiteral("json-preflight"),
			QStringLiteral("$"),
			preflight.errorString(),
			-1,
		});
		return result;
	}

	QJsonParseError parse_error;
	auto const document = QJsonDocument::fromJson(bytes, &parse_error);
	if (parse_error.error != QJsonParseError::NoError
	    || !document.isObject())
	{
		result.diagnostics.push_back({
			OicDiagnosticKind::CatalogError,
			QStringLiteral("json-parse"),
			QStringLiteral("$"),
			parse_error.error == QJsonParseError::NoError
			  ? QStringLiteral("Catalog root must be an object")
			  : parse_error.errorString(),
			-1,
		});
		return result;
	}

	CatalogValidator validator(result);
	validator.validate(document.object(), bytes);
	if (result.hasCatalogErrors())
	{
		for (auto& source : result.catalog.sources)
			source.resolved_source.reset();
	}
	return result;
}

}  // namespace OpenOrienteering::imagery
