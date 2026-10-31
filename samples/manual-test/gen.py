# -*- coding: utf-8 -*-
"""生成 DopusMdViewer 手动验收样本。"""
import os, base64, io
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = HERE
SAMPLES = os.path.dirname(HERE)
os.makedirs(ROOT, exist_ok=True)
os.makedirs(os.path.join(ROOT, "sub"), exist_ok=True)

def png(path, w, h, bg, text, fg="white"):
    im = Image.new("RGB", (w, h), bg)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, w - 1, h - 1], outline="white", width=3)
    d.text((16, h // 2 - 8), text, fill=fg)
    im.save(path, format="PNG")

png(os.path.join(ROOT, "pic.png"), 480, 160, "#2f6fdd", "pic.png 480x160 local")
png(os.path.join(ROOT, "sub", "pic2.png"), 320, 120, "#2e9e63", "sub/pic2.png 320x120")
png(os.path.join(SAMPLES, "up.png"), 320, 100, "#d08a2e", "samples/up.png")

# data URI：64x24
buf = io.BytesIO()
png(buf, 64, 24, "#8a4fd1", "data")
data_uri = "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()

def write(name, text, encoding="utf-8", bom=False):
    path = os.path.join(ROOT, name)
    data = text.encode(encoding)
    if bom and encoding == "utf-8":
        data = b"\xef\xbb\xbf" + data
    open(path, "wb").write(data)
    print(name, len(data), "B")

write("01-syntax.md", """---
title: 语法总览
tags: [test, gfm]
---

# 语法总览

> 预期：标题带下边框；列表/任务框/表格/代码高亮/定义列表/内联 HTML 全部正常；文首 front matter 不显示。

段落含 **加粗**、*斜体*、~~删除线~~、`行内代码`、==高亮==、H~2~O、x^2^。

## 引用

> 引用第一段
> 引用第二段，含 **加粗**。

## 列表

- 无序项 A
- 无序项 B
  - 嵌套项

3. third
4. fourth
5. fifth

- [ ] 未完成任务
- [x] 已完成任务

## 表格

| 列1 | 列2 | 列3 |
|---|:---:|---:|
| a | b | c |
| 中文 | 对齐 | 123 |

## 代码

```js
function add(a, b) {
  return a + b  // 应带语法高亮
}
```

    缩进代码块第一行
    缩进代码块第二行

## 定义列表

术语
: 定义列表的解释。

## 内联 HTML

<details>
<summary>点开看折叠内容</summary>

折叠里的正文。

</details>

<div>这是一个 div 块（内联原始 HTML）。</div>

---

结束。
""")

write("02-refs-footnotes.md", """# 引用与脚注

> 预期：三处引用都应是链接；脚注 [1][1][2] 上标；点角标跳到底部脚注区；
> 未定义的引用与脚注保持原文；定义块本身不显示。

跨块引用：[官方文档][docs]、[docs][]、[docs] 三种写法。

脚注：第一次[^note]，第二次[^note]（复用同一编号），另一个[^other]。

未定义的引用 [nope] 应保持原文。

未定义的脚注[^missing] 应保持原文。

[^note]: 这条脚注带 `行内代码`。

[^other]: 第二条脚注，
    续行缩进四个空格。
    第三行。

[docs]: https://example.com/path "示例标题"
""")

write("03-images.md", """# 图片

> 预期：
> - 同目录 / 子目录图片显示；块高会随图片加载后回填。
> - 上级目录与绝对路径不显示（只有 alt 文字）。
> - data URI 显示。
> - 网络图默认弹原生确认框；选“是”后加载（设置里可切三档）。

## 同目录

![同目录图片](pic.png)

## 子目录

![子目录图片](sub/pic2.png)

## 上级目录（预期不显示）

![上级目录图片](../up.png)

## 绝对路径（预期不显示）

![绝对路径图片](C:/Users/momo/Documents/ai/Directory Opus plugins/DopusMdViewer/samples/manual-test/pic.png)

## data URI（预期显示）

![data URI](%s)

## 网络图（默认弹确认框）

![网络图片](https://httpbin.org/image/png)
""" % data_uri)

write("04-math-mermaid.md", """# 公式与图表

> 预期：行内/独立公式排版正确；两张图按皮肤配色渲染；
> 未知图类型的那个块保留源码（日志里有 mermaid-error，属预期）。

行内公式 $E = mc^2$，行内希腊字母 $\\alpha + \\beta$。

$$
\\int_0^1 x^2 dx = \\frac{1}{3}
$$

$$\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}$$

```mermaid
flowchart LR
  A[开始] --> B{判断}
  B -->|是| C[执行]
  B -->|否| D[结束]
```

```mermaid
sequenceDiagram
  participant U as 用户
  participant V as 查看器
  U->>V: 打开文件
  V-->>U: 渲染完成
```

```mermaid
unknownDiagram
  x --> y
```

| 符号 | 公式 |
|---|---|
| 欧拉 | $e^{i\\pi}+1=0$ |
| 积分 | $\\sum_{i=1}^{n} i$ |
""")

# 05 混合：普通块 + 3000 行围栏 + 普通块
lines = ["# 普通块 + 巨型块混合", "",
         "> 预期：进出巨型围栏时滚动连续；围栏内行号连续、无空白带；离开围栏后正文恢复排版。", ""]
lines += ["前置段落 %d：普通渲染。" % i for i in range(1, 4)]
lines += ["", "```text"]
for i in range(3000):
    lines.append("L%05d  the quick brown fox jumps over the lazy dog" % i)
lines += ["```", ""]
lines += ["后置段落 %d：普通渲染。" % i for i in range(1, 4)]
lines += ["", "## 后置标题", "", "结束。"]

write("big-mixed-3000.md", "\n".join(lines) + "\n")

# 120k 行单围栏：跨段接缝
lines = ["# 12 万行单围栏", "",
         "> 预期：整篇一个代码块；第 10 万行附近是分段接缝，边框应连续（约有一行高的间距差）。",
         "> 跳到末尾再滚回接缝，行号应连续。", "", "```text"]
for i in range(120000):
    lines.append("L%06d  the quick brown fox jumps over the lazy dog 0123456789" % i)
lines += ["```"]

write("big-fence-120000.md", "\n".join(lines) + "\n")

# 3000 行表格：已知取舍，纯文本显示
lines = ["# 3000 行巨型表格", "",
         "> 预期：按已知取舍，表格整块超过 2000 行会走巨型块纯文本层——显示为代码块里的原始 Markdown 文本，不做表格排版。",
         "", "| # | name | value |", "|---|---|---|"]
for i in range(3000):
    lines.append("| %d | row%d | %d |" % (i, i, i * 7 % 1000))
write("big-table-3000.md", "\n".join(lines) + "\n")

# 三种编码：同一内容
enc_text = """# 编码判定

> 预期：三份内容显示完全一致；中文不乱码。

中文正文：解析、区块、索引、流式、视口、渲染。

- 列表项一
- 列表项二

| 列 | 值 |
|---|---|
| 编码 | GB18030 / UTF-16LE / UTF-8 BOM |

```text
代码块里的中文
```
"""
write("05-encoding-gb18030.md", enc_text, encoding="gb18030")
write("06-encoding-utf16le.md", enc_text, encoding="utf-16-le", bom=False)
# UTF-16LE 需要 BOM
path = os.path.join(ROOT, "06-encoding-utf16le.md")
data = open(path, "rb").read()
open(path, "wb").write(b"\xff\xfe" + data)
print("06-encoding-utf16le.md BOM added")
write("07-encoding-utf8-bom.md", enc_text, bom=True)

write("README.md", """# 手动验收样本（DopusMdViewer）

在 Opus 里按顺序打开，每个文件开头都写了预期。

| 文件 | 看什么 |
|---|---|
| 01-syntax.md | GFM 排版、代码高亮、任务列表、定义列表、内联 HTML、front matter 隐藏 |
| 02-refs-footnotes.md | 跨块引用 `[x][ref]`、脚注角标与文末脚注区、未定义项保持原文 |
| 03-images.md | 本地图（同目录/子目录）、`../` 与绝对路径被拒、data URI、网络图确认 |
| 04-math-mermaid.md | KaTeX 行内/独立公式、Mermaid 两种图、未知图降级 |
| 05/06/07-encoding-*.md | GB18030 / UTF-16LE / UTF-8 BOM 三种编码同一内容 |
| big-mixed-3000.md | 普通块与 3000 行围栏混排，进出巨型块的滚动衔接 |
| big-fence-120000.md | 12 万行单围栏：窗口化滚动、第 10 万行处的分段接缝 |
| big-table-3000.md | 已知取舍：巨型表格按纯文本显示 |

网络图确认受 `config.json` 的 `netImageConfirm` 影响；测完之后可在设置里切回默认。
""")

print("DONE", ROOT)
