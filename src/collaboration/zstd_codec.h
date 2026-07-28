/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_ZSTD_CODEC_H
#define OPENORIENTEERING_ZSTD_CODEC_H

#include <QByteArray>
#include <QString>

namespace OpenOrienteering::ZstdCodec {

QByteArray compress(const QByteArray &input, int level = 3,
                    QString *error = nullptr);
QByteArray decompress(const QByteArray &frame, qsizetype maximum_output_bytes,
                      QString *error = nullptr);

} // namespace OpenOrienteering::ZstdCodec

#endif
