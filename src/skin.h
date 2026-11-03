// skin.h - 皮肤文件定位与读取（皮肤是一份完整 CSS）。
//
// 查找顺序：用户目录同名覆盖内置。
//   <DLL 目录>\DopusMdViewer\skins\<id>.css    内置，随插件分发
//   <数据目录>\skins\<id>.css         用户皮肤
// 文件不存在就返回 false（不报错、不创建目录）。

#pragma once

#include <string>
#include <vector>

struct SkinEntry {
  std::wstring id;
  std::wstring label;  // 显示名，项尾带 (内置) / (用户)
  bool user = false;
};

// 枚举内置与用户皮肤；同名用户皮肤覆盖内置，只出现一条。按显示名排序。
void SkinList(std::vector<SkinEntry>* out);

// id 就是文件名主干，原样使用（大小写、点号都合法）；只拒绝路径分隔符与通配符等危险字符。
// 非法或空 id 返回 false。
bool SkinLoad(const std::wstring& id, std::wstring* cssOut);

// 内置皮肤目录与用户皮肤目录的绝对路径，供设置界面的「打开皮肤目录」用。
std::wstring SkinBuiltinDir(void);
std::wstring SkinUserDir(void);
