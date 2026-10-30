// skin.h - 皮肤文件定位与读取（票 07：皮肤是一份完整 CSS）。
//
// 查找顺序：用户目录同名覆盖内置。
//   <DLL 目录>\web\skins\<id>.css    内置，随插件分发
//   <数据目录>\skins\<id>.css         用户皮肤
// 文件不存在就返回 false（不报错、不创建目录）。

#pragma once

#include <string>

// id 只允许 [a-z0-9-]，其余字符丢弃；非法或空 id 返回 false。
bool SkinLoad(const std::wstring& id, std::wstring* cssOut);

// 内置皮肤目录与用户皮肤目录的绝对路径，供设置界面的「打开皮肤目录」用。
std::wstring SkinBuiltinDir(void);
std::wstring SkinUserDir(void);
