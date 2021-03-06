// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <zircon/assert.h>

#include <blobfs/compression-settings.h>
#include <blobfs/format.h>
#include <src/lib/chunked-compression/compression-params.h>
#include <zstd/zstd.h>

namespace blobfs {

const char* CompressionAlgorithmToString(CompressionAlgorithm algorithm) {
  switch (algorithm) {
    case CompressionAlgorithm::LZ4:
      return "LZ4";
    case CompressionAlgorithm::ZSTD:
      return "ZSTD";
    case CompressionAlgorithm::ZSTD_SEEKABLE:
      return "ZSTD_SEEKABLE";
    case CompressionAlgorithm::CHUNKED:
      return "ZSTD_CHUNKED";
    case CompressionAlgorithm::UNCOMPRESSED:
      return "UNCOMPRESSED";
  }
}

CompressionAlgorithm AlgorithmForInode(const Inode& inode) {
  uint16_t flags = inode.header.flags & kBlobFlagMaskAnyCompression;
  if (flags & kBlobFlagLZ4Compressed) {
    return CompressionAlgorithm::LZ4;
  } else if (flags & kBlobFlagZSTDCompressed) {
    return CompressionAlgorithm::ZSTD;
  } else if (flags & kBlobFlagZSTDSeekableCompressed) {
    return CompressionAlgorithm::ZSTD_SEEKABLE;
  } else if (flags & kBlobFlagChunkCompressed) {
    return CompressionAlgorithm::CHUNKED;
  } else if (!flags) {
    return CompressionAlgorithm::UNCOMPRESSED;
  }
  static_assert(
      kBlobFlagMaskAnyCompression == (kBlobFlagLZ4Compressed | kBlobFlagZSTDCompressed |
                                      kBlobFlagChunkCompressed | kBlobFlagZSTDSeekableCompressed),
      "Missing algorithm case");
  ZX_ASSERT(false);
}

uint16_t CompressionInodeHeaderFlags(const CompressionAlgorithm& algorithm) {
  switch (algorithm) {
    case CompressionAlgorithm::UNCOMPRESSED:
      return 0;
    case CompressionAlgorithm::LZ4:
      return kBlobFlagLZ4Compressed;
    case CompressionAlgorithm::ZSTD:
      return kBlobFlagZSTDCompressed;
    case CompressionAlgorithm::ZSTD_SEEKABLE:
      return kBlobFlagZSTDSeekableCompressed;
    case CompressionAlgorithm::CHUNKED:
      return kBlobFlagChunkCompressed;
  }
}

void SetCompressionAlgorithm(Inode* inode, const CompressionAlgorithm algorithm) {
  uint16_t* flags = &(inode->header.flags);
  *flags &= ~kBlobFlagMaskAnyCompression;
  *flags |= CompressionInodeHeaderFlags(algorithm);
}

bool CompressionSettings::IsValid() const {
  if (compression_level == std::nullopt) {
    return true;
  }
  switch (compression_algorithm) {
    case CompressionAlgorithm::LZ4:
    case CompressionAlgorithm::UNCOMPRESSED:
      // compression_level shouldn't be set in these cases.
      return false;
    case CompressionAlgorithm::ZSTD:
    case CompressionAlgorithm::ZSTD_SEEKABLE:
      return *compression_level >= ZSTD_minCLevel() && *compression_level <= ZSTD_maxCLevel();
    case CompressionAlgorithm::CHUNKED:
      return *compression_level >= chunked_compression::CompressionParams::MinCompressionLevel() &&
             *compression_level <= chunked_compression::CompressionParams::MaxCompressionLevel();
  }
}

}  // namespace blobfs
