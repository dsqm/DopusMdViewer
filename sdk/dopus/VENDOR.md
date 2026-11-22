# DOpus 插件 SDK 头文件（vendored）

`viewer plugins.h` 与 `plugin support.h` 原样取自 GPSoftware 的 Directory Opus 插件 SDK，
未做任何修改。`build.bat` 默认从这里取（`DOPUS_SDK_DIR` 可覆盖）。

## 为什么是这一版

官方下载页（`https://cdn.gpsoft.com.au/files/Misc/opus_sdk.zip`）目前仍是 2007 年的
Opus 9 SDK：`VIEWERPLUGININFOW_V4_SIZE` 为 144，而 Opus 12/13 的宿主按 160 字节的
结构传参。用旧头文件编译能过，但 `hasV4` 判断会错，插件随后写 `hIconSmall` /
`hIconLarge` 就越界。这里收录的是 2021-01-20 那版（V4_SIZE = 160），与本项目实际
编译使用的一致。

## 许可

SDK 文档第 1 页：

> The contents of this document and associated source code (header) files are
> Copyright © 2007 GP Software … and are not to be transmitted, modified, altered
> or distributed by any means without permission from GP Software. All Rights Reserved.
>
> … if these are to be distributed by third-parties, they must appear intact or,
> in the event they have been modified, such modifications must be clearly marked
> as such and the original copyright notice in these files must be preserved.

即：头文件可以随第三方项目分发，但必须原样、保留版权声明。本目录两份文件逐字未改。

## 校验

| 文件 | 字节 | SHA256 |
| --- | --- | --- |
| `viewer plugins.h` | 43822 | `1388936B2DFDF323301E26E29CB68B08F01E8E55F1E2C42840C472DD232288C2` |
| `plugin support.h` | 72824 | `B4C3A47B591F793D97B571F10E4E28DCD5713B262EF0999418BB2625216A1976` |

## 更新

拿到新版 SDK 后，把两份头文件原样覆盖到本目录（不要改内容），更新上表的大小与
SHA256，并确认 `VIEWERPLUGININFOW_V4_SIZE` 仍是 160（或按新值检查 `plugin.cpp` 的
`hasV4` 逻辑）。
