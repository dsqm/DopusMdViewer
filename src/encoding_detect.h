// encoding_detect.h - 判码链的详细结果（票 19）。
//
// 生产路径只用 DetectEncoding()；这里的 Ex 版本把中间证据（样本位置、CED 原始
// 结果）一并交出来，给 tools/encdetect 量准确率用，避免工具与 DLL 各写一套判码。

#pragma once

#include <cstddef>
#include <cstdint>

#include "doc_index.h"

namespace doc {

enum class DetectPick : uint8_t {
  Bom,
  Utf16NoBom,
  Ascii,
  Utf8Valid,
  Ced,
  CedFallback,
};

struct DetectDebug {
  DetectPick pick = DetectPick::Ascii;
  uint32_t codepage = 65001;
  size_t bom = 0;
  size_t firstHigh = 0;  // 第一个 >=0x80 的字节；没有时等于探测长度
  size_t sampleOff = 0;
  size_t sampleLen = 0;
  bool hasEsc = false;
  int cedId = 0;
  const char* cedMime = "";
  bool cedReliable = false;
  int cedConsumed = 0;
};

EncodingInfo DetectEncodingEx(const void* data, size_t bytes, DetectDebug* debug);

}  // namespace doc
