# DopusMdViewer

Directory Opus 的 Markdown 预览插件，页面由 WebView2 渲染。

## 安装
把 `bin\DopusMdViewer.dll` 与 `bin\DopusMdViewer\` 一起复制到 DOpus 的
Viewers 目录，重启 DOpus 后在「设置 → 查看器插件」中启用。

## 打印与导出 PDF

右键菜单两个入口，配色都取「设置 → 外观 → 打印主题」（与显示用的亮/暗皮肤是同一份清单）：

- **打印…**（Ctrl+P）：页面先把全文铺进打印容器，再打开 Chromium 打印预览，纸张与打印机由预览决定。
- **导出 PDF…**（Ctrl+Shift+P）：插件自己生成 PDF，A4、四边 10 mm、保留背景色、不带页眉页脚。

要「能选中、能复制」的 PDF 用导出：走 Adobe PDF 一类虚拟打印机时，打印栈会把文本转成
轮廓曲线，文件里就没有文本层了——这与插件无关，是那条打印路径的固有行为。
选深色打印主题时，导出的每一页都是深色底，不再另设开关。

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


