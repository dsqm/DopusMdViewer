// ced_wrap.cpp - 唯一 include compact_enc_det 的编译单元。

#include "ced_wrap.h"

#include "compact_enc_det/compact_enc_det.h"
#include "util/encodings/encodings.h"

namespace {

// CED 的 Encoding → Windows 代码页。0 表示这一类我们不解码（印地语系等）。
// 取值依据「代码页标识符」文档；ISO-2022 系列解码时 dwFlags 必须为 0。
unsigned CodePageOf(Encoding e) {
  switch (e) {
    case ASCII_7BIT: return 20127;
    case UTF8:
    case UTF8UTF8: return 65001;
    case UTF16LE: return 1200;
    case UTF16BE: return 1201;
    case UTF32LE: return 12000;
    case UTF32BE: return 12001;

    case JAPANESE_SHIFT_JIS:
    case JAPANESE_CP932:
    case KDDI_SHIFT_JIS:
    case DOCOMO_SHIFT_JIS:
    case SOFTBANK_SHIFT_JIS: return 932;
    case JAPANESE_EUC_JP: return 20932;
    case JAPANESE_JIS:
    case KDDI_ISO_2022_JP:
    case SOFTBANK_ISO_2022_JP: return 50220;

    // GB2312/GBK 一律用 54936：GB18030 是二者的超集，且 4 字节序列的第二字节是
    // 0x30-0x39，这在 GBK 里不是合法尾字节 —— 所以合法 GBK 文件用 54936 解码结果完全相同，
    // 反之若文件真含 4 字节字符，54936 才解得对。
    case CHINESE_GB: return 54936;
    case GBK: return 54936;
    case GB18030: return 54936;
    case CHINESE_BIG5:
    case CHINESE_BIG5_CP950: return 950;
    case BIG5_HKSCS: return 950;  // 无 HKSCS 扩展的 CP950，见票 19 的已知限制
    case CHINESE_EUC_CN:
    case CHINESE_EUC_DEC:
    case CHINESE_CNS: return 0;   // EUC-TW，Windows 无稳定代码页

    case KOREAN_EUC_KR: return 949;
    case ISO_2022_KR: return 50225;
    case ISO_2022_CN: return 50227;
    // HZ 与 UTF-7 也能显示，但检索要按 7 位状态机映射回字节；不在票 19 的范围内，
    // 按「不解码这一类」回退，避免搜索给出错位的高亮。
    case HZ_GB_2312: return 0;

    case RUSSIAN_KOI8_R: return 20866;
    case RUSSIAN_KOI8_RU: return 21866;
    case RUSSIAN_CP866: return 866;
    case RUSSIAN_CP1251: return 1251;

    case MSFT_CP1250: return 1250;
    case MSFT_CP1252: return 1252;
    case MSFT_CP1253: return 1253;
    case MSFT_CP1254: return 1254;
    case MSFT_CP1255: return 1255;
    case MSFT_CP1256: return 1256;
    case MSFT_CP1257: return 1257;
    case MSFT_CP874: return 874;

    case ISO_8859_1: return 28591;
    case ISO_8859_2: return 28592;
    case ISO_8859_3: return 28593;
    case ISO_8859_4: return 28594;
    case ISO_8859_5: return 28595;
    case ISO_8859_6: return 28596;
    case ISO_8859_7: return 28597;
    case ISO_8859_8: return 28598;
    case ISO_8859_8_I: return 38598;
    case ISO_8859_9: return 28599;
    case ISO_8859_10: return 28600;
    case ISO_8859_11: return 28601;
    case ISO_8859_13: return 28603;
    case ISO_8859_15: return 28605;

    case CZECH_CP852: return 852;
    case MACINTOSH_ROMAN: return 10000;

    case UTF7: return 0;
    case BINARYENC: return 0;
    default: return 0;
  }
}

}  // namespace

CedResult CedDetect(const char* text, int len, bool ignore7Bit) {
  CedResult r;
  if (text == nullptr || len <= 0) return r;

  int consumed = 0;
  bool reliable = false;
  const Encoding e = CompactEncDet::DetectEncoding(
      text, len,
      nullptr,   // url_hint
      nullptr,   // http_charset_hint
      nullptr,   // meta_charset_hint
      UNKNOWN_ENCODING, UNKNOWN_LANGUAGE,
      CompactEncDet::QUERY_CORPUS,
      ignore7Bit,
      &consumed, &reliable);

  r.id = static_cast<int>(e);
  r.mime = MimeEncodingName(e);
  r.codepage = CodePageOf(e);
  r.unknown = (e == UNKNOWN_ENCODING) || (e == BINARYENC);
  r.ascii = (e == ASCII_7BIT);
  r.reliable = reliable;
  r.consumed = consumed;
  return r;
}
