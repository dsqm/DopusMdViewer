// encoding_detect.cpp - 判码链（票 19）。
//
// 判定顺序：4 字节 BOM → 无 BOM UTF-16 零字节启发式 → 跳过前导 ASCII 取 64 KB 样本 →
// 纯 ASCII / 全段合法 UTF-8 → CED → GB18030 兜底。
// 不拿 CED 的 is_reliable 当门禁：实测 windows-1251 / EUC-JP / KOI8-R / ISO-2022-JP
// 都判对却报 unreliable；它只适合在「没判出来」时兜底。

#include "encoding_detect.h"

#include <cstddef>
#include <cstdint>

#include "ced_wrap.h"

namespace doc {
namespace {

constexpr size_t kSample = 64u << 10;  // 喂 CED 的样本上限
constexpr size_t kBack = 1u << 10;     // 第一个非 ASCII 之前回退的字节数
constexpr size_t kZeroScan = 4096;     // 无 BOM UTF-16 的零字节采样范围

// 尾部被切断（样本边界落在字符中间）按合法算：只有样本内部出错才判否。
bool LooksLikeUtf8(const unsigned char* p, size_t n) {
  size_t i = 0;
  while (i < n) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    uint32_t need = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
      need = 1;
      cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      need = 2;
      cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      need = 3;
      cp = c & 0x07u;
    } else {
      return false;
    }
    if (i + need >= n) return true;
    for (uint32_t k = 1; k <= need; ++k) {
      const unsigned char cc = p[i + k];
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (need == 1 && cp < 0x80) return false;
    if (need == 2 && cp < 0x800) return false;
    if (need == 3 && cp < 0x10000) return false;
    if (cp > 0x10FFFF) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    i += need + 1;
  }
  return true;
}

// ASCII 为主的文本在固定奇偶位置有大量 0x00。纯 CJK 的 UTF-16 一个 0x00 都没有，
// 这个启发式判不出来（票 19 的已知限制，靠手动覆盖或后续版本解决）。
bool LooksLikeUtf16(const unsigned char* p, size_t n, bool* bigEndian) {
  if (n < 64) return false;
  const size_t lim = n < kZeroScan ? n : kZeroScan;
  size_t nulEven = 0;
  size_t nulOdd = 0;
  size_t even = 0;
  size_t odd = 0;
  for (size_t i = 0; i < lim; ++i) {
    if ((i & 1) != 0) {
      ++odd;
      if (p[i] == 0) ++nulOdd;
    } else {
      ++even;
      if (p[i] == 0) ++nulEven;
    }
  }
  const bool le = nulOdd * 100 > odd * 40 && nulEven * 100 < even * 5;
  const bool be = nulEven * 100 > even * 40 && nulOdd * 100 < odd * 5;
  if (le == be) return false;
  *bigEndian = be;
  return true;
}

EncodingInfo FromCodePage(uint32_t cp, uint32_t bom) {
  EncodingInfo info;
  info.bomBytes = static_cast<uint16_t>(bom);
  if (cp == 65001) {
    info.encoding = Encoding::Utf8;
  } else if (cp == 1200) {
    info.encoding = Encoding::Utf16LE;
  } else if (cp == 1201) {
    info.encoding = Encoding::Utf16BE;
  } else {
    info.encoding = Encoding::Legacy;
    info.codepage = static_cast<uint16_t>(cp);
  }
  return info;
}

}  // namespace

EncodingInfo DetectEncodingEx(const void* data, size_t bytes, DetectDebug* debug) {
  if (debug) *debug = DetectDebug();
  EncodingInfo info;
  const unsigned char* p = static_cast<const unsigned char*>(data);
  const size_t n = bytes;
  if (debug) debug->firstHigh = n;
  if (!p || n == 0) return info;

  // ① 4 字节 BOM 先查：FF FE 00 00 是 UTF-32LE，不能被下面的 UTF-16LE 分支吞掉。
  if (n >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0x00 && p[3] == 0x00) {
    if (debug) {
      debug->pick = DetectPick::Bom;
      debug->codepage = 12000;
      debug->bom = 4;
    }
    return FromCodePage(12000, 4);
  }
  if (n >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFE && p[3] == 0xFF) {
    if (debug) {
      debug->pick = DetectPick::Bom;
      debug->codepage = 12001;
      debug->bom = 4;
    }
    return FromCodePage(12001, 4);
  }
  if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
    if (debug) {
      debug->pick = DetectPick::Bom;
      debug->codepage = 65001;
      debug->bom = 3;
    }
    return FromCodePage(65001, 3);
  }
  if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
    if (debug) {
      debug->pick = DetectPick::Bom;
      debug->codepage = 1200;
      debug->bom = 2;
    }
    return FromCodePage(1200, 2);
  }
  if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
    if (debug) {
      debug->pick = DetectPick::Bom;
      debug->codepage = 1201;
      debug->bom = 2;
    }
    return FromCodePage(1201, 2);
  }

  // ② 无 BOM UTF-16。
  bool bigEndian = false;
  if (LooksLikeUtf16(p, n, &bigEndian)) {
    const uint32_t cp = bigEndian ? 1201 : 1200;
    if (debug) {
      debug->pick = DetectPick::Utf16NoBom;
      debug->codepage = cp;
    }
    return FromCodePage(cp, 0);
  }

  // ③ 取样本：跳过前导 ASCII，落在真有非 ASCII 的区段里。
  size_t firstHigh = n;
  bool hasEsc = false;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] >= 0x80 && firstHigh == n) firstHigh = i;
    if (p[i] == 0x1B) hasEsc = true;
  }
  if (debug) {
    debug->firstHigh = firstHigh;
    debug->hasEsc = hasEsc;
  }

  if (firstHigh == n && !hasEsc) {
    if (debug) {
      debug->pick = DetectPick::Ascii;
      debug->codepage = 65001;
    }
    return FromCodePage(65001, 0);
  }

  const size_t sampleOff = firstHigh == n ? 0 : (firstHigh > kBack ? firstHigh - kBack : 0);
  const size_t sampleLen = n - sampleOff < kSample ? n - sampleOff : kSample;
  const unsigned char* s = p + sampleOff;
  if (debug) {
    debug->sampleOff = sampleOff;
    debug->sampleLen = sampleLen;
  }

  // ④ 样本是合法 UTF-8 且没有 ESC。含 ESC 不能短路：ISO-2022-JP 是纯 7 位字节流。
  if (!hasEsc && LooksLikeUtf8(s, sampleLen)) {
    if (debug) {
      debug->pick = DetectPick::Utf8Valid;
      debug->codepage = 65001;
    }
    return FromCodePage(65001, 0);
  }

  // ⑤ CED。ignore_7bit_mail_encodings=false，否则 ISO-2022 永远返回不了。
  const CedResult ced = CedDetect(reinterpret_cast<const char*>(s), static_cast<int>(sampleLen), false);
  if (debug) {
    debug->cedId = ced.id;
    debug->cedMime = ced.mime;
    debug->cedReliable = ced.reliable;
    debug->cedConsumed = ced.consumed;
  }
  const bool bogus = ced.unknown || (ced.ascii && firstHigh != n);
  if (!bogus && ced.codepage != 0) {
    if (debug) {
      debug->pick = DetectPick::Ced;
      debug->codepage = ced.codepage;
    }
    return FromCodePage(ced.codepage, 0);
  }

  if (debug) {
    debug->pick = DetectPick::CedFallback;
    debug->codepage = 54936;
  }
  return FromCodePage(54936, 0);
}

EncodingInfo DetectEncoding(const void* data, size_t bytes) {
  return DetectEncodingEx(data, bytes, nullptr);
}

bool EncodingFromCodePage(uint32_t codepage, EncodingInfo* out) {
  if (!out || codepage == 0) return false;
  *out = EncodingInfo();
  if (codepage == 65001) {
    out->encoding = Encoding::Utf8;
  } else if (codepage == 1200) {
    out->encoding = Encoding::Utf16LE;
  } else if (codepage == 1201) {
    out->encoding = Encoding::Utf16BE;
  } else {
    out->encoding = Encoding::Legacy;
    out->codepage = static_cast<uint16_t>(codepage);
  }
  return true;
}

}  // namespace doc
