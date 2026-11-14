# DopusMdViewer

Directory Opus 的 Markdown 预览插件，页面由 WebView2 渲染。

## 安装
把 `bin\DopusMdViewer.dll` 与 `bin\DopusMdViewer\` 一起复制到 DOpus 的
Viewers 目录，重启 DOpus 后在「设置 → 查看器插件」中启用。

## 打印与导出 PDF

右键菜单两个入口，配色都取「设置 → 外观 → 打印主题」（与显示用的亮/暗皮肤是同一份清单）：

- **打印…**（Ctrl+P）：页面先把全文铺进打印容器，再打开 Windows 系统打印对话框，纸张、边距、打印机与是否打背景都由对话框决定；不走 Chromium 的逐页预览。深色打印主题的底色、代码块底色需要在对话框里勾选「打印背景图形」才会印出来（导出 PDF 由插件强制打背景，不受此限）。超过 2 MB 的文档暂不支持打印，请改用「导出 PDF」。
- **导出 PDF…**（Ctrl+Shift+P）：插件自己生成 PDF，A4、四边 10 mm、保留背景色、不带页眉页脚。

两条路都要把全文铺成一份完整的 DOM，再交给引擎排版。整篇一次性交给引擎时，5 MB 档的
分页会长时间不返回，所以大文档先分段生成、最后由插件合并成**一份** PDF
（`name.part001.pdf` 这类中间文件在合并成功后自动删除）。分段按采样得到的 DOM 节点密度
切分，单份源文本不超过 512 KB：引擎分页耗时随单段 DOM 规模超线性增长，切到成本拐点
比按字节做大段更快。段数上限 512，40 MB 级文档可一次导出。
耗时随版面复杂度变化：普通正文 3 MB 约 30 秒、2100 余页，表格与公式密集的版面明显更慢；
状态栏会按进度给出整篇百分比与剩余时间。生成期间不要切换文件——切换会打断导出。

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


