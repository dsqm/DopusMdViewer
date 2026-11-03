#include "skin.h"

#include <windows.h>

#include <algorithm>
#include <map>

#include "log.h"
#include "util.h"

namespace {

// 皮肤文件上限 1 MB，超过拒绝。
const size_t kMaxSkinBytes = 1024 * 1024;

// 皮肤 id 就是文件名主干，配置里存什么就用什么：Dark.css、my.skin.css 都要能加载。
// 只挡会把查找带出皮肤目录（或把 MakeSkin 的字段分隔打乱）的字符。
bool IsSafeSkinId(const std::wstring& id) {
  if (id.empty() || id.size() > 128) return false;
  for (wchar_t ch : id) {
    if (ch < 0x20) return false;
    if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
        ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
      return false;
    }
  }
  return true;
}

}  // namespace

namespace {

void CollectSkins(const std::wstring& dir, bool user, std::map<std::wstring, SkinEntry>* out) {
  if (dir.empty() || !out) return;
  WIN32_FIND_DATAW fd = {};
  HANDLE h = ::FindFirstFileW((dir + L"\\*.css").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::wstring name = fd.cFileName;
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) continue;
    const std::wstring id = name.substr(0, dot);
    SkinEntry entry;
    entry.id = id;
    entry.user = user;
    entry.label = id + (user ? L"（用户）" : L"（内置）");
    (*out)[id] = entry;  // 用户目录同名覆盖内置
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
}

}  // namespace

void SkinList(std::vector<SkinEntry>* out) {
  if (!out) return;
  out->clear();
  std::map<std::wstring, SkinEntry> found;
  CollectSkins(SkinBuiltinDir(), false, &found);
  CollectSkins(SkinUserDir(), true, &found);
  for (const auto& item : found) out->push_back(item.second);
  std::sort(out->begin(), out->end(), [](const SkinEntry& a, const SkinEntry& b) {
    return ::CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a.label.c_str(), -1,
                            b.label.c_str(), -1) == CSTR_LESS_THAN;
  });
}

std::wstring SkinBuiltinDir(void) {
  const std::wstring webDir = UtilWebDir();
  if (webDir.empty()) return std::wstring();
  return UtilJoin(webDir, L"skins");
}

std::wstring SkinUserDir(void) {
  const std::wstring dataDir = UtilDataDir();
  if (dataDir.empty()) return std::wstring();
  return UtilJoin(dataDir, L"skins");
}

bool SkinLoad(const std::wstring& id, std::wstring* cssOut) {
  if (!cssOut) return false;

  if (!IsSafeSkinId(id)) {
    LogLine(L"SKIN: 非法 id \"%s\"", id.c_str());
    return false;
  }

  std::wstring path = UtilJoin(SkinUserDir(), (id + L".css").c_str());
  bool fromUser = UtilFileExists(path);
  if (!fromUser) {
    path = UtilJoin(SkinBuiltinDir(), (id + L".css").c_str());
    if (!UtilFileExists(path)) {
      LogLine(L"SKIN: 找不到 %s.css（用户目录与内置目录都没有）", id.c_str());
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
