/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "oom_json.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QJsonArray>
#include <QJsonObject>

namespace OpenOrienteering::OomJson {

namespace {

bool appendString(const QString &value, QByteArray &out, QString *error) {
  out.append('"');
  for (qsizetype i = 0; i < value.size(); ++i) {
    const auto unit = value.at(i).unicode();
    switch (unit) {
    case '"':
      out.append("\\\"");
      break;
    case '\\':
      out.append("\\\\");
      break;
    case '\b':
      out.append("\\b");
      break;
    case '\f':
      out.append("\\f");
      break;
    case '\n':
      out.append("\\n");
      break;
    case '\r':
      out.append("\\r");
      break;
    case '\t':
      out.append("\\t");
      break;
    default:
      if (unit < 0x20) {
        constexpr char hex[] = "0123456789abcdef";
        out.append("\\u00");
        out.append(hex[(unit >> 4) & 0xf]);
        out.append(hex[unit & 0xf]);
      } else if (QChar::isHighSurrogate(unit)) {
        if (i + 1 >= value.size() ||
            !QChar::isLowSurrogate(value.at(i + 1).unicode())) {
          if (error)
            *error = QStringLiteral(
                "oom-json/1 strings may not contain lone surrogates.");
          return false;
        }
        const auto codepoint =
            QChar::surrogateToUcs4(unit, value.at(++i).unicode());
        out.append(QString::fromUcs4(&codepoint, 1).toUtf8());
      } else if (QChar::isLowSurrogate(unit)) {
        if (error)
          *error = QStringLiteral(
              "oom-json/1 strings may not contain lone surrogates.");
        return false;
      } else {
        out.append(QString(QChar(unit)).toUtf8());
      }
    }
  }
  out.append('"');
  return true;
}

bool appendValue(const QJsonValue &value, QByteArray &out, QString *error) {
  switch (value.type()) {
  case QJsonValue::Null:
    out.append("null");
    return true;
  case QJsonValue::Bool:
    out.append(value.toBool() ? "true" : "false");
    return true;
  case QJsonValue::Double: {
    const auto missing = std::numeric_limits<qint64>::min();
    const auto integer = value.toInteger(missing);
    const auto number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || integer == missing ||
        static_cast<double>(integer) != number) {
      if (error)
        *error = QStringLiteral(
            "oom-json/1 numbers must be nonnegative signed 63-bit integers.");
      return false;
    }
    out.append(QByteArray::number(integer));
    return true;
  }
  case QJsonValue::String:
    return appendString(value.toString(), out, error);
  case QJsonValue::Array: {
    out.append('[');
    const auto array = value.toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
      if (i)
        out.append(',');
      if (!appendValue(array.at(i), out, error))
        return false;
    }
    out.append(']');
    return true;
  }
  case QJsonValue::Object: {
    const auto object = value.toObject();
    auto keys = object.keys();
    for (const auto &key : keys) {
      for (const auto unit : key) {
        if (unit.unicode() > 0x7f) {
          if (error)
            *error = QStringLiteral("oom-json/1 object keys must be ASCII.");
          return false;
        }
      }
    }
    std::sort(keys.begin(), keys.end());
    out.append('{');
    for (qsizetype i = 0; i < keys.size(); ++i) {
      if (i)
        out.append(',');
      const auto &key = keys.at(i);
      if (!appendString(key, out, error))
        return false;
      out.append(':');
      if (!appendValue(object.value(key), out, error))
        return false;
    }
    out.append('}');
    return true;
  }
  case QJsonValue::Undefined:
    if (error)
      *error = QStringLiteral("oom-json/1 does not allow undefined values.");
    return false;
  }
  if (error)
    *error = QStringLiteral("oom-json/1 encountered an unknown JSON type.");
  return false;
}

} // namespace

QByteArray canonical(const QJsonValue &value, QString *error) {
  QByteArray result;
  if (!appendValue(value, result, error))
    return {};
  return result;
}

} // namespace OpenOrienteering::OomJson
