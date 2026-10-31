// ced_wrap.h - 把 compact_enc_det 隔在一个 TU 后面。
//
// 为什么要包一层：util/encodings/encodings.h 里有 #undef UNICODE（为了让名为 UNICODE
// 的枚举值编得过），任何 include 过它的编译单元之后都不能再用 UNICODE 版 Win32 宏。
// 这里只暴露 POD 与纯函数，不带任何 CED 类型。

#ifndef ENCDETECT_CED_WRAP_H_
#define ENCDETECT_CED_WRAP_H_

struct CedResult {
  int id = 0;               // CED 的 Encoding 数值，只用于日志
  const char* mime = "";    // 例如 "Shift_JIS"
  unsigned codepage = 0;    // Windows 代码页；0 表示我们不解码这一类
  bool unknown = false;     // 没判出来（UNKNOWN_ENCODING / BINARYENC）
  bool ascii = false;       // 判成 ASCII_7BIT
  bool reliable = false;    // 首位比第二位高 2^10 倍；门槛极高，只能当参考
  int consumed = 0;         // 实际参与判定的字节数
};

// ignore7bit = false 时才会返回 ISO-2022-JP / ISO-2022-KR / HZ / UTF-7。
CedResult CedDetect(const char* text, int len, bool ignore7Bit);

#endif  // ENCDETECT_CED_WRAP_H_
