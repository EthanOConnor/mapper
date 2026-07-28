/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "zstd_codec.h"

#include <zstd.h>

#include <limits>
#include <memory>

namespace OpenOrienteering::ZstdCodec {

namespace {

void setError(QString *error, const QString &message) {
  if (error)
    *error = message;
}

} // namespace

QByteArray compress(const QByteArray &input, int level, QString *error) {
  if (input.isEmpty())
    return {};
  const auto bound = ZSTD_compressBound(std::size_t(input.size()));
  if (bound > std::size_t(std::numeric_limits<qsizetype>::max())) {
    setError(error, QStringLiteral("The input is too large for Zstandard."));
    return {};
  }
  QByteArray output(qsizetype(bound), Qt::Uninitialized);
  const auto size = ZSTD_compress(output.data(), bound, input.constData(),
                                  std::size_t(input.size()), level);
  if (ZSTD_isError(size)) {
    setError(error, QStringLiteral("Zstandard compression failed: %1")
                        .arg(QString::fromLatin1(ZSTD_getErrorName(size))));
    return {};
  }
  output.resize(qsizetype(size));
  return output;
}

QByteArray decompress(const QByteArray &frame, qsizetype maximum_output_bytes,
                      QString *error) {
  if (frame.isEmpty() || maximum_output_bytes < 0) {
    setError(error, QStringLiteral("The Zstandard frame is empty or invalid."));
    return {};
  }
  const auto frame_size = ZSTD_findFrameCompressedSize(
      frame.constData(), std::size_t(frame.size()));
  if (ZSTD_isError(frame_size) || frame_size != std::size_t(frame.size())) {
    setError(error, QStringLiteral(
                        "The response is not exactly one Zstandard frame."));
    return {};
  }
  const auto content_size =
      ZSTD_getFrameContentSize(frame.constData(), std::size_t(frame.size()));
  if (content_size != ZSTD_CONTENTSIZE_UNKNOWN &&
      content_size != ZSTD_CONTENTSIZE_ERROR) {
    if (content_size > std::size_t(maximum_output_bytes)) {
      setError(error,
               QStringLiteral("The decompressed response exceeds its limit."));
      return {};
    }
    QByteArray output(qsizetype(content_size), Qt::Uninitialized);
    const auto size =
        ZSTD_decompress(output.data(), std::size_t(output.size()),
                        frame.constData(), std::size_t(frame.size()));
    if (ZSTD_isError(size) || size != content_size) {
      setError(error, QStringLiteral("Zstandard decompression failed."));
      return {};
    }
    return output;
  }

  struct Deleter {
    void operator()(ZSTD_DStream *stream) const { ZSTD_freeDStream(stream); }
  };
  std::unique_ptr<ZSTD_DStream, Deleter> stream(ZSTD_createDStream());
  if (!stream || ZSTD_isError(ZSTD_initDStream(stream.get()))) {
    setError(error, QStringLiteral("Cannot initialize Zstandard."));
    return {};
  }
  QByteArray output;
  QByteArray chunk(qsizetype(ZSTD_DStreamOutSize()), Qt::Uninitialized);
  ZSTD_inBuffer input{frame.constData(), std::size_t(frame.size()), 0};
  std::size_t remaining = 1;
  while (input.pos < input.size || remaining != 0) {
    ZSTD_outBuffer out{chunk.data(), std::size_t(chunk.size()), 0};
    remaining = ZSTD_decompressStream(stream.get(), &out, &input);
    if (ZSTD_isError(remaining)) {
      setError(error, QStringLiteral("Zstandard decompression failed."));
      return {};
    }
    if (out.pos > std::size_t(maximum_output_bytes - output.size())) {
      setError(error,
               QStringLiteral("The decompressed response exceeds its limit."));
      return {};
    }
    output.append(chunk.constData(), qsizetype(out.pos));
    if (out.pos == 0 && input.pos == input.size && remaining != 0) {
      setError(error, QStringLiteral("The Zstandard frame is incomplete."));
      return {};
    }
  }
  return output;
}

} // namespace OpenOrienteering::ZstdCodec
