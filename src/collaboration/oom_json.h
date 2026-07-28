/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_OOM_JSON_H
#define OPENORIENTEERING_OOM_JSON_H

#include <QByteArray>
#include <QJsonValue>
#include <QString>

namespace OpenOrienteering::OomJson {

/** Serializes the restricted, integer-preserving oom-json/1 profile. */
QByteArray canonical(const QJsonValue &value, QString *error = nullptr);

} // namespace OpenOrienteering::OomJson

#endif
