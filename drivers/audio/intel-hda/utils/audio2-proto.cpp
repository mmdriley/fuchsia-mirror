// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "drivers/audio/intel-hda/utils/audio2-proto.h"

namespace audio {
namespace audio2_proto {

#define WITH_FLAGS(_str) \
    ((sample_format & AUDIO2_SAMPLE_FORMAT_FLAG_UNSIGNED)        \
    ? ((sample_format & AUDIO2_SAMPLE_FORMAT_FLAG_INVERT_ENDIAN) \
        ? _str "_UNSIGNED [InvEndian]" : _str "_UNSIGNED")       \
    : ((sample_format & AUDIO2_SAMPLE_FORMAT_FLAG_INVERT_ENDIAN) \
        ? _str " [InvEndian]" : _str))

const char* SampleFormatToString(SampleFormat sample_format) {
    auto fmt = static_cast<SampleFormat>(sample_format & ~AUDIO2_SAMPLE_FORMAT_FLAG_MASK);
    switch (fmt) {
    case AUDIO2_SAMPLE_FORMAT_BITSTREAM:    return WITH_FLAGS("BITSTREAM");
    case AUDIO2_SAMPLE_FORMAT_8BIT:         return WITH_FLAGS("8BIT");
    case AUDIO2_SAMPLE_FORMAT_16BIT:        return WITH_FLAGS("16BIT");
    case AUDIO2_SAMPLE_FORMAT_20BIT_PACKED: return WITH_FLAGS("20BIT_PACKED");
    case AUDIO2_SAMPLE_FORMAT_24BIT_PACKED: return WITH_FLAGS("24BIT_PACKED");
    case AUDIO2_SAMPLE_FORMAT_20BIT_IN32:   return WITH_FLAGS("20BIT_IN32");
    case AUDIO2_SAMPLE_FORMAT_24BIT_IN32:   return WITH_FLAGS("24BIT_IN32");
    case AUDIO2_SAMPLE_FORMAT_32BIT_FLOAT:  return WITH_FLAGS("32BIT_FLOAT");
    default:                                return WITH_FLAGS("<unknown>");
    }
}
#undef WITH_FLAGS

}  // namespace audio2_proto
}  // namespace audio
