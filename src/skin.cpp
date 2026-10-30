#include "skin.h"

#include <windows.h>

#include "log.h"
#include "util.h"

namespace {

// 票 07：皮肤文件上限 1 MB，超过拒绝。
const size_t kMaxSkinBytes = 1024 * 1024;

std::wstring SanitizeId(const std::wstring& id) {
  std::wstring out;
  for (wchar_t ch : id) {
    if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-') out += ch;
  }
  return out;
}

}  // namespace

std::wstring SkinBuiltinDir(void) {
  const std::wstring moduleDir = UtilModuleDir();
  if (moduleDir.empty()) return std::wstring();
  return UtilJoin(UtilJoin(moduleDir, L"web"), L"skins");
}

std::wstring SkinUserDir(void) {
  const std::wstring dataDir = UtilDataDir();
  if (dataDir.empty()) return std::wstring();
  return UtilJoin(dataDir, L"skins");
}

bool SkinLoad(const std::wstring& id, std::wstring* cssOut) {
  if (!cssOut) return false;

  const std::wstring clean = SanitizeId(id);
  if (clean.empty()) {
    LogLine(L"SKIN: 非法 id \"%s\"", id.c_str());
    return false;
  }

  std::wstring path = UtilJoin(SkinUserDir(), (clean + L".css").c_str());
  bool fromUser = UtilFileExists(path);
  if (!fromUser) {
    path = UtilJoin(SkinBuiltinDir(), (clean + L".css").c_str());
    if (!UtilFileExists(path)) {
      LogLine(L"SKIN: 找不到 %s.css（用户目录与内置目录都没有）", clean.c_str());
      return false;
    }
  }

  std::string bytes;
  if (!UtilReadFileBytes(path, kMaxSkinBytes, &bytes)) {
    LogLine(L"SKIN: 读取失败或超过 1MB %s", path.c_str());
    return false;
  }

  std::wstring css;
  if (!UtilDecodeText(bytes, &css)) {
    LogLine(L"SKIN: %s 既不是 UTF-8 也不是 GB18030", path.c_str());
    return false;
  }

  LogLine(L"SKIN: 已加载 %s（%s，%zu 字节 → %zu 字符）", path.c_str(),
          fromUser ? L"用户" : L"内置", bytes.size(), css.size());

  *cssOut = std::move(css);
  return true;
}
