# DopusMdViewer

Directory Opus 的 Markdown 预览插件，页面由 WebView2 渲染。

## 安装
把 `bin\DopusMdViewer.dll` 与 `bin\DopusMdViewer\` 一起复制到 DOpus 的
Viewers 目录，重启 DOpus 后在「设置 → 查看器插件」中启用。

## 支持或赞助
https://docs.qq.com/aio/DRWtMY3FQS0ZHRGRG?p=qIniwAv5QQjaAipgIvwLF5

## **构建**
`build.bat` 编译并链接出 `bin\DopusMdViewer.dll`；`node webbuild/build.mjs`
重新打包前端资源到 `web\`，再同步到 `bin\DopusMdViewer\`；
`python tools/build_unittests.py run` 跑 C++ 单元测试。
需要 MSVC v14.44、Windows SDK 10.0.26100.0、Node.js、Python 3。

## 协议
MIT，见 [LICENSE](LICENSE)。Copyright (c) 2026 dsqm / DopusMdViewer。
第三方依赖与许可见 `web\LICENSES\INDEX.txt`。


