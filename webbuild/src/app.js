import MarkdownIt from 'markdown-it'
import deflist from 'markdown-it-deflist'
import taskLists from 'markdown-it-task-lists'
import sub from 'markdown-it-sub'
import sup from 'markdown-it-sup'
import mark from 'markdown-it-mark'
import { alert as alertPlugin } from '@mdit/plugin-alert'
import { setupMath, katexEngine, KATEX_OPTIONS } from './texcompat.js'
import DOMPurify from 'dompurify'
import hljs from 'highlight.js/lib/core'

import javascript from 'highlight.js/lib/languages/javascript'
import typescript from 'highlight.js/lib/languages/typescript'
import python from 'highlight.js/lib/languages/python'
import c from 'highlight.js/lib/languages/c'
import cpp from 'highlight.js/lib/languages/cpp'
import csharp from 'highlight.js/lib/languages/csharp'
import java from 'highlight.js/lib/languages/java'
import go from 'highlight.js/lib/languages/go'
import rust from 'highlight.js/lib/languages/rust'
import json from 'highlight.js/lib/languages/json'
import bash from 'highlight.js/lib/languages/bash'
import xml from 'highlight.js/lib/languages/xml'
import css from 'highlight.js/lib/languages/css'
import sql from 'highlight.js/lib/languages/sql'
import yaml from 'highlight.js/lib/languages/yaml'
import ini from 'highlight.js/lib/languages/ini'
import markdown from 'highlight.js/lib/languages/markdown'
import diff from 'highlight.js/lib/languages/diff'
import powershell from 'highlight.js/lib/languages/powershell'

/* ============================================================
   约定
   - 全文不进 DOM：自绘滚动条 + 估算总高 + 常驻窗口（800 块量级）。
   - 块正文走 https://mdblocks.localhost/__blocks 的二进制帧，批量取。
   - 颜色全在 #base / #skin，字体全在 #fx（#fx 永远最后）。
   - 单块高亮闸门：≤2000 行、≤1 MiB，且平均行长 ≤32 KiB，超限只出纯文本。
   ============================================================ */

const HIGHLIGHT_MAX_LINES = 2000
const HIGHLIGHT_MAX_BYTES = 1024 * 1024
// 花在 hljs 上的时间取决于 token 密度，不取决于体积：分行代码 1 MiB 约 30–200 ms，
// 而「整块就是一条巨长行」（压缩 JSON、base64）1 MiB 要 3.8 s。后者没救，按平均
// 行长挡掉；正常内容即使行长到 4 KiB 也远在这条线以内。
const HIGHLIGHT_MAX_AVG_LINE = 32 * 1024
const DOC_BASE_DEFAULT = 'https://mddoc.localhost/'
const DOC_HOST = 'https://mddoc.localhost/'
const BLOCKS_URL = 'https://mdblocks.localhost/__blocks'
const LINES_URL = 'https://mdblocks.localhost/__lines'
const DEFS_URL = 'https://mdblocks.localhost/__defs'
const SEARCH_URL = 'https://mdblocks.localhost/__search'
const OUTLINE_URL = 'https://mdblocks.localhost/__outline'

const OVERSCAN = 120          // 视口上下各留多少块
const MAX_FETCH_BLOCKS = 100  // 单次请求块数
const MAX_INFLIGHT = 3

// 钉块：选区覆盖的常驻块不参与换出，上限 3000 块 / 32MB。
const PIN_MAX_BLOCKS = 3000
const PIN_MAX_BYTES = 32 * 1024 * 1024
const SEL_EDGE = 42           // 拖选到离视口边缘多少 px 开始自动滚动

// 打印：全文要临时渲染进 DOM，文档越大越慢，超过上限直接拒绝。
const PRINT_MAX_FILE = 2 * 1024 * 1024
const PRINT_BATCH = 200

// 全选复制：复制的是渲染后的文本（不是 Markdown 源文本）。整篇提取有成本，
// 超过这个字符数就截断并提示；1M 级文档可以完整复制，再大只复制前缀。
const COPY_MAX_CHARS = 1200000
const COPY_BATCH = 200

// 巨型块（>2000 行）的块内行窗口：按 64 行 checkpoint 对齐，最少 128 行、
// 最多 512 行。窗口放在视口上下各一屏之外，滚动时不至于看到空白带。
const LINE_ALIGN = 64
const LINE_MARGIN = 64
const LINE_MIN = 128
const LINE_MAX = 512

// 巨型表格：一张表拆成多张块级 <table>，每张 BT_ROWS 行。
// 单表一次性布局 2 万行要 1.8 s，逐批往同一张表里追加要 4.7 s（每次追加都触发整表
// 重排，总代价 O(n²)）；拆成块级表 + content-visibility:auto 后首帧只要 0.14 s。
// 行内（tr / tbody）加 content-visibility 在 Chromium 里不生效，所以只能按块拆。
// 块取 250 不取 500：一块的「渲染 + 建 DOM + 布局」是 60–80 ms 的同步任务，打开
// 文档后半秒内滚一下滚轮就会撞上它，卡一帧；250 行把单次任务压到 30–40 ms。
// 偶数，斑马纹跨块才不会翻。
const BT_ROWS = 250
const BT_EST_ROW = 32      // 行高还没实测时的高度估算（px）
const BT_EST_HEAD = 40     // 表头估算高度（px）
// 表格按分块真表格渲染的数据行上限。再往上 DOM 节点数本身成为瓶颈（2 万行 × 5 列
// ≈ 10 万单元格），超过就沿用原有的行窗口纯文本路径。
const BT_MAX_LINES = 20000

const LANGUAGES = {
  javascript, typescript, python, c, cpp, csharp, java, go, rust,
  json, bash, xml, css, sql, yaml, ini, markdown, diff, powershell,
}

for (const [name, lang] of Object.entries(LANGUAGES)) hljs.registerLanguage(name, lang)

/* 跨块语义：链接引用定义与脚注定义散在别的块里，逐块渲染时拿不到。
   C++ 把隐藏块原文一次性送过来，这里先建 references，再按定义顺序给脚注编号。
   脚注角标自己实现，不用 markdown-it-footnote 的全局状态。 */
const REF = { references: null }
const FOOT = { defs: [], nums: new Map(), html: '' }

function normLabel(s) {
  return String(s).trim().replace(/\s+/g, ' ').toUpperCase()
}

function blockEnv() {
  return REF.references ? { references: REF.references } : {}
}

/* ---------------- 渲染管线 ---------------- */

const md = new MarkdownIt({
  html: true,
  linkify: false,
  breaks: false,
  typographer: false,
  highlight: null,
})

md.use(deflist)
  .use(taskLists, { enabled: false, label: false })
  .use(sub)
  .use(sup)
  .use(mark)

setupMath(md)

md.use(alertPlugin, {
  alertNames: ['note', 'tip', 'important', 'warning', 'caution'],
  deep: true,
  titleRenderer: (tokens, idx) => {
    const name = tokens[idx].markup || ''
    return '<p class="markdown-alert-title">' +
      escapeHtml(name.charAt(0).toUpperCase() + name.slice(1)) + '</p>\n'
  },
})

// [^label] → 上标角标。编号由文末脚注区的定义顺序给定；定义还没到时留原文。
md.inline.ruler.before('link', 'mdv_footnote_ref', (state, silent) => {
  const src = state.src
  const start = state.pos
  if (src.charCodeAt(start) !== 0x5b || src.charCodeAt(start + 1) !== 0x5e) return false
  const end = src.indexOf(']', start + 2)
  if (end < 0) return false
  const num = FOOT.nums.get(normLabel(src.slice(start + 2, end)))
  if (!num) return false
  if (!silent) {
    const token = state.push('html_inline', '', 0)
    token.content = '<sup class="footnote-ref"><a href="#fn' + num + '" id="fnref' + num +
      '">[' + num + ']</a></sup>'
  }
  state.pos = end + 1
  return true
})

const escapeHtml = md.utils.escapeHtml

/* front matter（开头 --- 包的 YAML 段）不参与渲染。C++ 侧扫描块时按同一条规则
   把它标成隐藏块，两边块号才不会错位。 */
const FRONT_MATTER = /^\uFEFF?---[ \t]*\r?\n[\s\S]*?\r?\n(?:---|\.\.\.)[ \t]*(?:\r?\n|$)/

function stripFrontMatter(source) {
  return source.replace(FRONT_MATTER, '')
}

function countLines(text) {
  let n = 1
  for (let i = 0; i < text.length; i++) if (text.charCodeAt(i) === 10) n++
  return n
}

// 显示用行数：尾随换行不另起一行（`a\n` 是 1 行）。围栏块的内容几乎总带尾随换行，
// 按 countLines 数会多出一格空行号。
function displayLines(text) {
  let n = 1
  for (let i = 0; i < text.length - 1; i++) if (text.charCodeAt(i) === 10) n++
  return n
}

// 行号列宽按位数给足（ch 是等宽字体里 "0" 的宽度）。用类名而不是行内自定义属性：
// 含 '<' 的块要过一遍 DOMPurify，行内 style 在那里不保证原样留下。
function lineGutterWidth(lines) {
  return 'ln-d' + Math.min(6, String(lines).length)
}

// 行号：只放空元素，数字由 CSS 计数器在 ::before 里生成。
// 伪元素内容不进 textContent，所以复制（取 <code> 文本）和查找（拿块内文本对齐源码）
// 都看不见行号——这是不能用文本拼行号的原因。
function lineGutter(lines) {
  return '<span class="mdv-ln" aria-hidden="true">' + '<span></span>'.repeat(lines) + '</span>'
}

/* ---------------- 空白符标记 ---------------- */

// 空格画点、制表符画箭头。字形只出现在伪元素与背景里，DOM 的文本节点保持原样，
// 所以复制（取 <code> 文本）和查找（拿块内文本对齐源码）都看不见标记——
// 这和行号用的是同一个道理，不能改成往文本里塞符号。
//
// 范围按常显的多少递增编号（配置值、菜单顺序、设置页单选都是这个顺序）：
//   1 = 仅选中：一个都不常显，只有选中的部分点亮。
//   2 = 边界：行首缩进、行尾空白、行内连续 2 个以上的空格；
//              词与词之间的单个空格不算，选中时再点亮（见 wsUpdateSelection）。
//   3 = 全部：所有空格与制表符。
//
// 三档是一条单调阶梯：常显的部分逐档变多，选中时一律全显。所以「仅选中」在
// 渲染期包的 span 与「全部」档完全相同（都是全包），只是都不加 mdv-on。
// 标记必须提前铺好：选区变化时再插 DOM 会把选区搅乱，只能改 class。
//
// 粒度：普通块逐字符（每个空白字符一个 span，字形绝对定位盖在字符上）；
// 巨型块逐段（连续空白一个 span，背景平铺点阵）——巨型块每次滚动都重画一遍，
// 逐字符的节点数扛不住。逐段粒度下点亮也只能整段点亮，与「全部」档一致。
const WS_SEL = 1
const WS_BOUNDARY = 2
const WS_ALL = 3

// 给一段 HTML 里的空白加标记。html 里可能混着标签（高亮的 span），
// 只处理标签之外的文本部分。
// perChar: true = 每个空白字符一个 span；false = 每段同质空白一个 span。
function markWhitespace(html, mode, perChar) {
  if (!mode || !html) return html
  const parts = html.split(/(<[^>]*>)/)
  let plain = ''
  const segs = []
  for (let i = 0; i < parts.length; i++) {
    const p = parts[i]
    if (!p || p.charAt(0) === '<') continue
    segs.push({ i, from: plain.length })
    plain += p
  }
  if (!plain) return html

  // 行首偏移：空白段是否贴着行首/行尾据此判断
  const lineBegins = [0]
  for (let i = 0; i < plain.length; i++) {
    if (plain.charCodeAt(i) === 10) lineBegins.push(i + 1)
  }
  const lineOf = (off) => {
    let lo = 0
    let hi = lineBegins.length - 1
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1
      if (lineBegins[mid] <= off) lo = mid
      else hi = mid - 1
    }
    return lo
  }

  const re = /[ \t]+/g
  for (const seg of segs) {
    const text = parts[seg.i]
    const base = seg.from
    let buf = ''
    let last = 0
    let m
    re.lastIndex = 0
    while ((m = re.exec(text))) {
      const s = base + m.index
      const e = s + m[0].length
      const li = lineOf(s)
      const lineEnd = li + 1 < lineBegins.length ? lineBegins[li + 1] - 1 : plain.length
      // 贴着行首或行尾算边界；行内连着两个以上的空格也算
      const boundary = s === lineBegins[li] || e === lineEnd || e - s >= 2
      buf += text.slice(last, m.index)
      // 「仅选中」档：全包但一律不常显，点亮只由 wsUpdateSelection 负责
      const visible = mode === WS_ALL || (mode === WS_BOUNDARY && boundary)
      buf += wrapWsRun(m[0], perChar, visible)
      last = m.index + m[0].length
    }
    buf += text.slice(last)
    parts[seg.i] = buf
  }
  return parts.join('')
}

// 一段空白按类型切开再包：空格与制表符画法不同，混在一段里没法分别处理。
function wrapWsRun(run, perChar, visible) {
  let out = ''
  let i = 0
  while (i < run.length) {
    const tab = run.charCodeAt(i) === 9
    let j = i + 1
    while (j < run.length && (run.charCodeAt(j) === 9) === tab) j++
    out += wrapWsChunk(run.slice(i, j), tab, perChar, visible)
    i = j
  }
  return out
}

function wrapWsChunk(chunk, tab, perChar, visible) {
  if (perChar) {
    const cls = (tab ? 'mdv-wt' : 'mdv-ws') + (visible ? ' mdv-on' : '')
    let out = ''
    for (let i = 0; i < chunk.length; i++) out += '<span class="' + cls + '">' + chunk[i] + '</span>'
    return out
  }
  // 逐段：空格靠背景平铺画点阵；制表符仍用伪元素（箭头只标位置，不标跨度）
  const cls = (tab ? 'mdv-wt' : 'mdv-wrs') + (visible ? ' mdv-on' : '')
  return '<span class="' + cls + '">' + chunk + '</span>'
}

/* ---------------- 选中时点亮（边界档 / 仅选中档） ---------------- */

// 选区内的空白一律按「全部」显示：把选到的空白段点亮。
// 边界档是「常显边界类 + 选区内补全」，仅选中档是「常显为零 + 选区内补全」，
// 两档共用这一份逻辑，只有常显部分不同（见 markWhitespace）。
// 「全部」档本来全亮，不必参与。
// 只改 class 不动 DOM：动 DOM（拆文本节点）会把选区搅乱。
const WS = { lit: new Set(), raf: 0 }

function wsSpansOf(code) {
  // 缓存挂在元素上：块重渲染时 <code> 是新的，缓存自然失效
  if (!code.__wsSpans) {
    code.__wsSpans = Array.prototype.slice.call(
      code.querySelectorAll('.mdv-ws, .mdv-wt, .mdv-wrs'))
  }
  return code.__wsSpans
}

function wsCodeOf(node) {
  let el = node
  if (el && el.nodeType === 3) el = el.parentNode
  while (el && el.nodeType === 1) {
    if (el.tagName === 'CODE') return el
    el = el.parentNode
  }
  return null
}

// 选区经过的代码块。最常见的是整段落在一个块里，走快路直接返回；
// 其余情形（跨块、或从正文拖进块里）扫一遍全文档的 <code> 看谁被碰到。
// 扫到的行内代码没有空白段，交给后面的取段逻辑自然跳过。
function wsCodesInRange(range) {
  const a = wsCodeOf(range.startContainer)
  const b = wsCodeOf(range.endContainer)
  if (a && a === b) return [a]
  const out = []
  for (const c of document.querySelectorAll('code')) {
    if (range.intersectsNode(c)) out.push(c)
  }
  return out
}

function wsClearLit() {
  for (const s of WS.lit) s.classList.remove('mdv-sel')
  WS.lit.clear()
}

function wsUpdateSelection() {
  if (state.render.codeWs !== WS_BOUNDARY && state.render.codeWs !== WS_SEL) {
    wsClearLit()
    return
  }
  const sel = window.getSelection()
  if (!sel || !sel.rangeCount) {
    wsClearLit()
    return
  }
  const range = sel.getRangeAt(0)
  if (range.collapsed) {
    wsClearLit()
    return
  }

  const next = new Set()
  for (const code of wsCodesInRange(range)) wsCollectLit(code, range, next)
  for (const s of WS.lit) if (!next.has(s)) s.classList.remove('mdv-sel')
  for (const s of next) s.classList.add('mdv-sel')
  WS.lit = next
}

// 把 code 里与选区相交的空白段收进 next。
//
// 选区落在一个块里时，命中的段在文档序上必是连续的一段，两头各二分一次就能圈出
// 候选窗口，再逐段精确判定。
//
// 二分只能用文本节点的 comparePoint：comparePoint(段, 0) 传元素时，Chromium 把
// 「段首那个位置」当成「段之前」，选区恰好从某段开头起选时它返回 -1——于是选区
// 第一段永远不亮，只选一个空格时整段被排除，看起来像点了没反应。
// 二分出来的窗口比实际宽一格是常态，多出来的交给 wsOverlaps 筛掉。
function wsCollectLit(code, range, next) {
  const spans = wsSpansOf(code)
  const n = spans.length
  if (!n) return

  // 窗口右界：第一个「起点晚于选区终点」的段，它和它之后的都不可能相交
  let lo = 0
  let hi = n
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    const t = spans[mid].firstChild
    if (t && range.comparePoint(t, 0) > 0) hi = mid
    else lo = mid + 1
  }
  const end = lo

  // 窗口左界：第一个「终点不早于选区起点」的段，它之前的都不可能相交
  lo = 0
  hi = end
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    const t = spans[mid].lastChild
    if (t && range.comparePoint(t, t.length) < 0) lo = mid + 1
    else hi = mid
  }

  for (let i = lo; i < end; i++) if (wsOverlaps(range, spans[i])) next.add(spans[i])
}

// 一段空白是否真的与选区有重叠（不是只贴着边）。
// comparePoint 只有「前 / 里 / 后」三态，分不清「落在里面」与「正好落在边界上」，
// 所以两端各补一次判断：端点在同一节点里时还得看偏移，偏移落在段的两端就等于没选。
function wsOverlaps(range, span) {
  const t = span.firstChild
  if (!t) return false
  const a = range.comparePoint(t, 0)
  if (a > 0) return false // 段首已在选区之后
  const b = range.comparePoint(t, t.length)
  if (b < 0) return false // 段尾还在选区之前
  if (a < 0) return range.startContainer === t && range.startOffset < t.length
  if (b > 0) return range.endContainer === t && range.endOffset > 0
  return true
}

function wsScheduleSelection() {
  if (WS.raf) return
  WS.raf = requestAnimationFrame(() => {
    WS.raf = 0
    wsUpdateSelection()
  })
}

function utf8Bytes(text) {
  // 只用于高亮闸门判定，TextEncoder 的精确计数不是必需
  return new TextEncoder().encode(text).length
}

// 图标只引用 sprite 里的一份定义；符号自带 viewBox，外层不必再写。
function iconSvg(id) {
  return '<svg class="mdv-i" aria-hidden="true" focusable="false"><use href="#' + id + '"></use></svg>'
}

function copyButtonHtml(kind, extraAttrs) {
  const label = kind === 'table' ? '复制表格' : '复制代码'
  return '<button type="button" class="mdv-copy" data-k="' + kind + '"' + (extraAttrs || '') +
    ' title="' + label + '" aria-label="' + label + '">' + iconSvg('i-copy') + '</button>'
}

md.renderer.rules.fence = (tokens, idx) => {
  const token = tokens[idx]
  const info = (token.info || '').trim()
  const lang = info.split(/\s+/)[0].toLowerCase()
  const code = token.content

  if (lang === 'mermaid' && state.render.mermaid) {
    return '<div class="mermaid-block" data-mermaid="pending">'
      + '<pre class="mermaid-src"><code>' + escapeHtml(code) + '</code></pre>'
      + '</div>\n'
  }

  const lines = countLines(code)
  const bytes = utf8Bytes(code)
  const avgLine = bytes / Math.max(1, lines)
  const tooLong = avgLine > HIGHLIGHT_MAX_AVG_LINE
  const tooHuge = lines > HIGHLIGHT_MAX_LINES || bytes > HIGHLIGHT_MAX_BYTES || tooLong

  let body
  if (lang && state.render.highlight && !tooHuge && hljs.getLanguage(lang)) {
    body = hljs.highlight(code, { language: lang, ignoreIllegals: true }).value
  } else {
    body = escapeHtml(code)
  }
  // 空白符标记：字形在伪元素里，文本节点原样保留
  body = markWhitespace(body, state.render.codeWs, true)

  const cls = 'language-' + (lang || 'none')
  const note = tooHuge
    ? '<div class="code-note">' + (tooLong ? '代码块单行过长（' : '代码块过大（')
      + lines + ' 行 / ' + Math.round(bytes / 1024) + ' KiB），已关闭高亮</div>'
    : ''

  // 复制按钮取的是 <code> 的 textContent：hljs 只加 span 不改字符，
  // 所以拿到的就是源码原文，不用另存一份。
  const btn = state.render.copyCode ? copyButtonHtml('code') : ''
  // 行号格子挂在 <code> 里而不是 <pre> 里：<pre> 内可能先有一个 .code-note，
  // 锚在 <code> 上就不用管它把首行推下去多少。
  let gutter = ''
  let lnCls = ''
  if (state.render.lineNumbers) {
    const shown = displayLines(code)
    gutter = lineGutter(shown)
    lnCls = ' has-ln ' + lineGutterWidth(shown)
  }
  return '<div class="mdv-cb">' + btn
    + '<pre>' + note + '<code class="hljs ' + cls + lnCls + '">' + gutter + body
    + '</code></pre></div>\n'
}

// 缩进式代码块（4 空格缩进、非围栏）：markdown-it 默认规则只给一段纯文本，
// 没有行号也没有高亮。设置里打开开关后才套上行号与空白符标记；
// 没有语言信息，语法高亮无从谈起，也不给复制按钮。
const defaultCodeBlock = md.renderer.rules.code_block

md.renderer.rules.code_block = (tokens, idx, options, env, self) => {
  if (!state.render.indentedBlocks) {
    return defaultCodeBlock(tokens, idx, options, env, self)
  }
  const code = tokens[idx].content
  const body = markWhitespace(escapeHtml(code), state.render.codeWs, true)
  let gutter = ''
  let lnCls = ''
  if (state.render.lineNumbers) {
    const shown = displayLines(code)
    gutter = lineGutter(shown)
    lnCls = ' has-ln ' + lineGutterWidth(shown)
  }
  return '<pre><code' + (lnCls ? ' class="' + lnCls.trim() + '"' : '') + '>' + gutter + body
    + '</code></pre>\n'
}

// 表格：按钮挂在 </table> 后面，但绝对定位在包层右上角，所以视觉上在表格角上。
// Markdown 源码靠 table_close 的 map 回源文档切片，渲染规则里拿不到原文。
const defaultTableOpen = md.renderer.rules.table_open ||
  ((tokens, idx, options, env, self) => self.renderToken(tokens, idx, options))
const defaultTableClose = md.renderer.rules.table_close ||
  ((tokens, idx, options, env, self) => self.renderToken(tokens, idx, options))

// 内层 .mdv-cb__scroll 负责横向滚动，复制按钮留在不滚动的外层：
// 否则按钮是滚动容器的绝对定位子元素，滚到表格右侧时会被带着滚出视野。
md.renderer.rules.table_open = (tokens, idx, options, env, self) =>
  '<div class="mdv-cb mdv-cb--table"><div class="mdv-cb__scroll">' +
  defaultTableOpen(tokens, idx, options, env, self)

md.renderer.rules.table_close = (tokens, idx, options, env, self) => {
  // markdown-it 只给 table_open 挂 map（table_close 的 map 是空的），
  // 而结束行号是解析完才回填进 open 那个数组的——所以要回头找 open。
  let open = idx - 1
  while (open >= 0 && tokens[open].type !== 'table_open') open--
  const map = open >= 0 ? tokens[open].map : null
  let btn = ''
  if (state.render.copyTable && map && env) {
    const lines = ' data-l0="' + map[0] + '" data-l1="' + map[1] + '"'
    if (Number.isInteger(env.blockIndex)) {
      // 线上路径：逐块解析，行号相对本块，点的时候按块号回源
      btn = copyButtonHtml('table', ' data-src="b" data-i="' + env.blockIndex + '"' + lines)
    } else if (typeof env.docText === 'string') {
      // 开发模式整篇解析：行号相对全文，源码就在手里
      btn = copyButtonHtml('table', ' data-src="d"' + lines)
    }
  }
  return defaultTableClose(tokens, idx, options, env, self) + '</div>' + btn + '</div>'
}

/* ---------------- 清洗与 URL 改写 ---------------- */

// 图片：src 必须在渲染期就定下来。改写只写在 DOMPurify 钩子里是不够的——
// sanitizeBlock 会跳过「源码不含 < 的块」，`![](pic.png)` 正是这种块，钩子根本
// 不执行，src 原样留在 HTML 里被按 shell.html 的基址解析成 mdprev.localhost/pic.png，
// 既不进 __asset 也不报日志，变成一张破图。
const defaultImage = md.renderer.rules.image ||
  ((tokens, idx, options, env, self) => self.renderToken(tokens, idx, options))

md.renderer.rules.image = (tokens, idx, options, env, self) => {
  const t = tokens[idx]
  const at = t.attrIndex('src')
  if (at >= 0) {
    const a = imagePolicy(t.attrs[at][1])
    if (a.src) t.attrs[at][1] = a.src
    else t.attrs.splice(at, 1)
    if (a.mdvSrc) {
      t.attrPush(['data-mdv-src', a.mdvSrc])
      const ci = t.attrIndex('class')
      if (ci >= 0) t.attrs[ci][1] += ' mdv-net-ph'
      else t.attrPush(['class', 'mdv-net-ph'])
    }
    if (a.title) t.attrPush(['title', a.title])
  }
  // alt 交给默认规则填（它会按 markdown-it 的规则转义）
  return defaultImage(tokens, idx, options, env, self)
}

const TRANSPARENT_IMG = 'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7'

// 网络图片的确认状态。键是 URL.host（域名 + 非默认端口，不含协议）。
// 答复按「文档 + 主机」记：同一篇里重复出现不再问，换到别的文档要重新问，
// 切回原来的文档仍然记得。文档由 index 帧的文档键标识（小写路径+字节数+写入时间），
// 会话由 sessionId 标识——两者任一不符就是上一轮 DO 留下的，整份作废。
//
//   allowed / denied —— "每主机"档按主机记的答复；
//   decided         —— "每文档"档的整篇答复（一篇只有一个，不看主机）。
//
// 记忆落在 localStorage：页面被回收重建（关光预览窗格后空闲释放、渲染进程崩溃、
// 插件重载）之后仍然在，只有 DO 重启换了 sessionId 才清空。
const NET = {
  mode: 1,             // 0 不询问 / 1 每主机一次 / 2 每文档一次
  session: '',         // 宿主给的会话 id；空 = 还没收到 index
  docs: null,          // Map<文档哈希, {allowed, denied, decided, used}>，懒加载
  key: '',             // 当前文档的哈希键
  cur: null,           // docs.get(key) 的引用
  used: 0,             // 最近使用序，淘汰靠它
  queue: [],
  current: null,
  panel: null,
  title: null,
}

const NET_STORE_KEY = 'mdvNetConsent'
const NET_STORE_VER = 1
// 一格只有几个主机名，但会话再长也该有上界：超了按最近使用淘汰，那篇文档再开就重问
const NET_MAX_DOCS = 64

function isRemoteUrl(raw) {
  return /^(?:https?:)?\/\//i.test(raw || '')
}

// DOMPurify 会把 `C:` 当成未知协议、把 src 整个删掉；在它做 URI 检查之前先换成
// __asset 的 https 地址，绝对路径与 file:/// 才到得了 afterSanitizeAttributes。
DOMPurify.addHook('uponSanitizeAttribute', (node, data) => {
  if (data.attrName !== 'src' || !data.attrValue) return
  if (node.tagName !== 'IMG' && node.tagName !== 'INPUT') return
  const v = data.attrValue
  const file = /^file:\/\/(.*)$/i.exec(v)
  if (file) {
    let p = file[1]
    try { p = decodeURIComponent(p) } catch { /* 保留原文 */ }
    if (/^\/[a-zA-Z]:/.test(p)) p = p.slice(1)        // file:///C:/x
    else if (!p.startsWith('/')) p = '\\\\' + p       // file://server/share
    data.attrValue = assetUrl(p)
    return
  }
  if (/^[a-zA-Z]:[\\/%]/.test(v)) data.attrValue = assetUrl(decodePath(v))
})

DOMPurify.addHook('afterSanitizeAttributes', (node) => {
  // 远程背景图 / 视频封面 / SVG 引用会绕过 <img> 的确认，直接去掉。
  if (node.hasAttribute && node.hasAttribute('style')) {
    const style = node.getAttribute('style')
    if (/url\(\s*['"]?(?:https?:)?\/\//i.test(style)) node.removeAttribute('style')
  }
  if (node.hasAttribute && node.hasAttribute('background')) {
    if (isRemoteUrl(node.getAttribute('background'))) node.removeAttribute('background')
  }
  if (node.tagName === 'VIDEO' && node.hasAttribute('poster')) {
    if (isRemoteUrl(node.getAttribute('poster'))) node.removeAttribute('poster')
  }
  if (node.tagName === 'image' || node.tagName === 'IMAGE' || node.tagName === 'use') {
    for (const attr of ['href', 'xlink:href']) {
      if (node.hasAttribute && node.hasAttribute(attr) && isRemoteUrl(node.getAttribute(attr))) {
        node.removeAttribute(attr)
      }
    }
  }
  if (node.tagName === 'INPUT' && (node.getAttribute('type') || '').toLowerCase() === 'image') {
    if (isRemoteUrl(node.getAttribute('src'))) node.removeAttribute('src')
  }

  if (node.tagName === 'IMG') {
    const src = node.getAttribute('src')
    if (!src) return
    // 内联 HTML 走的 <img> 也按同一套策略定 src（Markdown 图片在渲染期就算好了）。
    // 不设 loading=lazy：块级虚拟化已经只保留视口附近的块，原生懒加载在
    // translate3d 的虚拟滚动里判定不准，会让图片一直不发起请求。
    const a = imagePolicy(src)
    if (a.src) node.setAttribute('src', a.src)
    else node.removeAttribute('src')
    if (a.mdvSrc) {
      node.setAttribute('data-mdv-src', a.mdvSrc)
      node.classList.add('mdv-net-ph')
    }
    if (a.title) node.setAttribute('title', a.title)
    return
  }
  if (node.tagName === 'A') {
    const href = node.getAttribute('href')
    if (!href) return
    node.setAttribute('rel', 'noreferrer noopener')
    // 页内锚点留给 shell 处理，其余交回宿主 ShellExecute
    if (href.charAt(0) !== '#') node.setAttribute('data-external', resolveDocUrl(href))
  }
})

// 图片走 __asset：虚拟主机映射出来的跨源 <img> 在 WebView2 里不生效（onerror），
// 而 mdblocks.localhost 的请求由 C++ 合成，可带正确的 Content-Type。
// 绝对路径与 ../ 也统一走 __asset，宿主按设置决定放不放行。
function assetUrl(path) {
  return 'https://mdblocks.localhost/__asset?p=' + encodeURIComponent(path)
}

function decodePath(raw) {
  try {
    return decodeURIComponent(raw)
  } catch {
    return raw
  }
}

// 本地图片一律走 __asset。markdown-it 已经把路径里的非 ASCII 字符百分号化了，
// 所以要先解一层再编码一次：直接对 raw 再 encodeURIComponent 会二次编码，中文或
// 带空格的文件名会变成 %25E5…，宿主只解一层、拿到字面量 %E5…，文件必然找不到。
function resolveImageUrl(raw) {
  if (!raw) return raw
  if (/^(?:data|blob):/i.test(raw)) return raw
  if (/^\/\//.test(raw)) return raw  // 协议相对 URL（https:）
  const file = /^file:\/\/(.*)$/i.exec(raw)
  if (file) {
    let p = decodePath(file[1])
    if (/^\/[a-zA-Z]:/.test(p)) p = p.slice(1)        // file:///C:/x
    else if (!p.startsWith('/')) p = '\\\\' + p       // file://server/share
    return assetUrl(p)
  }
  const decoded = decodePath(raw)
  // Windows 盘符绝对路径 / UNC
  if (/^[a-zA-Z]:[\\/]/.test(decoded) || /^\\\\/.test(decoded)) return assetUrl(decoded)
  // 其它协议（mailto 等）原样交给浏览器
  if (/^[a-z][a-z0-9+.-]*:/i.test(raw)) return raw
  return assetUrl(decoded)  // 文档目录内（含 ../，宿主按设置决定放不放行）
}

// 图片该带什么属性：本地图走 __asset，远程图先放透明占位等页内确认，
// data/blob 属于文档自身直接放行。「本地图片」开关关掉时连 src 都不给。
function imagePolicy(raw) {
  if (!raw) return {}
  if (/^(?:data|blob):/i.test(raw)) return { src: raw }
  if (isRemoteUrl(raw)) {
    // 已经被上面的钩子换成本地 __asset 的，不要再当成网络图去走确认
    if (/^https:\/\/mdblocks\.localhost\//i.test(raw)) return { src: raw }
    let abs = null
    try { abs = new URL(raw, location.href) } catch { abs = null }
    // CSP 只放行 https:：http 等协议不发起请求，只留 alt
    if (!abs || abs.protocol !== 'https:') return {}
    return { src: TRANSPARENT_IMG, mdvSrc: abs.href }
  }
  if (!state.render.localImages) return { title: '本地图片已关闭' }
  return { src: resolveImageUrl(raw) }
}

function resolveDocUrl(raw) {
  if (!raw) return raw
  if (/^(?:[a-z][a-z0-9+.-]*:|#|\/\/)/i.test(raw)) return raw
  try {
    return new URL(raw, state.docBase).href
  } catch {
    return raw
  }
}

function sanitize(html) {
  return DOMPurify.sanitize(html, {
    WHOLE_DOCUMENT: false,
    RETURN_DOM_FRAGMENT: false,
    // use 不在默认 SVG 白名单里，href 也只对 a 放行：复制按钮的 <use href="#i-copy">
    // 不加这两项会被整条剥掉，只剩一个空 <svg>
    ADD_TAGS: ['use'],
    ADD_ATTR: ['data-mermaid', 'data-external', 'data-state', 'href'],
    FORBID_TAGS: ['style', 'link', 'meta', 'base'],
    FORBID_ATTR: ['srcset'],
  })
}

/* ---------------- 状态 ---------------- */

const state = {
  docBase: DOC_BASE_DEFAULT,
  skinId: 'light',
  mermaidState: 'idle',
  mermaidApi: null,
  generation: 0,
  render: {
    mermaid: true,
    katex: true,
    highlight: true,
    inlineHtml: true,
    localImages: true,
    copyCode: true,      // 代码块右上角的复制按钮（写进块 HTML，改了要重渲染）
    copyTable: true,     // 表格右上角的复制按钮（同上）
    lineNumbers: true,   // 代码块行号（同上）
    codeWs: WS_BOUNDARY, // 空白符标记：0 关 / 1 仅选中 / 2 边界 / 3 全部（写进块 HTML）
    indentedBlocks: false, // 缩进式代码块也套用行号与空白符（同上）
    tableCopyMd: false,  // 表格复制 Markdown 源码而不是 TSV；纯点击时行为，不进 HTML
    tableWrap: true,     // 表格长内容自动换行；关掉后单行 + 包装层横向滚动
  },
  chrome: {
    find: true,     // 工具条搜索按钮
    outline: true,  // 工具条大纲按钮
  },
  // 整篇渲染（开发模式）时的全文源码；线上走逐块渲染，这里一直是 null
  docText: null,
}

const MATH_RULES = ['math_inline', 'math_inline_block', 'math_block']

const V = {
  active: false,
  docId: 0,
  blockCount: 0,
  prefixCount: 0,
  estTotal: 0,
  fullIndex: false,
  lastVisible: 0,   // 最后一个非隐藏块：脚注区挂在它后面
  fileSize: 0,
  enc: 0,
  heights: null,
  measured: null,
  hidden: null,
  fenwick: null,
  totalH: 0,
  defaultH: 120,
  off: 0,
  target: 0,
  vh: 0,
  anchorId: 0,
  anchorDy: 0,
  anchorSet: false,
  windowStart: -1,
  windowEnd: -1,
  stageBase: 0,
  topsDirty: true,
  topsFrom: 0,            // 高度变化后，从这一块起需要重算 top
  resident: new Map(),
  pool: [],
  bigPool: [],
  pinned: new Set(),     // 选区覆盖、暂时不换出的块号
  pinsDirty: false,      // 钉块集合缩小过：即使窗口没变也要回收一次
  big: new Map(),        // 块号 -> { lines }（>2000 行的块）
  bigTable: new Map(),   // 块号 -> { lines }（>20000 行的表格块，走分块真表格）
  bigPending: null,      // 页面还没就绪时收到的巨型块表
  pending: new Set(),
  fetching: new Set(),
  queue: [],
  pumpRaf: 0,
  placeRaf: 0,
  raf: 0,
  lastTick: 0,
  lastWheelAt: 0,
  lastScrollAt: 0,         // 最后一次用户滚动：补块任务据此让路
  trackH: 0,             // 滚动条轨道高度缓存（0 = 需要重读）
  thumbH: '',
  thumbTop: '',
  initializing: false,
  indexAt: 0,
  drag: null,
  flashBlock: -1,        // 待闪烁的块号（锚点跳转后还没渲染出来时先记下）
}

const decoder = new TextDecoder('utf-8')
const KIND_NAMES = ['none', 'para', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6',
  'fence', 'table', 'list', 'quote', 'html', 'hr', 'fm']

/* 页内搜索与大纲。命中在 C++ 里按源字节找，块内偏移换成解码文本的
   UTF-16 单元下标；前端在渲染后的文本里找最近的一处真实出现位置再包 mark——
   源码里的 `**`、链接语法不在渲染文本里，纯按下标切会偏。 */
const FIND = {
  open: false,
  query: '',
  cs: false,
  ww: false,
  re: false,
  error: '',
  limited: false,
  results: [],
  index: -1,
  total: 0,
  truncated: false,
  busy: false,
  seq: 0,
  timer: 0,
  pending: null,
  current: null,
  bigHit: null,  // 巨型块里待高亮的行：{ b, line }，等行窗口到位后包 mark
}

const PRINT = {
  busy: false,
  root: null,
  docId: 0,
  timer: 0,
}

const COPY = { busy: false }

const OUT = {
  items: [],
  loaded: false,
  loading: false,
  total: 0,
  truncated: false,
  seq: 0,
}

/* 标题 slug：GitHub 风格、按出现顺序去重，重名追加 -1 / -2。
   清单来自 __outline 的前 1 万条，超过这个数的标题没有锚点——与大纲同一条上限。 */
const SLUG = {
  bySlug: new Map(),   // slug -> 块号
  byBlock: new Map(),  // 块号 -> slug
}
const SLUG_TAG = /<[!\/a-z].*?>/gi
const SLUG_DROP = /[\u2000-\u206F\u2E00-\u2E7F\\'!"#$%&()*+,./:;<=>?@[\]^`{|}~]/g

let docEl = null
let sbEl = null
let thumbEl = null
let toolsEl = null
let findEl = null
let findInput = null
let findCount = null
let findCaseBtn = null
let findWordBtn = null
let findRegexBtn = null
let outlineEl = null
let outlineList = null

function bindDom() {
  docEl = document.getElementById('doc')
  sbEl = document.getElementById('sb')
  thumbEl = document.getElementById('sb-thumb')
  toolsEl = document.getElementById('tools')
  findEl = document.getElementById('find')
  findInput = document.getElementById('find-input')
  findCount = document.getElementById('find-count')
  findCaseBtn = document.getElementById('find-case')
  findWordBtn = document.getElementById('find-word')
  findRegexBtn = document.getElementById('find-regex')
  outlineEl = document.getElementById('outline')
  outlineList = document.getElementById('outline-list')
}

/* ---------------- 几何 ---------------- */

function emPx(mult) {
  const fs = parseFloat(getComputedStyle(document.body).fontSize) || 15
  return fs * mult
}
const topPad = () => emPx(1.5)
const bottomPad = () => emPx(3)

/* ---------------- 巨型块几何 ---------------- */

// 块高 = 块顶留白 + 边框/内边距 + 行数 × 行高。行高从实际 CSS 取，
// 不写死 24.75：字号/行高/皮肤一改，这里必须跟着变。
const big = { ready: false, lineH: 0, overhead: 0 }

function bigMetrics() {
  if (big.ready) return big
  const host = document.createElement('div')
  host.className = 'blk blk--fence blk--big'
  host.style.cssText =
    'position:absolute;left:-100000px;top:0;width:600px;height:200px;visibility:hidden'
  host.innerHTML = '<div class="big-frame"><pre class="big-pre"><code>x</code></pre></div>'
  document.body.appendChild(host)

  const frame = host.firstElementChild
  const pre = frame.firstElementChild
  const cs = getComputedStyle(frame)
  const fs = parseFloat(getComputedStyle(document.body).fontSize) || 15
  big.lineH = parseFloat(getComputedStyle(pre).lineHeight) || fs * 1.65
  big.overhead =
    (parseFloat(getComputedStyle(host).paddingTop) || 0) +
    (parseFloat(cs.paddingTop) || 0) +
    (parseFloat(cs.paddingBottom) || 0) +
    (parseFloat(cs.borderTopWidth) || 0) +
    (parseFloat(cs.borderBottomWidth) || 0)
  document.body.removeChild(host)
  big.ready = true
  return big
}

function heightOf(i) {
  if (V.big.has(i)) return bigHeight(i)
  // 巨型表格的高度只能实测（单元格换行让行高不固定），实测前按行数估一个，
  // 否则滚动条位置会随着分块陆续到位一路跳。
  if (V.bigTable.has(i)) {
    // ★ 只有 setHeight 能改块高：这里一旦按内部状态（行高、是否补完）算出别的数，
    // Fenwick 的增量维护就和它对不上，总高会算成负数。
    // 补块过程中的高度由 setHeight 写成「已测行高 × 总行数」的外推值。
    if (V.measured[i] && V.heights[i] > 0) return V.heights[i]
    const m = V.bigTable.get(i)
    return BT_EST_HEAD + Math.max(1, (m && m.lines) || 1) * BT_EST_ROW
  }
  return V.measured[i] ? V.heights[i] : V.defaultH
}

// Fenwick：heights[0..i-1] 的和；块 i 的文档位置 = topPad + fwSum(i)
function fwSum(i) {
  let s = 0
  for (let k = i; k > 0; k -= k & -k) s += V.fenwick[k]
  return s
}

function fwAdd(i, delta) {
  const n = V.blockCount
  for (let k = i + 1; k <= n; k += k & -k) V.fenwick[k] += delta
}

function pos(i) {
  return topPad() + fwSum(i)
}

function rebuildFenwick() {
  const n = V.blockCount
  const fw = V.fenwick
  fw.fill(0)
  for (let i = 1; i <= n; i++) {
    fw[i] += heightOf(i - 1)
    const j = i + (i & -i)
    if (j <= n) fw[j] += fw[i]
  }
  // fw[n] 只是树节点，不是前缀和；n 不是 2 的幂时会少算一大截。
  V.totalH = topPad() + fwSum(n) + bottomPad()
  // 重建意味着一批高度同时变了（首屏、字号、巨型块布局），所有块的位置都要重算
  V.topsDirty = true
  V.topsFrom = 0
}

function offsetToBlock(off) {
  const n = V.blockCount
  if (n <= 0) return 0
  const x = Math.max(0, off - topPad())
  let idx = 0
  let acc = 0
  let bit = 1
  while (bit <= n) bit <<= 1
  for (bit >>= 1; bit; bit >>= 1) {
    const next = idx + bit
    if (next <= n && acc + V.fenwick[next] <= x) {
      idx = next
      acc += V.fenwick[next]
    }
  }
  return Math.min(idx, n - 1)
}

function setHeight(i, h) {
  if (!V.active || i < 0 || i >= V.blockCount) return
  if (!Number.isFinite(h)) h = 0
  const old = heightOf(i)
  if (V.measured[i] && Math.abs(old - h) < 0.5) return
  V.measured[i] = 1
  V.heights[i] = h
  if (V.initializing) return
  const delta = h - old
  if (delta === 0) return
  fwAdd(i, delta)
  // 总高按树算，不用累加：h 有时是占位值（巨型表格还没插内容就被量到 0），
  // 累加会把 totalH 减成负数，滚动条直接卡死在顶部。
  V.totalH = topPad() + fwSum(V.blockCount) + bottomPad()
  if (V.topsDirty) {
    if (i < V.topsFrom) V.topsFrom = i
  } else {
    V.topsDirty = true
    V.topsFrom = i
  }
  if (V.anchorSet && i < V.anchorId) {
    // 锚点之上变高/变矮：把滚动位置一起挪，屏幕内容不动
    V.off = Math.max(0, V.off + delta)
    V.target = Math.max(0, V.target + delta)
  }
  schedulePlace()
}

/* ---------------- 渲染 ---------------- */

let mermaidChain = Promise.resolve()
let resizeObserver = null

// 块 HTML 缓存：滚动来回经过同一批块时省掉 markdown-it 与 DOMPurify 的开销。
// 块内容在一份文档内不会变，换文档 / 重算定义 / 改渲染开关时整体清掉。
const HTML_CACHE_MAX = 6 << 20
const htmlCache = new Map()
let htmlCacheBytes = 0

function htmlCacheGet(i) {
  const v = htmlCache.get(i)
  if (v === undefined) return null
  htmlCache.delete(i)
  htmlCache.set(i, v)
  return v
}

function htmlCachePut(i, html) {
  const old = htmlCache.get(i)
  if (old !== undefined) htmlCacheBytes -= old.length
  htmlCache.delete(i)
  htmlCache.set(i, html)
  htmlCacheBytes += html.length
  while (htmlCacheBytes > HTML_CACHE_MAX && htmlCache.size > 1) {
    const [k, v] = htmlCache.entries().next().value
    htmlCache.delete(k)
    htmlCacheBytes -= v.length
  }
}

function htmlCacheClear() {
  htmlCache.clear()
  htmlCacheBytes = 0
}

// 块里没有 `<` 时输出的 HTML 全部由 markdown-it / KaTeX 生成，不需要再过 DOMPurify
function sanitizeBlock(html, text) {
  return text.includes('<') ? sanitize(html) : html
}

function renderBlockHtml(text, flags, i) {
  const cached = htmlCacheGet(i)
  if (cached !== null) return cached
  let html
  try {
    const env = blockEnv()
    // 表格复制按钮要带块号，点击时才回源文档按 token.map 切 Markdown 源码
    env.blockIndex = i
    const slug = SLUG.byBlock.get(i)
    if ((flags & 0x10) || slug) {
      const tokens = md.parse(text, env)
      if (flags & 0x10) {
        for (const t of tokens) {
          if (t.type === 'paragraph_open' || t.type === 'paragraph_close') t.hidden = false
        }
      }
      if (slug) {
        for (const t of tokens) {
          if (t.type === 'heading_open') {
            t.attrSet('id', slug)
            break
          }
        }
      }
      html = sanitizeBlock(md.renderer.render(tokens, md.options, env), text)
    } else {
      html = sanitizeBlock(md.render(text, env), text)
    }
  } catch (err) {
    report('render-error', String(err))
    html = '<pre>' + escapeHtml(text) + '</pre>'
  }
  htmlCachePut(i, html)
  return html
}

/* ---------------- 跨块定义：链接引用与脚注 ---------------- */

function stripDefIndent(raw) {
  const lines = raw.split(/\r?\n/)
  for (let i = 1; i < lines.length; i++) lines[i] = lines[i].replace(/^ {1,4}/, '')
  return lines.join('\n')
}

function buildFootHtml() {
  if (!FOOT.defs.length) return ''
  const items = []
  for (const d of FOOT.defs) {
    const back = '<a class="footnote-backref" href="#fnref' + d.num + '">\u21a9</a>'
    let body = ''
    try {
      body = sanitize(md.render(d.body, blockEnv()))
    } catch {
      body = '<p>' + escapeHtml(d.body) + '</p>'
    }
    // 回跳箭头塞进最后一段，别单独占一行
    body = /<\/p>\s*$/.test(body) ? body.replace(/<\/p>\s*$/, ' ' + back + '</p>')
                                     : body + back
    items.push('<li id="fn' + d.num + '">' + body + '</li>')
  }
  return '<section class="footnotes"><ol>' + items.join('') + '</ol></section>'
}

// 隐藏块原文：先拼链接引用定义喂一次 markdown-it（归一化交给它），
// 再按定义顺序给脚注编号。
function applyDefs(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const refTexts = []
  const footTexts = []
  let o = 0
  while (o + 8 <= bytes.byteLength) {
    const flags = dv.getUint32(o, true); o += 4
    const len = dv.getUint32(o, true); o += 4
    if (o + len > bytes.byteLength) break
    const text = decoder.decode(bytes.subarray(o, o + len)); o += len
    if (flags & 0x40) footTexts.push(text)
    else refTexts.push(text)
  }

  const env = {}
  if (refTexts.length) {
    try { md.parse(refTexts.join('\n\n'), env) } catch { /* 坏定义不影响正文 */ }
  }
  REF.references = env.references || null

  FOOT.defs = []
  FOOT.nums.clear()
  for (const text of footTexts) {
    const m = /^\s*\[\^([^\]]+)\]:[ \t]*/.exec(text)
    if (!m) continue
    const label = normLabel(m[1])
    if (FOOT.nums.has(label)) continue
    const num = FOOT.defs.length + 1
    FOOT.defs.push({ label, num, body: stripDefIndent(text.slice(m[0].length)) })
    FOOT.nums.set(label, num)
  }
  FOOT.html = buildFootHtml()
  htmlCacheClear()

  report('defs', 'refs=' + (REF.references ? Object.keys(REF.references).length : 0)
    + ' foot=' + FOOT.defs.length)
  rerenderResidents()
}

function loadDefs() {
  const docId = V.docId
  fetch(DEFS_URL, { cache: 'no-store' })
    .then((res) => (res.ok ? res.arrayBuffer() : null))
    .then((buf) => {
      if (!buf || docId !== V.docId) return
      applyDefs(new Uint8Array(buf))
    })
    .catch((err) => {
      if (docId === V.docId) report('defs-error', String(err))
    })
}

function rerenderResidents() {
  if (!V.active) return
  for (const [i, rec] of V.resident) {
    if (rec.big || !rec.el || !rec.text) continue
    let html = renderBlockHtml(rec.text, rec.flags, i)
    if (i === V.lastVisible && FOOT.html) html += FOOT.html
    rec.el.innerHTML = html
    processNetImages(rec.el)
    setHeight(i, rec.el.offsetHeight)
    queueMermaid(rec.el, V.docId)
  }
  reapplyCurrentHit()
  schedulePlace()
}

function queueMermaid(el, docId) {
  if (!el.querySelector('[data-mermaid="pending"]')) return
  const generation = state.generation
  mermaidChain = mermaidChain
    .then(() => (docId === V.docId ? renderMermaidBlocks(el, generation) : undefined))
    .catch(() => {})
}

function ensureObserver() {
  if (resizeObserver) return
  resizeObserver = new ResizeObserver((entries) => {
    if (!V.active) return
    for (const entry of entries) {
      const i = Number(entry.target.dataset.i)
      if (!Number.isInteger(i)) continue
      const rec = V.resident.get(i)
      if (!rec) continue
      // 巨型表格观察的是内部的 host（块本身的高度由它决定），其余块观察块元素
      if (rec.el !== entry.target && !(rec.table && rec.table.host === entry.target)) continue
      const h = entry.target.offsetHeight
      if (rec.table) {
        if (h <= 0) continue
        const bt = rec.table
        if (bt.done) {
          setHeight(i, h)
          continue
        }
        // 补块途中：用「已补部分的平均行高」外推整表。这样滚动条随补块一起长，
        // 而不是补完那一刻从估算值跳到实测值（那一跳会把视口内容整体推走）。
        if (bt.rows > 0) {
          bt.rowH = Math.max(10, (h - bt.headH) / bt.rows)
          const lines = Math.max(1, (V.bigTable.get(i) || {}).lines || 1)
          setHeight(i, bt.headH + lines * bt.rowH)
        }
        continue
      }
      setHeight(i, h)
    }
  })
}

function acquireEl() {
  const el = V.pool.pop()
  if (el) {
    el.className = 'blk'
    el.innerHTML = ''
    el.style.top = ''
    el.style.height = ''
    return el
  }
  const fresh = document.createElement('div')
  fresh.className = 'blk'
  return fresh
}

function ensureConsentPanel() {
  if (NET.panel) return NET.panel
  const panel = document.createElement('div')
  panel.className = 'net-consent ui'
  panel.hidden = true
  panel.innerHTML =
    '<div class="net-consent-text">' +
    '<span class="net-consent-title"></span>' +
    '</div>' +
    '<div class="net-consent-actions">' +
    '<button type="button" data-act="deny">拒绝</button>' +
    '<button type="button" data-act="allow">允许</button>' +
    '</div>'
  panel.querySelector('[data-act="allow"]').addEventListener('click', () => decideNetConsent(true))
  panel.querySelector('[data-act="deny"]').addEventListener('click', () => decideNetConsent(false))
  document.body.appendChild(panel)
  NET.panel = panel
  NET.title = panel.querySelector('.net-consent-title')
  return panel
}

function showNetPrompt(host) {
  const panel = ensureConsentPanel()
  // 档位 2 的一票管整篇所有来源（设置项写的就是「答一次管所有来源」），
  // 标题只报一个主机就与真实影响不符——这里点的是整篇的开关。
  NET.title.textContent = NET.mode === 2
    ? '允许加载本篇里所有来源的网络图片吗？（第一个来自 ' + host + '）'
    : '允许加载来自 ' + host + ' 的网络图片吗？'
  panel.hidden = false
}

function hideNetPrompt() {
  if (NET.panel) NET.panel.hidden = true
}

// 落盘前再哈希一次：磁盘上不留本地路径与文件名，排查靠日志里那行「文档键」。
// 页内比较用的也是这个哈希，所以磁盘与内存是同一套键。
function netHash(text) {
  let h = 0xcbf29ce484222325n
  for (let i = 0; i < text.length; i++) {
    h ^= BigInt(text.charCodeAt(i))
    h = (h * 0x100000001b3n) & 0xffffffffffffffffn
  }
  return h.toString(16).padStart(16, '0')
}

// 从 localStorage 读回记忆。会话 id 不符、版本不符、解析失败都当成"没有"——
// 宁可从零开始问，也不要拿上一轮 DO 的答复放行。
function netStoreLoad() {
  const docs = new Map()
  NET.used = 0
  if (!NET.session) return docs
  let raw = null
  try { raw = window.localStorage.getItem(NET_STORE_KEY) } catch { return docs }
  if (!raw) return docs
  let obj = null
  try { obj = JSON.parse(raw) } catch { return docs }
  if (!obj || obj.v !== NET_STORE_VER || obj.sid !== NET.session || !obj.docs) return docs
  for (const [k, rec] of Object.entries(obj.docs)) {
    if (!rec) continue
    const used = Number(rec.t) || 0
    if (used > NET.used) NET.used = used
    docs.set(k, {
      allowed: new Set(Array.isArray(rec.a) ? rec.a : []),
      denied: new Set(Array.isArray(rec.d) ? rec.d : []),
      decided: rec.x === 0 || rec.x === 1 ? rec.x === 1 : null,
      used,
    })
  }
  return docs
}

function netStoreSave() {
  if (!NET.session || !NET.docs) return
  if (NET.docs.size > NET_MAX_DOCS) {
    const order = [...NET.docs.entries()].sort((a, b) => a[1].used - b[1].used)
    for (const [k] of order.slice(0, NET.docs.size - NET_MAX_DOCS)) NET.docs.delete(k)
  }
  const docs = {}
  for (const [k, rec] of NET.docs) {
    docs[k] = {
      a: [...rec.allowed],
      d: [...rec.denied],
      x: rec.decided === null ? null : (rec.decided ? 1 : 0),
      t: rec.used,
    }
  }
  try {
    window.localStorage.setItem(NET_STORE_KEY,
      JSON.stringify({ v: NET_STORE_VER, sid: NET.session, docs }))
  } catch {
    // 存储被禁用或写满：记忆退化成只在本页有效，放行逻辑不受影响
  }
}

// 当前文档那一格；没有就建。文档键为空（设置页预览这类场景）用固定格兜住。
function netEntry() {
  if (!NET.docs) NET.docs = netStoreLoad()
  if (!NET.key) NET.key = 'none'
  let rec = NET.docs.get(NET.key)
  if (!rec) {
    rec = { allowed: new Set(), denied: new Set(), decided: null, used: 0 }
    NET.docs.set(NET.key, rec)
  }
  rec.used = ++NET.used
  NET.cur = rec
  return rec
}

// index 到达：切到这篇文档的答复。换文档本身不清记忆（切回来还要用），
// 只有会话变了才整份作废。
function netOpenDoc(docKey, session) {
  if (session !== NET.session) {
    NET.session = session || ''
    NET.docs = null  // 上一轮 DO 留下的，读都不用读
    NET.key = ''
    NET.cur = null
  }
  NET.key = docKey ? netHash(docKey) : ''
  NET.cur = null
  netEntry()
}

// 打印视图与页内共用一套判定：只查，不建格子
function netAllowedNow(host) {
  if (NET.mode === 0) return true
  const rec = NET.cur
  if (!rec) return false
  if (NET.mode === 2) return rec.decided === true
  return rec.allowed.has(host)
}

function ensureNetConsent(host) {
  if (NET.mode === 0) return Promise.resolve(true)
  const rec = netEntry()
  if (NET.mode === 2) {
    if (rec.decided !== null) return Promise.resolve(rec.decided)
  } else {
    if (rec.allowed.has(host)) return Promise.resolve(true)
    if (rec.denied.has(host)) return Promise.resolve(false)
  }
  return new Promise((resolve) => {
    // 入队时把格子一起带上：弹窗期间换了文档，答复仍要记在当初那篇名下
    NET.queue.push({ host, rec, resolve })
    pumpNetConsent()
  })
}

// 一次只问一个来源；档位 2 的答复管整篇，档位 1 的答复只针对当前主机
function pumpNetConsent() {
  if (NET.current) return
  while (NET.queue.length) {
    const item = NET.queue[0]
    if (NET.mode === 0) {
      NET.queue.shift()
      item.resolve(true)
      continue
    }
    if (NET.mode === 2 && item.rec.decided !== null) {
      NET.queue.shift()
      item.resolve(item.rec.decided)
      continue
    }
    if (NET.mode === 1 && item.rec.allowed.has(item.host)) {
      NET.queue.shift()
      item.resolve(true)
      continue
    }
    if (NET.mode === 1 && item.rec.denied.has(item.host)) {
      NET.queue.shift()
      item.resolve(false)
      continue
    }
    NET.queue.shift()
    NET.current = item
    showNetPrompt(item.host)
    return
  }
}

function decideNetConsent(allow) {
  const item = NET.current
  if (!item) return
  NET.current = null
  if (NET.mode === 2) {
    item.rec.decided = allow
  } else if (NET.mode === 1) {
    if (allow) item.rec.allowed.add(item.host)
    else item.rec.denied.add(item.host)
  }
  item.rec.used = ++NET.used
  netStoreSave()
  hideNetPrompt()
  item.resolve(allow)
  pumpNetConsent()
}

function setNetMode(mode) {
  NET.mode = (mode === 0 || mode === 2) ? mode : 1
  // 档位切到“不询问”：正在问的直接放行；其余排队项交给 pump 按新档位处理
  if (NET.mode === 0 && NET.current) {
    const item = NET.current
    NET.current = null
    hideNetPrompt()
    item.resolve(true)
  }
  pumpNetConsent()
}

function dropNetPrompts() {
  // 换文档：在排队的问答作废（答了也不是这篇的），记忆本身留着——切回来还要用。
  // 真正的失效只有两种：换了会话（DO 重启）、或这一篇被 64 篇的上限淘汰掉。
  for (const item of NET.queue) item.resolve(false)
  if (NET.current) NET.current.resolve(false)
  NET.queue = []
  NET.current = null
  hideNetPrompt()
}

function processNetImages(root) {
  const imgs = root.querySelectorAll('img[data-mdv-src]')
  for (const img of imgs) {
    if (img.dataset.mdvNet === '1') continue
    img.dataset.mdvNet = '1'
    img.classList.add('mdv-net-ph')

    // 三种「图没到」的状态都退回同一张 1x1 透明 GIF，让 CSS 的占位框露出来。
    // 图标与边框只走背景，不写文本节点：复制与查找都不受影响。
    const showPlaceholder = (tip) => {
      img.src = TRANSPARENT_IMG
      img.classList.add('mdv-net-ph')
      img.title = tip
    }
    const isRealSrc = () => img.getAttribute('src') === img.dataset.mdvSrc
    img.addEventListener('error', () => {
      if (img.isConnected && isRealSrc()) showPlaceholder('网络图片加载失败')
    })
    img.addEventListener('load', () => {
      if (!isRealSrc()) return
      img.classList.remove('mdv-net-ph')
      if (img.title === '网络图片加载失败' || img.title === '网络图片未加载') {
        img.removeAttribute('title')
      }
    })

    // 键用 host 而不是 origin：同一个域名换个协议/端口不该再问一次（弹窗上显示的也是它）
    let host = ''
    try { host = new URL(img.dataset.mdvSrc).host } catch { continue }
    ensureNetConsent(host).then((allow) => {
      if (!img.isConnected) return
      if (allow) img.src = img.dataset.mdvSrc
      else showPlaceholder('网络图片未加载')
    })
  }
}

function renderItem(item, pendingMeasure) {
  const i = item.i
  if (!V.active || i >= V.blockCount || item.docId !== V.docId) return

  if (item.flags & 0x20) {  // hidden：front matter / 链接与脚注定义
    V.hidden[i] = 1
    setHeight(i, 0)
    return
  }
  if (V.hidden[i]) return
  if (V.bigTable.has(i)) {  // 巨型表格：分块真表格，正文按行号分页取
    bigTableMount(i)
    if (FIND.pending && FIND.pending.b === i) maybeApplyHit()
    return
  }
  if (V.big.has(i)) {  // 巨型块不渲染正文，只建段容器
    ensureBigResident(i)
    if (FIND.pending && FIND.pending.b === i) FIND.pending = null
    return
  }
  // 跳跃滚动时队列里可能攒着旧窗口的块，窗口已经走了就别建 DOM
  if (!V.initializing && (i < V.windowStart || i > V.windowEnd)) return

  let rec = V.resident.get(i)
  if (!rec) {
    const el = acquireEl()
    el.dataset.i = String(i)
    docEl.appendChild(el)
    rec = { el }
    V.resident.set(i, rec)
    ensureObserver()
    resizeObserver.observe(el)
  }
  const kind = KIND_NAMES[item.flags & 0x0f] || 'none'
  rec.el.className = 'blk blk--' + kind + (i === 0 ? ' blk--first' : '')
  rec.el.style.top = (pos(i) - V.stageBase) + 'px'
  rec.text = item.text
  rec.flags = item.flags
  let html = renderBlockHtml(item.text, item.flags, i)
  // 脚注区挂在最后一个块上：没有独立块号，也不参与索引
  if (i === V.lastVisible && FOOT.html) html += FOOT.html
  rec.el.innerHTML = html
  processNetImages(rec.el)
  if (pendingMeasure) pendingMeasure.push(i)
  else setHeight(i, rec.el.offsetHeight)
  if (V.flashBlock === i) {
    V.flashBlock = -1
    animateBlock(rec.el)
  }
  queueMermaid(rec.el, V.docId)
  if (FIND.pending && FIND.pending.b === i) maybeApplyHit()
  else if (FIND.current && FIND.current.b === i) applyHitMark(rec.el, rec.text, FIND.current.off, false)
}

/* ---------------- 巨型块：分段 + 块内行窗口 ----------------
   单个元素的 CSS 高度被 Blink 按设备像素封顶（本机 175% 缩放下约 1917 万
   CSS px，7350 万 px 的 200 MB 围栏会被截断），所以一个巨型块切成多个段
   元素：每段 10 万行（约 250 万 px），段只负责定位与裁剪，行窗口照样按视口
   取。块内不支持跨窗口选择。 */

const BIG_SEG_LINES = 100000

function segCount(i) {
  const m = V.big.get(i)
  return m ? Math.max(1, Math.ceil(m.lines / BIG_SEG_LINES)) : 0
}

// 第 k 段的顶/高。各段首尾相接，总和严格等于 bigHeight：
//   bigHeight = overhead + 行数 × lineH
// 首段带完整留白与顶部边框（多段时去掉下边框），中间段无边框无留白，
// 末段只有下边框——视觉上仍是一整块。
function segTop(i, k) {
  const g = bigMetrics()
  return k === 0 ? 0 : g.overhead - 1 + k * BIG_SEG_LINES * g.lineH
}

function segHeight(i, k) {
  const g = bigMetrics()
  const m = V.big.get(i)
  const n = segCount(i)
  const s = k * BIG_SEG_LINES
  const e = Math.min(m.lines, s + BIG_SEG_LINES)
  let extra = 0
  if (k === 0) extra = n > 1 ? g.overhead - 1 : g.overhead
  else if (k === n - 1) extra = 1
  return extra + (e - s) * g.lineH
}

function bigHeight(i) {
  const m = V.big.get(i)
  if (!m) return 0
  return bigMetrics().overhead + m.lines * bigMetrics().lineH
}

function makeSegDom() {
  const el = document.createElement('div')
  el.innerHTML = '<div class="big-frame"><pre class="big-pre">'
    + '<span class="mdv-ln" aria-hidden="true"></span><code></code></pre></div>'
  const frame = el.firstElementChild
  const pre = frame.firstElementChild
  return { el, pre, ln: pre.firstElementChild, code: pre.lastElementChild }
}

// 巨型块的行号：窗口每次换行范围都要跟着换，行数按窗口重算、起始号交给 counter-reset。
// 空行元素复用（只增删差量），否则每次滚动都要造几百个节点。
// 列宽按整块行数算，不按窗口算：否则滚过 99999 这种位数边界时整列会宽一格、代码横移。
function syncSegGutter(seg, from, count, totalLines) {
  const g = seg.ln
  if (!g) return
  if (!state.render.lineNumbers) {
    if (seg.lnOn) {
      g.textContent = ''
      seg.pre.classList.remove('has-ln')
      if (seg.lnCls) seg.pre.classList.remove(seg.lnCls)
      seg.lnOn = false
      seg.lnCls = ''
    }
    return
  }
  if (!seg.lnOn) {
    seg.pre.classList.add('has-ln')
    seg.lnOn = true
  }
  const cls = lineGutterWidth(Math.max(totalLines || 0, from + count))
  if (seg.lnCls !== cls) {
    if (seg.lnCls) seg.pre.classList.remove(seg.lnCls)
    seg.pre.classList.add(cls)
    seg.lnCls = cls
  }
  g.style.counterReset = 'mdv-line ' + from
  let cur = g.childElementCount
  while (cur < count) {
    g.appendChild(document.createElement('span'))
    cur++
  }
  while (cur > count) {
    g.removeChild(g.lastChild)
    cur--
  }
}

function ensureBigResident(i) {
  let rec = V.resident.get(i)
  if (rec) {
    if (rec.big) return rec
    // 普通块刚被认定为巨型块：拆掉旧的 DOM，换成段容器
    if (resizeObserver) resizeObserver.unobserve(rec.el)
    if (rec.el.parentNode) rec.el.parentNode.removeChild(rec.el)
    rec.el.innerHTML = ''
    V.pool.push(rec.el)
    rec = { big: { segs: new Map() } }
    V.resident.set(i, rec)
    return rec
  }
  rec = { big: { segs: new Map() } }
  V.resident.set(i, rec)
  return rec
}

function releaseSeg(rec, k) {
  const seg = rec.big.segs.get(k)
  if (!seg) return
  seg.code.textContent = ''
  seg.dropHead = 0
  if (seg.ln) {
    seg.ln.textContent = ''
    seg.pre.classList.remove('has-ln')
    if (seg.lnCls) seg.pre.classList.remove(seg.lnCls)
    seg.lnOn = false
    seg.lnCls = ''
  }
  seg.pre.style.transform = ''
  seg.el.style.top = ''
  seg.el.style.height = ''
  seg.el.className = 'blk'
  if (seg.el.parentNode) seg.el.parentNode.removeChild(seg.el)
  V.bigPool.push(seg)
  rec.big.segs.delete(k)
}

function releaseBig(rec) {
  for (const k of [...rec.big.segs.keys()]) releaseSeg(rec, k)
}

function ensureSeg(i, rec, k) {
  let seg = rec.big.segs.get(k)
  if (!seg) {
    const item = V.bigPool.pop() || makeSegDom()
    seg = {
      el: item.el, pre: item.pre, ln: item.ln, code: item.code,
      from: -1, got: 0, want: null, loading: false, shortAt: -1, dropHead: 0,
      lnOn: false, lnCls: '',
    }
    seg.el.dataset.i = String(i)
    seg.el.dataset.seg = String(k)
    docEl.appendChild(seg.el)
    rec.big.segs.set(k, seg)
  }
  const n = segCount(i)
  seg.el.className = 'blk blk--big'
    + (n > 1 && k === 0 ? ' blk--bighead' : '')
    + (k > 0 ? ' blk--bigseg' : '')
    + (n > 1 && k === n - 1 ? ' blk--biglast' : '')
  seg.el.style.top = (pos(i) + segTop(i, k) - V.stageBase) + 'px'
  seg.el.style.height = segHeight(i, k) + 'px'
  return seg
}

// 只保留视口上下各一屏范围内的段；段的边界是精确的，不需要估算
function layoutBigSegments(i, rec) {
  const m = V.big.get(i)
  if (!m || !rec || !rec.big) return
  const n = segCount(i)
  const blockTop = pos(i)
  const keep = new Set()
  for (let k = 0; k < n; k++) {
    const topDoc = blockTop + segTop(i, k)
    const h = segHeight(i, k)
    if (topDoc + h < V.off - V.vh || topDoc > V.off + V.vh * 2) continue
    keep.add(k)
    ensureSeg(i, rec, k)
  }
  for (const k of [...rec.big.segs.keys()]) {
    if (!keep.has(k)) releaseSeg(rec, k)
  }
}

function updateBigWindows() {
  if (!V.big.size) return
  // 长途跳跃中不取行：每一帧的窗口都是废的，落地后再取一次
  if (Math.abs(V.target - V.off) > V.vh * 1.5) return
  const g = bigMetrics()
  for (const [i, rec] of V.resident) {
    if (!rec.big) continue
    const m = V.big.get(i)
    if (!m) continue
    const blockTop = pos(i)
    for (const [k, seg] of rec.big.segs) {
      const sLine = k * BIG_SEG_LINES
      const eLine = Math.min(m.lines, sLine + BIG_SEG_LINES)
      const topDoc = blockTop + segTop(i, k)
      const visTop = V.off - topDoc

      // 段内行号：首段内容比段顶多出「段顶留白 + 上内边距」
      const base = sLine + (k === 0 ? -g.overhead / g.lineH : 0)
      let from = Math.floor((visTop / g.lineH) + base) - LINE_MARGIN
      from = Math.max(sLine, Math.floor(from / LINE_ALIGN) * LINE_ALIGN)
      let to = Math.ceil(((visTop + V.vh) / g.lineH) + base) + LINE_MARGIN
      to = Math.min(eLine, Math.ceil(to / LINE_ALIGN) * LINE_ALIGN)
      let count = Math.min(LINE_MAX, Math.max(LINE_MIN, to - from))
      count = Math.min(count, eLine - from)
      if (count <= 0) continue

      if (seg.from === from && (seg.got >= count || seg.shortAt === from)) continue
      if (seg.loading) {
        seg.want = { from, count }
        continue
      }
      requestBigLines(i, k, from, count)
    }
  }
}

// 巨型围栏块：首行是 ```lang、末行是收尾围栏，它们不是代码内容，按源码渲染会露出来。
// 这里把围栏行剔掉，首段整体下移一行（视觉上等于围栏那行留白），末段那行同样只在
// 含块末行的窗口里剔。返回的 lineBase 是行号计数器的起点减一：代码行从 1 起，
// 不把围栏算进行号。
const FENCE_RE = /^\s*(?:`{3,}|~{3,})/

function trimFenceLines(text, from, got, m) {
  if (!m || m.kind !== KIND_FENCE) return { text, lineBase: from, dropHead: 0 }
  const lines = text.split('\n')
  let dropHead = 0
  if (from === 0 && lines.length && FENCE_RE.test(lines[0])) {
    lines.shift()
    dropHead = 1
  }
  if (m.lines && from + got >= m.lines && lines.length &&
      FENCE_RE.test(lines[lines.length - 1])) {
    lines.pop()
  }
  return { text: lines.join('\n'), lineBase: from + dropHead - 1, dropHead }
}

/* ---------------- 巨型代码块的后台高亮 ----------------
   行窗口每次只取一两百行，逐窗口喂 hljs 会在窗口切换那帧掉到 25 ms 以上，而且跨窗口的
   块注释、长字符串会断色。改成整块高亮一次、按行切分缓存：窗口取用退化成字符串拼接，
   着色与整块一次性算出时完全一致。超过规模就不试——hljs 是同步的，宁可不上色也不让
   后台任务拖长。 */

const BIG_HL_MAX_LINES = 6000
const BIG_HL_MAX_CHARS = 6 * 1024 * 1024  // 所有块的高亮缓存总预算

function closeTags(stack) {
  let s = ''
  for (let k = stack.length - 1; k >= 0; k--) {
    const tag = stack[k]
    const nameEnd = tag.search(/[\s/>]/)
    s += '</' + (nameEnd > 1 ? tag.slice(1, nameEnd) : 'span') + '>'
  }
  return s
}

// 把 hljs 的整体输出切成「每行一段、行内标签自闭合」的数组：
// 行尾补上未闭合的标签，下一行开头重新打开（开标签原样存着，重开时整条拼回去）。
// 之后任意窗口 slice + join 都能直接当 HTML 用。
// 关键点：hljs 是对整块代码跑的，多行注释、多行字符串的判定发生在切分之前，
// 所以切分只重排标签、不改变「哪些字符属于注释/字符串」，跨行结构不会断色。
function splitHighlightLines(html) {
  const out = []
  const stack = []
  let buf = ''
  let i = 0
  const n = html.length
  while (i < n) {
    const lt = html.indexOf('<', i)
    const nl = html.indexOf('\n', i)
    if (nl >= 0 && (lt < 0 || nl < lt)) {
      buf += html.slice(i, nl)
      out.push(buf + closeTags(stack))
      buf = stack.join('')
      i = nl + 1
      continue
    }
    if (lt < 0) {
      buf += html.slice(i)
      break
    }
    const gt = html.indexOf('>', lt)
    if (gt < 0) {
      buf += html.slice(lt)
      break
    }
    const tag = html.slice(lt, gt + 1)
    if (tag.charCodeAt(1) === 47) stack.pop()
    else if (tag.charCodeAt(tag.length - 2) !== 47) stack.push(tag)
    buf += html.slice(i, gt + 1)
    i = gt + 1
  }
  out.push(buf + closeTags(stack))
  return out
}

// 后台整块高亮：取整块正文 → 去围栏 → hljs → 按行切分，写回 m.hl 并重画该块的段。
function scheduleBigHighlight(i) {
  const m = V.big.get(i)
  if (!m || m.hl || m.hlBusy || !state.render.highlight) return
  if (m.kind !== KIND_FENCE || !m.lines || m.lines > BIG_HL_MAX_LINES) return
  m.hlBusy = true
  const docId = V.docId

  const go = () => {
    const cur = V.big.get(i)
    if (docId !== V.docId || !cur || cur.hl) return
    fetch(BLOCKS_URL + '?from=' + i + '&count=1', { cache: 'no-store' })
      .then((res) => (res.ok ? res.arrayBuffer() : null))
      .then((buf) => {
        if (!buf || docId !== V.docId) return
        const items = parseFrames(buf)
        const item = items.find((x) => x.i === i)
        if (!item) return
        const lines = item.text.split('\n')
        let lang = ''
        if (lines.length && FENCE_RE.test(lines[0])) {
          lang = (lines[0].match(/^\s*(?:`{3,}|~{3,})\s*([^\s`~]+)/) || ['', ''])[1]
          lines.shift()
        }
        if (lines.length && FENCE_RE.test(lines[lines.length - 1])) lines.pop()
        const code = lines.join('\n')
        if (!lang || !hljs.getLanguage(lang) || code.length > HIGHLIGHT_MAX_BYTES) return
        const html = hljs.highlight(code, { language: lang, ignoreIllegals: true }).value
        const cur2 = V.big.get(i)
        if (!cur2) return
        cur2.hl = { lines: splitHighlightLines(html), cost: html.length }
        bigHlBudget()
        rerenderBigSegs(i)
      })
      .catch((err) => {
        if (docId === V.docId) report('big-hl-error', String(err))
      })
  }

  // 滚动中不做：整块高亮是一次几百毫秒的同步任务，别落在滑动的帧上
  const later = () => {
    const m2 = V.big.get(i)
    if (!m2 || docId !== V.docId) return
    if (performance.now() - V.lastScrollAt < 400) {
      setTimeout(later, 300)
      return
    }
    go()
  }
  setTimeout(later, 120)
}

// 高亮缓存按总量回收：超预算就从最早缓存的块开始丢（丢掉的块退回纯文本，不影响正确性）
function bigHlBudget() {
  let total = 0
  for (const m of V.big.values()) if (m.hl) total += m.hl.cost
  if (total <= BIG_HL_MAX_CHARS) return
  for (const [i, m] of V.big) {
    if (total <= BIG_HL_MAX_CHARS) break
    if (!m.hl) continue
    total -= m.hl.cost
    m.hl = null
    m.hlBusy = false
    rerenderBigSegs(i)
  }
}

function rerenderBigSegs(i) {
  const rec = V.resident.get(i)
  if (!rec || !rec.big) return
  for (const seg of rec.big.segs.values()) {
    if (seg.from < 0) continue
    applyBigSeg(seg, i, seg.from, seg.got, seg.plain || '')
  }
}

// 一段窗口的内容：有整块高亮就切缓存，没有就用取回来的源码文本。
// 缓存里的第 0 行是代码第一行，对应源码第 1 行（开围栏被剔掉），所以索引整体减一。
function applyBigSeg(seg, i, from, got, text) {
  const m = V.big.get(i)
  const g = bigMetrics()
  let dropHead = 0
  let lineBase
  let count
  if (m && m.hl) {
    dropHead = from === 0 ? 1 : 0
    const lo = Math.max(0, from - 1)
    const hi = Math.min(m.hl.lines.length, Math.max(lo, from + got - 1))
    const lines = m.hl.lines.slice(lo, hi)
    lineBase = lo
    count = Math.max(1, lines.length)
    seg.code.innerHTML = markWhitespace(lines.join('\n'), state.render.codeWs, false)
  } else {
    const cut = trimFenceLines(text, from, got, m)
    dropHead = cut.dropHead
    lineBase = cut.lineBase
    count = Math.max(1, displayLines(cut.text))
    // 逐段标记要拼 HTML，文本先转义（原来用 textContent，渲染结果一样）
    seg.code.innerHTML = markWhitespace(escapeHtml(cut.text), state.render.codeWs, false)
  }
  syncSegGutter(seg, lineBase, count, m ? m.lines : 0)
  seg.dropHead = dropHead
  // 段是整块复用同一个 <code>（只有 innerHTML 被换掉），缓存里的 span 已脱离
  // 文档，comparePoint 会抛 WrongDocumentError，必须在这里作废。
  delete seg.code.__wsSpans
  const segOffset = from - Math.floor(from / BIG_SEG_LINES) * BIG_SEG_LINES
  seg.pre.style.transform =
    'translateY(' + (segOffset * g.lineH + dropHead * g.lineH).toFixed(1) + 'px)'
  // 段是整块重建的，原来点亮的 span 已成孤儿：选区还在就得重新点亮一次。
  // 只在「仅选中」档下是硬需求（点亮是它唯一的显示来源），其余档多跑一次也无妨：
  // 没有选区时这里第一件事就返回，有 rAF 合并，滚动不会把它堆起来。
  wsScheduleSelection()
}

function requestBigLines(i, k, from, count) {
  const rec = V.resident.get(i)
  if (!rec || !rec.big) return
  const seg = rec.big.segs.get(k)
  if (!seg) return
  seg.loading = true
  const docId = V.docId
  fetch(LINES_URL + '?i=' + i + '&from=' + from + '&count=' + count, { cache: 'no-store' })
    .then((res) => {
      if (!res.ok) throw new Error('HTTP ' + res.status)
      return res.arrayBuffer()
    })
    .then((buf) => {
      if (docId !== V.docId || buf.byteLength < 20) return
      const dv = new DataView(buf)
      const idx = dv.getUint32(0, true)
      const lineFrom = dv.getUint32(4, true)
      const got = dv.getUint32(8, true)
      const total = dv.getUint32(12, true)
      const len = dv.getUint32(16, true)
      if (idx !== i || lineFrom !== from || 20 + len > buf.byteLength) return
      const cur = V.resident.get(i)
      const curSeg = cur && cur.big && cur.big.segs.get(k)
      if (!curSeg) return
      const text = decoder.decode(new Uint8Array(buf, 20, len))
      curSeg.from = from
      curSeg.got = got
      curSeg.plain = text
      applyBigSeg(curSeg, i, from, got, text)
      if (got < count) curSeg.shortAt = from
      if (total > 0) updateBigLines(i, total)
      scheduleBigHighlight(i)
      if (FIND.bigHit && FIND.bigHit.b === i) applyBigHitMark()
    })
    .catch((err) => {
      if (docId === V.docId) report('lines-error', String(err))
    })
    .finally(() => {
      const cur = V.resident.get(i)
      const curSeg = cur && cur.big && cur.big.segs.get(k)
      if (!curSeg) return
      curSeg.loading = false
      const want = curSeg.want
      curSeg.want = null
      if (want && docId === V.docId) requestBigLines(i, k, want.from, want.count)
    })
}

// 行数先按前缀估算摆位，全文索引到达后再校正；段的几何全部重建
function updateBigLines(i, lines) {
  const m = V.big.get(i)
  if (!m || m.lines === lines) return
  V.big.set(i, { ...m, lines })
  const rec = V.resident.get(i)
  if (rec && rec.big) releaseBig(rec)
  rebuildFenwick()
  schedulePlace()
}

// 巨型块表：`块号:行数` 表示走行窗口纯文本，`块号:行数:1` 表示表格。
// 表格是否按真表格渲染由这里的规模上限决定：过线的表格改用分块真表格，
// 再大的仍回落行窗口纯文本。
// 块类型（与 C++ 的 doc::Kind 同值）。巨型块里只有围栏块要按代码渲染。
const KIND_FENCE = 8

function parseBigTable(text) {
  // 别叫 big：那会遮住模块级的段几何（big.lineH 等），以后在这里顺手引用就踩空
  const bigs = new Map()
  const tables = new Map()
  if (!text) return { big: bigs, tables }
  for (const part of text.split(',')) {
    const f = part.split(':')
    if (f.length < 2) continue
    const i = parseInt(f[0], 10)
    const lines = parseInt(f[1], 10)
    if (!Number.isInteger(i) || i < 0 || !Number.isInteger(lines) || lines <= 0) continue
    const kind = f.length > 3 ? parseInt(f[3], 10) : -1
    // lines 含表头行与分隔行，数据行要减 2
    if (f[2] === '1' && lines - 2 <= BT_MAX_LINES) tables.set(i, { lines })
    else bigs.set(i, { lines, kind })
  }
  return { big: bigs, tables }
}

function applyBigTable(text) {
  const parsed = parseBigTable(text)
  V.bigPending = parsed
  if (!V.active) return
  V.bigPending = null
  V.big = parsed.big
  V.bigTable = parsed.tables
  bigMetrics()

  for (const [i, rec] of [...V.resident]) {
    if (parsed.tables.has(i)) {
      bigTableMount(i)
    } else if (parsed.big.has(i)) {
      ensureBigResident(i)
    } else if (rec.table) {
      bigTableRelease(rec)
      V.resident.delete(i)
    } else if (rec.big) {
      releaseBig(rec)
      V.resident.delete(i)
    }
  }
  rebuildFenwick()
  schedulePlace()
  maybeApplyHit()
}

function recycleOutside(a, b) {
  for (const [i, rec] of V.resident) {
    if (i >= a && i <= b) continue
    if (V.pinned.has(i)) continue
    if (rec.table) {
      bigTableRelease(rec)
    } else if (rec.big) {
      releaseBig(rec)
    } else {
      if (resizeObserver) resizeObserver.unobserve(rec.el)
      rec.el.style.top = ''
      if (rec.el.parentNode) rec.el.parentNode.removeChild(rec.el)
      V.pool.push(rec.el)
    }
    V.resident.delete(i)
  }
}

/* ---------------- 块请求 ---------------- */

function parseFrames(buf) {
  const dv = new DataView(buf)
  const out = []
  let o = 0
  while (o + 9 <= buf.byteLength) {
    const len = dv.getUint32(o, true); o += 4
    const i = dv.getUint32(o, true); o += 4
    const flags = dv.getUint8(o); o += 1
    if (o + len > buf.byteLength) break
    out.push({ i, flags, text: decoder.decode(new Uint8Array(buf, o, len)) })
    o += len
  }
  return out
}

function requestBlocks(from, count, mode) {
  const key = from + ':' + count + ':' + mode
  if (V.fetching.has(key)) return
  V.fetching.add(key)
  const docId = V.docId
  for (let i = from; i < from + count; i++) V.pending.add(i)

  fetch(BLOCKS_URL + '?from=' + from + '&count=' + count, { cache: 'no-store' })
    .then((res) => {
      if (!res.ok) throw new Error('HTTP ' + res.status)
      return res.arrayBuffer()
    })
    .then((buf) => {
      const items = parseFrames(buf)
      if (docId !== V.docId) return
      const got = new Set()
      for (const it of items) {
        it.docId = docId
        got.add(it.i)
      }
      // 没返回的块（超出索引）解除等待；返回的块保留到真正渲染，避免重复请求
      for (let i = from; i < from + count; i++) {
        if (!got.has(i)) V.pending.delete(i)
      }
      if (mode === 'init') {
        for (const it of items) {
          renderItem(it)
          V.pending.delete(it.i)
        }
        finishInit()
        return
      }
      for (const it of items) {
        if (it.i >= V.windowStart && it.i <= V.windowEnd) {
          V.queue.push(it)
        } else {
          // 滚出窗口的块不入队，也就不会有人渲染它：这里必须同时解除等待，
          // 否则它此后既不请求也不渲染，滚回来就是永久空白。
          V.pending.delete(it.i)
        }
      }
      schedulePump()
      schedulePlace()
    })
    .catch((err) => {
      for (let i = from; i < from + count; i++) V.pending.delete(i)
      if (docId === V.docId) report('blocks-error', String(err))
    })
    .finally(() => {
      V.fetching.delete(key)
      if (docId === V.docId) schedulePlace()
    })
}

function ensureRange(a, b) {
  if (!V.active || V.initializing) return
  // 还在长途跳跃中：中间窗口不取块，等落点稳定再取，否则请求与队列全是废的
  if (Math.abs(V.target - V.off) > V.vh * 1.5) return
  const runs = []
  let start = -1
  for (let i = a; i <= b; i++) {
    const need = !V.resident.has(i) && !V.pending.has(i) && !V.hidden[i] && !V.big.has(i)
    if (need && start < 0) start = i
    if (!need && start >= 0) {
      runs.push([start, i - 1])
      start = -1
    }
  }
  if (start >= 0) runs.push([start, b])

  for (const [s, e] of runs) {
    let from = s
    while (from <= e) {
      if (V.fetching.size >= MAX_INFLIGHT) return
      const count = Math.min(MAX_FETCH_BLOCKS, e - from + 1)
      requestBlocks(from, count, 'normal')
      from += count
    }
  }
}

function schedulePump() {
  if (V.pumpRaf || !V.active) return
  V.pumpRaf = requestAnimationFrame(pump)
}

function pump() {
  V.pumpRaf = 0
  if (!V.active) return
  // 视口附近的块先渲染：跳转时不必等窗口里离得远的块排完
  if (V.queue.length > 12) {
    const first = offsetToBlock(V.off)
    const last = offsetToBlock(Math.min(Math.max(0, V.totalH - 1), V.off + V.vh))
    const dist = (i) => (i < first ? first - i : i > last ? i - last : 0)
    V.queue.sort((a, b) => dist(a.i) - dist(b.i))
  }
  // 按时间片而不是块数截止：单个大块（数学密集）可能就要几十毫秒
  const t0 = performance.now()
  const touched = []
  let n = 0
  while (V.queue.length && n < 10) {
    const item = V.queue.shift()
    renderItem(item, touched)
    V.pending.delete(item.i)
    n++
    if (performance.now() - t0 >= 7) break
  }
  // 先写完这一批 DOM 再统一量高：交替「innerHTML → offsetHeight」会让每块各触发一次同步布局
  for (const i of touched) {
    const rec = V.resident.get(i)
    if (rec && !rec.big && rec.el) setHeight(i, rec.el.offsetHeight)
  }
  if (V.queue.length) schedulePump()
  schedulePlace()
}

/* ---------------- 视口 ---------------- */

function schedulePlace() {
  if (V.placeRaf || !V.active) return
  V.placeRaf = requestAnimationFrame(() => {
    V.placeRaf = 0
    if (V.active) place()
  })
}

function place() {
  if (!V.active || !docEl) return
  V.vh = window.innerHeight
  const scrollable = Math.max(0, V.totalH - V.vh)
  V.off = Math.min(Math.max(V.off, 0), scrollable)
  V.target = Math.min(Math.max(V.target, 0), scrollable)

  const first = offsetToBlock(V.off)
  const last = offsetToBlock(Math.min(Math.max(0, V.totalH - 1), V.off + V.vh))
  const a = Math.max(0, first - OVERSCAN)
  const b = Math.min(V.blockCount - 1, last + OVERSCAN)

  // 舞台基点到视口的距离要有界：巨型块内部滚动时块顶可能离视口几千万 px，
  // 而元素的 top 也会被 Blink 的布局上限截断，所以让 stageBase 跟着视口走，
  // 只在跨过 4 屏时才重定一次基（重定会刷新所有常驻块的 top）。
  const anchor = Math.max(pos(a), V.off - V.vh * 2)
  if (a !== V.windowStart || b !== V.windowEnd || V.pinsDirty) {
    V.windowStart = a
    V.windowEnd = b
    recycleOutside(a, b)
    V.pinsDirty = false
    // 跳跃滚动时队列里会攒下大量已经过时的窗口块，别让它们挡住当前窗口。
    // 丢掉的条目同时解除 pending：它们既不入队也不会被渲染，留着就是永久空白。
    if (V.queue.length) {
      const kept = []
      for (const it of V.queue) {
        if (it.i >= a && it.i <= b) kept.push(it)
        else V.pending.delete(it.i)
      }
      V.queue = kept
    }
  }
  // 基点只在离开 4 屏时重定一次；窗口本身移动不改变块间相对位置，不需要刷全部 top
  if (Math.abs(anchor - V.stageBase) > V.vh * 4) {
    V.stageBase = anchor
    V.topsDirty = true
    V.topsFrom = 0
  }

  docEl.style.transform = 'translate3d(0,' + (-(V.off - V.stageBase)).toFixed(2) + 'px,0)'
  ensureRange(a, b)
  if (V.big.size) {
    for (let i = a; i <= b; i++) {
      if (!V.big.has(i)) continue
      const rec = V.resident.get(i) || ensureBigResident(i)
      layoutBigSegments(i, rec)
    }
  }
  if (V.topsDirty) {
    const from = V.topsFrom
    for (const [i, rec] of V.resident) {
      if (i >= from && rec.el) rec.el.style.top = (pos(i) - V.stageBase) + 'px'
    }
    V.topsDirty = false
  }
  updateBigWindows()
  bigTableEnsureVisible()
  applyBigHitMark()
  updateScrollbar()
  V.anchorId = first
  V.anchorDy = V.off - pos(first)
  V.anchorSet = true
}

function updateScrollbar() {
  if (!sbEl) return
  const scrollable = V.totalH - V.vh
  if (scrollable <= 0) {
    sbEl.hidden = true
    return
  }
  sbEl.hidden = false
  // 轨道高度只在窗口尺寸变化后重读：每帧读 clientHeight 会强制同步布局
  if (V.trackH <= 0) V.trackH = sbEl.clientHeight
  const trackH = V.trackH
  if (trackH <= 0) return
  const thumbH = Math.max(28, Math.min(trackH, (V.vh / V.totalH) * trackH))
  const maxTop = Math.max(0, trackH - thumbH)
  const top = scrollable > 0 ? (V.off / scrollable) * maxTop : 0
  const h = thumbH.toFixed(1) + 'px'
  const tr = 'translateY(' + top.toFixed(1) + 'px)'
  if (h !== V.thumbH) { thumbEl.style.height = h; V.thumbH = h }
  if (tr !== V.thumbTop) { thumbEl.style.transform = tr; V.thumbTop = tr }
}

/* ---------------- 首屏与索引 ---------------- */

function finishInit() {
  let sum = 0
  let n = 0
  for (const [i] of V.resident) {
    if (!V.hidden[i] && V.measured[i] && V.heights[i] > 0) {
      sum += V.heights[i]
      n++
    }
  }
  V.defaultH = n ? Math.max(24, sum / n) : 120
  V.initializing = false
  rebuildFenwick()
  V.vh = window.innerHeight
  V.windowStart = -1
  V.windowEnd = -1
  V.stageBase = 0
  V.anchorSet = false
  place()
  V.anchorSet = true

  const hs = []
  let nHidden = 0
  let nMeasured = 0
  for (let k = 0; k < V.blockCount; k++) {
    if (V.hidden[k]) nHidden++
    if (V.measured[k]) nMeasured++
    if (k < 8) hs.push(Math.round(V.heights[k]))
  }
  const ms = performance.now() - V.indexAt
  const metrics = 'h=' + hs.join(',') + ' d=' + Math.round(V.defaultH)
    + ' hm=' + nMeasured + '/' + nHidden + ' total=' + Math.round(V.totalH) + ' vh=' + V.vh
    + ' sb=' + (sbEl && !sbEl.hidden ? 1 : 0)
    + (V.big.size ? ' bh=' + big.lineH.toFixed(2) + '/' + big.overhead.toFixed(1) : '')
    + ' vp=' + window.innerWidth + 'x' + window.innerHeight
    + ' dpr=' + (window.devicePixelRatio || 1).toFixed(2)
  report('rendered', ms.toFixed(1) + '|' + V.prefixCount + '|' + V.blockCount + '|' + metrics)
  schedulePlace()

  if (V.pendingFullCount) {
    const full = V.pendingFullCount
    const last = V.pendingLastVisible
    V.pendingFullCount = 0
    V.pendingLastVisible = 0
    applyIndexFull(full, last)
  }
}

function resetVirtual() {
  closeZoomMask()  // 换文档：遮罩与 imgctx 待办一起作废
  resetFind()
  OUT.seq++
  OUT.items = []
  OUT.total = 0
  OUT.truncated = false
  OUT.loaded = false
  OUT.loading = false
  if (outlineEl) outlineEl.hidden = true

  V.docId++
  V.active = false
  syncTools()
  // 选区点亮的挂起 rAF 与 lit 集合属于上一份文档，换文档时一并作废。
  if (WS.raf) cancelAnimationFrame(WS.raf)
  WS.raf = 0
  wsClearLit()
  if (V.pumpRaf) cancelAnimationFrame(V.pumpRaf)
  if (V.placeRaf) cancelAnimationFrame(V.placeRaf)
  if (V.raf) cancelAnimationFrame(V.raf)
  V.pumpRaf = 0
  V.placeRaf = 0
  V.raf = 0
  V.queue.length = 0
  V.pending.clear()
  V.fetching.clear()
  htmlCacheClear()
  if (resizeObserver) resizeObserver.disconnect()
  for (const rec of V.resident.values()) {
    if (rec.table) {
      bigTableRelease(rec)
    } else if (rec.big) {
      releaseBig(rec)
    } else if (rec.el.parentNode) {
      rec.el.parentNode.removeChild(rec.el)
    }
  }
  V.resident.clear()
  V.pool = []
  V.bigPool = []
  if (docEl) {
    docEl.textContent = ''
    docEl.style.transform = ''
  }
  if (sbEl) sbEl.hidden = true
  V.initializing = false
  V.anchorSet = false
  V.big = new Map()
  V.bigTable = new Map()
  V.bigPending = null
  V.lastVisible = 0
  REF.references = null
  FOOT.defs = []
  FOOT.nums.clear()
  FOOT.html = ''
  SLUG.bySlug.clear()
  SLUG.byBlock.clear()
  V.pinned.clear()
  V.pinsDirty = false
  V.flashBlock = -1  // 换文档后同一块号不该再闪一次
  SEL.all = false
  onSelUp()
  cleanupPrint()
  dropNetPrompts()
}

function applyIndex(base, prefix, est, fileSize, enc, lastVisible, complete, docKey, session) {
  resetVirtual()
  bindDom()
  document.body.classList.add('virtual')
  document.body.classList.remove('dev')

  state.docBase = base || state.docBase
  state.generation = ++renderGeneration
  // 网络图答复按这篇文档记：必须在下面渲染块之前切好，processNetImages 要查它
  netOpenDoc(docKey, session)

  V.active = true
  V.docId++
  V.prefixCount = prefix
  V.estTotal = est
  V.fullIndex = !!complete
  V.lastVisible = lastVisible || 0
  V.fileSize = fileSize
  V.enc = enc
  V.blockCount = Math.max(1, est || prefix || 1)
  V.heights = new Float32Array(V.blockCount)
  V.measured = new Uint8Array(V.blockCount)
  V.hidden = new Uint8Array(V.blockCount)
  V.fenwick = new Float64Array(V.blockCount + 1)
  V.defaultH = 120
  V.totalH = 0
  V.off = 0
  V.target = 0
  V.anchorSet = false
  V.indexAt = performance.now()
  V.initializing = true
  V.pendingFullCount = 0
  if (V.bigPending) {
    V.big = V.bigPending.big
    V.bigTable = V.bigPending.tables
    V.bigPending = null
    bigMetrics()
  }

  syncTools()
  requestBlocks(0, Math.min(30, Math.max(1, prefix || 1)), 'init')
  loadDefs()
  loadOutline()
}

function applyIndexFull(count, lastVisible) {
  if (!V.active || !count) return
  V.fullIndex = true
  // 前缀只扫了 8 MB，后面可能还有引用/脚注定义；全文索引到了再取一次
  loadDefs()
  loadOutline()
  // 搜索会扫全文，但命中块号要用全文索引才准；索引到了重跑一遍当前查询
  if (FIND.open && FIND.query) runSearch()
  const lastChanged = lastVisible !== undefined && lastVisible !== V.lastVisible
  if (lastVisible !== undefined) V.lastVisible = lastVisible
  if (V.initializing) {
    // 首屏还没收尾：等 finishInit 之后统一换数组，避免重入
    V.pendingFullCount = count
    V.pendingLastVisible = V.lastVisible
    return
  }
  const oldCount = V.blockCount
  if (count === oldCount) {
    if (lastChanged) rerenderResidents()
    return
  }

  if (count < oldCount) {
    // 估算偏大（常见）：Fenwick 多出来的尾巴不影响前 count 块的前缀和，
    // 只改计数与总高，避免 400 万项重建卡一帧。
    V.blockCount = count
    if (V.lastVisible > count - 1) V.lastVisible = count - 1
    V.totalH = topPad() + fwSum(count) + bottomPad()
    const scrollable = Math.max(0, V.totalH - V.vh)
    V.off = Math.min(V.off, scrollable)
    V.target = Math.min(V.target, scrollable)
    refetchTruncatedPrefix()
    if (lastChanged) rerenderResidents()
    schedulePlace()
    return
  }

  const heights = new Float32Array(count)
  const measured = new Uint8Array(count)
  const hidden = new Uint8Array(count)
  const copy = Math.min(oldCount, count)
  heights.set(V.heights.subarray(0, copy))
  measured.set(V.measured.subarray(0, copy))
  hidden.set(V.hidden.subarray(0, copy))
  V.heights = heights
  V.measured = measured
  V.hidden = hidden
  V.blockCount = count
  V.fenwick = new Float64Array(count + 1)
  rebuildFenwick()

  refetchTruncatedPrefix()
  if (lastChanged) rerenderResidents()
  schedulePlace()
}

// 前缀扫描的最后一个块可能被 8MB 边界截断，常驻时重取
function refetchTruncatedPrefix() {
  const last = V.prefixCount - 1
  if (last < 0 || !V.resident.has(last)) return
  const rec = V.resident.get(last)
  if (!rec.el) return
  if (resizeObserver) resizeObserver.unobserve(rec.el)
  if (rec.el.parentNode) rec.el.parentNode.removeChild(rec.el)
  V.pool.push(rec.el)
  V.resident.delete(last)
}

/* ---------------- 输入 ---------------- */

function kick() {
  if (V.raf) return
  V.lastTick = 0
  V.raf = requestAnimationFrame(tick)
}

function tick(t) {
  V.raf = 0
  if (!V.active) return
  if (!V.lastTick) V.lastTick = t
  const dt = Math.min(50, Math.max(1, t - V.lastTick))
  V.lastTick = t
  if (V.off !== V.target) {
    const k = 1 - Math.exp(-dt / 80)
    V.off += (V.target - V.off) * k
    if (Math.abs(V.target - V.off) < 0.5) V.off = V.target
  }
  place()
  if (V.off !== V.target) V.raf = requestAnimationFrame(tick)
}

function horizontalAncestor(target) {
  for (let el = target; el && el !== document.body; el = el.parentElement) {
    const cs = getComputedStyle(el)
    if ((cs.overflowX === 'auto' || cs.overflowX === 'scroll') && el.scrollWidth > el.clientWidth) {
      return el
    }
  }
  return null
}

function onWheel(e) {
  if (!V.active) return
  // 搜索条 / 大纲里的滚轮交给它们自己（大纲列表要能滚）
  if (e.target && e.target.closest && e.target.closest('.mdv-ui')) return
  if (e.shiftKey && horizontalAncestor(e.target)) return
  e.preventDefault()
  const unit = e.deltaMode === 1 ? emPx(1.65) : e.deltaMode === 2 ? V.vh : 1
  const delta = e.deltaY * unit
  const now = performance.now()
  const trackpad = Math.abs(delta) < 40 && now - V.lastWheelAt < 60
  V.lastWheelAt = now
  V.lastScrollAt = now
  const scrollable = Math.max(0, V.totalH - V.vh)
  V.target = Math.min(Math.max(V.target + delta, 0), scrollable)
  if (trackpad) {
    V.off = V.target
    place()
    return
  }
  kick()
}

function onKeyDown(e) {
  if (!V.active) return
  if (e.ctrlKey && !e.altKey && !e.metaKey && (e.key === 'a' || e.key === 'A')) {
    if (e.target && /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName)) return
    e.preventDefault()
    selectAllText()
    return
  }
  if (e.ctrlKey && !e.altKey && !e.metaKey && (e.key === 'c' || e.key === 'C')) {
    if (e.target && /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName)) return
    if (!SEL.all) return  // 普通选区交给浏览器原生复制
    e.preventDefault()
    copyRendered()
    return
  }
  if (e.ctrlKey && !e.altKey && !e.metaKey && (e.key === 'f' || e.key === 'F')) {
    if (IMGZ.open) {  // 搜索条会被遮罩盖住，干脆不开
      e.preventDefault()
      return
    }
    e.preventDefault()
    openFind()
    return
  }
  if (e.key === 'Escape') {
    if (IMGZ.open) {  // 遮罩最先消费 Esc，其次才是搜索条与大纲抽屉
      e.preventDefault()
      closeZoomMask()
      return
    }
    if (FIND.open) {
      e.preventDefault()
      closeFind()
      return
    }
    if (outlineEl && !outlineEl.hidden) {
      e.preventDefault()
      closeOutline()
      return
    }
  }
  // 输入框里的方向键/空格属于输入框，不能拿去滚文档
  if (e.target && /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName)) return
  if (e.ctrlKey || e.altKey || e.metaKey) return
  const page = V.vh * 0.9
  const scrollable = Math.max(0, V.totalH - V.vh)
  let delta = 0
  switch (e.key) {
    case 'PageDown': delta = page; break
    case 'PageUp': delta = -page; break
    case 'ArrowDown': delta = 40; break
    case 'ArrowUp': delta = -40; break
    case ' ':
      if (e.target && /^(INPUT|TEXTAREA)$/.test(e.target.tagName)) return
      delta = e.shiftKey ? -page : page
      break
    case 'Home':
      V.off = V.target = 0
      place()
      e.preventDefault()
      return
    case 'End':
      V.off = V.target = scrollable
      place()
      e.preventDefault()
      return
    default:
      return
  }
  e.preventDefault()
  V.target = Math.min(Math.max(V.target + delta, 0), scrollable)
  kick()
}

function onTrackDown(e) {
  if (!V.active || e.target === thumbEl) return
  const rect = sbEl.getBoundingClientRect()
  const thumbH = thumbEl.offsetHeight
  const maxTop = Math.max(1, rect.height - thumbH)
  const ratio = Math.min(1, Math.max(0, (e.clientY - rect.top - thumbH / 2) / maxTop))
  const scrollable = Math.max(0, V.totalH - V.vh)
  V.lastScrollAt = performance.now()
  V.off = V.target = ratio * scrollable
  place()
}

function onThumbDown(e) {
  if (!V.active) return
  e.preventDefault()
  try { thumbEl.setPointerCapture(e.pointerId) } catch { /* 忽略 */ }
  V.drag = { y: e.clientY, off: V.off }
}

function onThumbMove(e) {
  if (!V.drag) return
  const trackH = sbEl.clientHeight
  const thumbH = thumbEl.offsetHeight
  const maxTop = Math.max(1, trackH - thumbH)
  const scrollable = Math.max(0, V.totalH - V.vh)
  const dy = e.clientY - V.drag.y
  V.lastScrollAt = performance.now()
  V.off = V.target = Math.min(Math.max(V.drag.off + (dy / maxTop) * scrollable, 0), scrollable)
  place()
}

function onThumbUp(e) {
  if (!V.drag) return
  V.drag = null
  try { thumbEl.releasePointerCapture(e.pointerId) } catch { /* 忽略 */ }
}

/* ---------------- 跨块选区：钉块 + 拖边自动滚动 ----------------
   块会随视口换出，选区一旦跨到被换出的块就会塌陷。选区覆盖的常驻块钉住不换，
   拖到视口边缘时按帧滚动并用 caretRangeFromPoint 把选区延伸过去。 */

const SEL = {
  down: false, x: 0, y: 0, raf: 0, warned: false, all: false, allLo: 0, allHi: 0,
  reported: false,   // 上一次上报给宿主的「有没有选区」
}

function blockIndexOfNode(node) {
  if (!node) return null
  const el = node.nodeType === 1 ? node : node.parentElement
  const blk = el && el.closest ? el.closest('.blk[data-i]') : null
  if (!blk) return null
  const i = parseInt(blk.dataset.i, 10)
  return Number.isInteger(i) ? i : null
}

function refreshPins() {
  if (!V.active) {
    V.pinned.clear()
    return
  }
  const sel = window.getSelection()
  if (!sel || !sel.rangeCount) {
    if (V.pinned.size) {
      V.pinned.clear()
      V.pinsDirty = true
      schedulePlace()
    }
    return
  }
  const range = sel.getRangeAt(0)
  if (range.collapsed) {
    if (V.pinned.size) {
      V.pinned.clear()
      V.pinsDirty = true
      schedulePlace()
    }
    return
  }
  const a = blockIndexOfNode(range.startContainer)
  const b = blockIndexOfNode(range.endContainer)
  if (a === null || b === null) return
  // 键盘改了选区（端点不再是 Ctrl+A 留下的那两块）就不再算全选
  if (SEL.all && (Math.min(a, b) !== SEL.allLo || Math.max(a, b) !== SEL.allHi)) SEL.all = false

  const lo = Math.min(a, b)
  const hi = Math.max(a, b)
  const next = new Set()
  let bytes = 0
  let clipped = false
  for (let i = lo; i <= hi; i++) {
    const rec = V.resident.get(i)
    if (!rec || rec.big || !rec.el) continue
    if (next.size >= PIN_MAX_BLOCKS || bytes >= PIN_MAX_BYTES) {
      clipped = true
      break
    }
    next.add(i)
    bytes += rec.text ? rec.text.length * 2 + 64 : 4096
  }
  if (clipped && !SEL.warned) {
    SEL.warned = true
    postRaw('status|选区过大，请分段复制')
  }
  let same = next.size === V.pinned.size
  if (same) {
    for (const i of next) {
      if (!V.pinned.has(i)) { same = false; break }
    }
  }
  if (!same) {
    V.pinned = next
    V.pinsDirty = true
    schedulePlace()
  }
}

// 指针还在边缘时持续滚动，并把选区焦点延伸到指针下的字符。
function selEdgeTick() {
  SEL.raf = 0
  if (!SEL.down || !V.active) return
  const y = SEL.y
  const overTop = SEL_EDGE - y
  const overBottom = y - (window.innerHeight - SEL_EDGE)
  let dy = 0
  if (overTop > 0) dy = -Math.min(overTop, SEL_EDGE) * 0.45
  else if (overBottom > 0) dy = Math.min(overBottom, SEL_EDGE) * 0.45
  if (!dy) return

  // 先续上下一帧，再干活：下面任何一条 return 都不会把滚动链打断
  // （此前选区还是 collapsed 就直接 return，链断掉，指针不动就不再滚）。
  SEL.raf = requestAnimationFrame(selEdgeTick)

  const scrollable = Math.max(0, V.totalH - V.vh)
  const before = V.target
  V.target = Math.min(Math.max(V.target + dy, 0), scrollable)
  V.off = V.target
  if (V.target === before) return
  place()

  const sel = window.getSelection()
  if (!sel || !sel.rangeCount) return
  if (document.caretRangeFromPoint) {
    const r = document.caretRangeFromPoint(SEL.x, Math.min(Math.max(y, 1), window.innerHeight - 1))
    if (r && docEl && docEl.contains(r.startContainer)) {
      try { sel.extend(r.startContainer, r.startOffset) } catch { /* 忽略越界 */ }
    }
  }
}

function armSelScroll() {
  if (!SEL.down || SEL.raf || !V.active) return
  if (SEL.y > SEL_EDGE && SEL.y < window.innerHeight - SEL_EDGE) return
  SEL.raf = requestAnimationFrame(selEdgeTick)
}

function onSelDown(e) {
  if (!V.active || e.button !== 0) return
  if (e.target && e.target.closest && e.target.closest('.mdv-ui')) return
  SEL.all = false
  SEL.down = true
  SEL.x = e.clientX
  SEL.y = e.clientY
}

function onSelMove(e) {
  if (!SEL.down) return
  SEL.x = e.clientX
  SEL.y = e.clientY
  armSelScroll()
}

function onSelUp() {
  SEL.down = false
  SEL.warned = false
  if (SEL.raf) cancelAnimationFrame(SEL.raf)
  SEL.raf = 0
}

// Ctrl+A：全文不在 DOM 里，视觉上只能选中已载入的块；同时记下“全选”，
// 让接下来的 Ctrl+C 走宿主复制完整全文。
function selectAllText() {
  if (!V.active) return
  let lo = -1
  let hi = -1
  for (const [i, rec] of V.resident) {
    if (!rec.el) continue
    if (lo < 0 || i < lo) lo = i
    if (i > hi) hi = i
  }
  if (lo >= 0) {
    const first = V.resident.get(lo).el
    const last = V.resident.get(hi).el
    const range = document.createRange()
    range.setStart(first, 0)
    range.setEnd(last, last.childNodes.length)
    const sel = window.getSelection()
    sel.removeAllRanges()
    sel.addRange(range)
  }
  SEL.all = lo >= 0
  SEL.allLo = lo
  SEL.allHi = hi
}

// 渲染后的纯文本：用 markdown-it 的 token 取，不生成整篇 DOM。
// 公式走 KaTeX 的 MathML 再取文字；Mermaid 图不复制源码。块之间空行，
// 表格行换行/单元格制表符，<br> 换行，图片跳过，脚注编号保留。
function htmlPlain(raw) {
  return String(raw || '')
    .replace(/<(script|style)\b[\s\S]*?<\/\1>/gi, '')
    .replace(/<br\s*\/?>/gi, '\n')
    .replace(/<[^>]*>/g, '')
}

function decodeEntities(text) {
  return String(text)
    .replace(/&#x([0-9a-f]+);/gi, (m, h) => {
      try { return String.fromCodePoint(parseInt(h, 16)) } catch { return '' }
    })
    .replace(/&#(\d+);/g, (m, d) => {
      try { return String.fromCodePoint(parseInt(d, 10)) } catch { return '' }
    })
    .replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&')
    .replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&nbsp;/g, ' ')
}

// 只取 MathML 的可见符号；annotation 里是 LaTeX 源码，不能进剪贴板。
function mathText(tex) {
  try {
    const html = katexEngine.renderToString(tex, { ...KATEX_OPTIONS, output: 'mathml' })
    const plain = html
      .replace(/<annotation[\s\S]*?<\/annotation>/gi, '')
      .replace(/<[^>]*>/g, '')
    return decodeEntities(plain).trim()
  } catch {
    return ''
  }
}

function inlineText(children) {
  let s = ''
  for (const t of children || []) {
    if (t.type === 'text' || t.type === 'code_inline') s += t.content
    else if (t.type === 'softbreak') s += ' '
    else if (t.type === 'hardbreak') s += '\n'
    else if (t.type === 'html_inline') s += htmlPlain(t.content)
    else if (t.type === 'image') continue
    else if (t.type === 'math_inline' || t.type === 'math_inline_block' ||
             t.type === 'math_block') s += mathText(t.content)
    else if (t.children) s += inlineText(t.children)
  }
  return s
}

function blockText(src) {
  let tokens
  try { tokens = md.parse(src, blockEnv()) } catch { return src }
  let out = ''
  let cells = 0
  const line = () => { if (out && !out.endsWith('\n')) out += '\n' }
  for (const t of tokens) {
    switch (t.type) {
      case 'inline':
        out += inlineText(t.children)
        break
      case 'fence':
        // Mermaid 渲染成图以后源码不该出现在剪贴板里；关掉渲染时才按代码块复制
        if (state.render.mermaid && /^mermaid\b/i.test((t.info || '').trim())) break
        out += t.content
        line()
        break
      case 'code_block':
        out += t.content
        line()
        break
      case 'math_block':
        out += mathText(t.content)
        line()
        break
      case 'html_block':
        out += htmlPlain(t.content)
        line()
        break
      case 'tr_open': cells = 0; line(); break
      case 'tr_close': line(); break
      case 'th_open':
      case 'td_open':
        if (cells++) out += '\t'
        break
      case 'paragraph_close':
      case 'heading_close':
      case 'blockquote_close':
      case 'list_item_close':
        line()
        break
      case 'hr':
        line()
        break
      default:
        break
    }
  }
  return out.trim()
}

// 按块提取文本；索引没覆盖全文或超过字符上限时带截断原因。
async function buildRenderedCopy() {
  const docId = V.docId
  const total = V.blockCount
  const parts = []
  let chars = 0
  let reason = 0
  for (let from = 0; from < total; from += COPY_BATCH) {
    if (docId !== V.docId) throw new Error('文档已切换')
    const count = Math.min(COPY_BATCH, total - from)
    const res = await fetch(BLOCKS_URL + '?from=' + from + '&count=' + count, { cache: 'no-store' })
    if (!res.ok) throw new Error('HTTP ' + res.status)
    const items = parseFrames(await res.arrayBuffer())
    for (const it of items) {
      if (it.flags & 0x20) continue
      if (it.flags & 0x80) reason = 1
      if (chars > 0 && chars + it.text.length > COPY_MAX_CHARS) {
        reason = 1
        break
      }
      const piece = blockText(it.text)
      if (piece) parts.push(piece)
      chars += it.text.length
    }
    if (reason) break
    if (items.length < count) {
      reason = 2
      break
    }
  }
  if (!reason && !V.fullIndex) reason = 2

  let text = parts.join('\n\n')
  if (FOOT.defs.length) {
    const notes = []
    for (const d of FOOT.defs) notes.push('[' + d.num + '] ' + blockText(d.body))
    text += '\n\n' + notes.join('\n\n')
  }
  return { text, reason }
}

async function copyRendered() {
  if (!V.active || COPY.busy) return
  COPY.busy = true
  postRaw('status|正在准备复制…')
  const t0 = performance.now()
  try {
    const result = await buildRenderedCopy()
    if (!V.active) return
    report('copy-text', result.text.length + '|' + result.reason + '|'
      + (performance.now() - t0).toFixed(0))
    postRaw('copyset|' + result.reason + '|' + result.text)
  } catch (err) {
    postRaw('status|复制失败：' + String((err && err.message) || err))
    report('copy-error', String(err))
  } finally {
    COPY.busy = false
  }
}

/* ---------------- 代码块 / 表格的复制按钮 ----------------
   按钮 HTML 由渲染规则产出，点击在 document 上统一代理：块会被虚拟滚动反复
   重绘，逐个绑监听既多余也留不住。 */

const COPY_DONE_MS = 1500

function cellTextOf(cell) {
  let out = ''
  const walk = (node) => {
    for (let n = node.firstChild; n; n = n.nextSibling) {
      if (n.nodeType === 3) out += n.nodeValue
      else if (n.nodeName === 'BR') out += ' '
      else if (n.nodeType === 1) walk(n)
    }
  }
  walk(cell)
  // 单元格里的换行与制表符会破坏 TSV 的行列结构，统一压成单个空格
  return out.replace(/\s+/g, ' ').trim()
}

function tableAsTsv(wrap) {
  const table = wrap.querySelector('table')
  if (!table) return ''
  const rows = []
  for (const tr of table.querySelectorAll('tr')) {
    const cells = []
    for (const cell of tr.children) {
      if (cell.tagName !== 'TD' && cell.tagName !== 'TH') continue
      cells.push(cellTextOf(cell))
    }
    if (cells.length) rows.push(cells.join('\t'))
  }
  return rows.join('\n')
}

// Markdown 源码只能回源文档切：渲染后的 DOM 里已经没有 | --- | 这类语法。
async function tableAsMarkdown(btn) {
  const l0 = parseInt(btn.dataset.l0, 10)
  const l1 = parseInt(btn.dataset.l1, 10)
  if (!Number.isInteger(l0) || !Number.isInteger(l1)) {
    throw new Error('这张表格没有源码位置')
  }
  if (btn.dataset.src === 'd') {
    // 开发模式：整篇解析，行号相对全文
    if (typeof state.docText !== 'string') throw new Error('文档源码不可用')
    return state.docText.split(/\r?\n/).slice(l0, l1).join('\n')
  }
  const i = parseInt(btn.dataset.i, 10)
  if (!Number.isInteger(i)) throw new Error('这张表格没有源码位置')
  const docId = V.docId
  const res = await fetch(BLOCKS_URL + '?from=' + i + '&count=1', { cache: 'no-store' })
  if (!res.ok) throw new Error('HTTP ' + res.status)
  if (docId !== V.docId) throw new Error('文档已切换')
  const item = parseFrames(await res.arrayBuffer()).find((it) => it.i === i)
  if (!item) throw new Error('块内容已不可用')
  return item.text.split(/\r?\n/).slice(l0, l1).join('\n')
}

function markCopied(btn) {
  const use = btn.querySelector('use')
  if (use) use.setAttribute('href', '#i-check')
  btn.dataset.done = '1'
  if (btn.resetTimer) clearTimeout(btn.resetTimer)
  btn.resetTimer = setTimeout(() => {
    if (use) use.setAttribute('href', '#i-copy')
    delete btn.dataset.done
  }, COPY_DONE_MS)
  const live = document.getElementById('mdv-live')
  if (live) live.textContent = '已复制'
}

async function copyFromButton(btn) {
  const wrap = btn.closest('.mdv-cb')
  if (!wrap) return
  const isTable = btn.dataset.k === 'table'
  let text = ''
  try {
    if (isTable && btn.dataset.big) {
      // 巨型表格：DOM 里可能还没补全，直接从源码整块取
      const i = parseInt(btn.dataset.big, 10)
      if (!Number.isInteger(i)) throw new Error('这张表格没有源码位置')
      const docId = V.docId
      const src = await bigTableSource(i)
      if (docId !== V.docId) throw new Error('文档已切换')
      text = state.render.tableCopyMd ? src : bigTextAsTsv(src)
    } else if (isTable) {
      text = state.render.tableCopyMd ? await tableAsMarkdown(btn) : tableAsTsv(wrap)
    } else {
      const code = wrap.querySelector('pre > code')
      text = code ? code.textContent : ''
    }
  } catch (err) {
    postRaw('status|复制失败：' + String((err && err.message) || err))
    return
  }
  text = text.replace(/\r\n/g, '\n').replace(/\n+$/, '')
  if (!text) {
    postRaw('status|没有可复制的内容')
    return
  }
  postRaw('clip|' + (isTable ? '表格' : '代码') + '|' + text)
  markCopied(btn)
}

document.addEventListener('click', (e) => {
  const btn = e.target && e.target.closest ? e.target.closest('.mdv-copy') : null
  if (!btn) return
  e.preventDefault()
  copyFromButton(btn)
})

/* ---------------- 图片点击放大 ----------------
   左键点图开黑底遮罩（滚轮以光标为中心缩放 / 按住拖动平移 / 双击 1:1↔适应 /
   Esc 或点背景关闭）；右键图发 imgctx|<URL>|<local|remote> 让宿主弹图片动作区，
   菜单里的「放大查看」再由宿主发 zoom| 回来开遮罩。
   路由与几何抽成顶层纯函数：回归测试从本文件抠真实实现断言（test/img-zoom.mjs）。 */

// 判定一张图此刻的动作。attrs 是 img 的属性快照：
//   { inLink, src, mdvSrc, mdvNet } —— mdvNet 标记远程图，mdvSrc 是远程真身
// button 是 'left' | 'right'。返回 { act, kind, url }：
//   act='zoom'   开遮罩（左键）
//   act='imgctx' 发 imgctx 帧（右键）
//   act=null     这张图此刻不可动手：无 src、未加载的远程图、链接内左键
function imgAction(attrs, button) {
  const src = attrs.src || ''
  if (!src) return { act: null, kind: null, url: '' }
  let kind = null
  let url = ''
  if (attrs.mdvNet && attrs.mdvSrc) {
    // 远程图只有「确认后真正加载」这一种状态可动手：src 被换成 data-mdv-src
    // 才算到位；等待确认、被拒、加载失败都是 1x1 透明占位，都到不了这里
    if (src !== attrs.mdvSrc) return { act: null, kind: null, url: '' }
    kind = 'remote'
    url = attrs.mdvSrc
  } else if (/^https:\/\/mdblocks\.localhost\/__asset(?:[?#]|$)/i.test(src) ||
             /^(?:data|blob):/i.test(src)) {
    kind = 'local'
    url = src
  } else {
    return { act: null, kind: null, url: '' }
  }
  if (button === 'left') {
    // 链接里的图片左键仍跟随链接（[![](thumb)](href) 的既有语义），放大走右键
    if (attrs.inLink) return { act: null, kind: null, url: '' }
    return { act: 'zoom', kind, url }
  }
  return { act: 'imgctx', kind, url }
}

// ---- 遮罩几何 ----
// 缩放态一律 { fit, scale, x, y }：fit 是「适应窗口」倍率，scale 当前倍率，
// x/y 是图片左上角在遮罩里的位置（图比视口大时是负数）。

// contain：完整显示、宽高比不变；小图也放大塞满可用空间（object-fit: contain 口径）。
function zoomFitScale(natW, natH, availW, availH) {
  if (!(natW > 0) || !(natH > 0) || !(availW > 0) || !(availH > 0)) return 1
  return Math.min(availW / natW, availH / natH)
}

// 允许的倍率范围：fit 与 1:1 都必须可达（大图缩到 fit 还能双击回 1:1，
// 小图放到 fit 还能缩回 1:1），两端各再留 8 倍余量。
function zoomScaleBounds(fit) {
  const f = fit > 0 && isFinite(fit) ? fit : 1
  return { min: Math.min(f, 1) / 8, max: Math.max(f, 1) * 8 }
}

// 平移边界：图比视口小的轴固定居中，大的轴图像边缘不许离开视口（不露缝不出界）。
function zoomClampOffset(x, y, dispW, dispH, vpW, vpH) {
  return {
    x: dispW <= vpW ? (vpW - dispW) / 2 : Math.min(Math.max(x, vpW - dispW), 0),
    y: dispH <= vpH ? (vpH - dispH) / 2 : Math.min(Math.max(y, vpH - dispH), 0),
  }
}

// 以光标为中心缩放：光标下的图像点前后不动，再钳倍率与平移边界。
function zoomWheelZoom(cur, cursor, factor) {
  const b = zoomScaleBounds(cur.fit)
  const scale = Math.min(Math.max(cur.scale * factor, b.min), b.max)
  const px = (cursor.x - cur.x) / cur.scale
  const py = (cursor.y - cur.y) / cur.scale
  const c = zoomClampOffset(cursor.x - px * scale, cursor.y - py * scale,
                            cur.natW * scale, cur.natH * scale, cur.vpW, cur.vpH)
  return { scale, x: c.x, y: c.y }
}

// 双击：在 1:1 与适应窗口之间切换，切换后的点仍锚在光标下。
function zoomToggleZoom(cur, cursor) {
  const scale = Math.abs(cur.scale - cur.fit) <= Math.abs(cur.scale - 1) ? 1 : cur.fit
  const px = (cursor.x - cur.x) / cur.scale
  const py = (cursor.y - cur.y) / cur.scale
  const c = zoomClampOffset(cursor.x - px * scale, cursor.y - py * scale,
                            cur.natW * scale, cur.natH * scale, cur.vpW, cur.vpH)
  return { scale, x: c.x, y: c.y }
}

// 打开遮罩的初始态：适应窗口 + 居中。
function zoomInitialState(natW, natH, vpW, vpH) {
  const fit = zoomFitScale(natW, natH, vpW, vpH)
  const c = zoomClampOffset((vpW - natW * fit) / 2, (vpH - natH * fit) / 2,
                            natW * fit, natH * fit, vpW, vpH)
  return { fit, scale: fit, x: c.x, y: c.y }
}

// ---- DOM 侧 ----

const IMGZ = {
  open: false,
  box: null,
  img: null,
  natW: 0, natH: 0,
  fit: 1, scale: 1, x: 0, y: 0,
  drag: null,    // pointerdown 起点 { x, y, ox, oy, moved }
  pending: null, // imgctx 存下的 { url, docId }，等宿主 zoom| 回来开遮罩
}

function imgSnapshot(img) {
  return {
    inLink: !!(img.closest && img.closest('a')),
    src: img.getAttribute('src') || '',
    mdvSrc: img.getAttribute('data-mdv-src') || '',
    mdvNet: img.getAttribute('data-mdv-net') === '1',
  }
}

function zoomCur() {
  return { fit: IMGZ.fit, scale: IMGZ.scale, x: IMGZ.x, y: IMGZ.y,
           natW: IMGZ.natW, natH: IMGZ.natH,
           vpW: window.innerWidth, vpH: window.innerHeight }
}

function applyZoomTransform() {
  IMGZ.img.style.width = (IMGZ.natW * IMGZ.scale) + 'px'
  IMGZ.img.style.height = (IMGZ.natH * IMGZ.scale) + 'px'
  IMGZ.img.style.transform = 'translate(' + IMGZ.x + 'px,' + IMGZ.y + 'px)'
}

function zoomMaskLoaded() {
  IMGZ.natW = IMGZ.img.naturalWidth
  IMGZ.natH = IMGZ.img.naturalHeight
  const st = zoomInitialState(IMGZ.natW, IMGZ.natH, window.innerWidth, window.innerHeight)
  IMGZ.fit = st.fit
  IMGZ.scale = st.scale
  IMGZ.x = st.x
  IMGZ.y = st.y
  applyZoomTransform()
}

function ensureZoomMask() {
  if (IMGZ.box) return IMGZ.box
  const box = document.createElement('div')
  box.id = 'mdv-zoom'
  box.hidden = true
  const img = document.createElement('img')
  img.draggable = false
  img.alt = ''
  img.addEventListener('load', () => {
    if (IMGZ.open) zoomMaskLoaded()
  })
  box.appendChild(img)

  // 右上角关闭按钮：不想摸键盘找 Esc 的路径。pointerdown 拦住，别让按它变成拖动起点
  const btn = document.createElement('button')
  btn.type = 'button'
  btn.className = 'mdv-zoom-close'
  btn.title = '关闭 (Esc)'
  btn.setAttribute('aria-label', '关闭')
  btn.innerHTML = '<svg class="mdv-i" aria-hidden="true"><use href="#i-close"></use></svg>'
  btn.addEventListener('pointerdown', (e) => e.stopPropagation())
  btn.addEventListener('click', (e) => {
    e.preventDefault()
    closeZoomMask()
  })
  box.appendChild(btn)

  IMGZ.box = box
  IMGZ.img = img

  box.addEventListener('wheel', (e) => {
    e.preventDefault()
    e.stopPropagation()
    if (!IMGZ.open || !(IMGZ.natW > 0)) return
    // deltaMode=1（行）按每行 33px 折算，与文档滚动的 em 折算同数量级
    const dy = e.deltaMode === 1 ? (e.deltaY || 0) * 33 : (e.deltaY || 0)
    const st = zoomWheelZoom(zoomCur(), { x: e.clientX, y: e.clientY },
                             Math.exp(-dy * 0.0015))
    IMGZ.scale = st.scale
    IMGZ.x = st.x
    IMGZ.y = st.y
    applyZoomTransform()
  }, { passive: false })

  box.addEventListener('dblclick', (e) => {
    e.preventDefault()
    if (!IMGZ.open || !(IMGZ.natW > 0)) return
    const st = zoomToggleZoom(zoomCur(), { x: e.clientX, y: e.clientY })
    IMGZ.scale = st.scale
    IMGZ.x = st.x
    IMGZ.y = st.y
    applyZoomTransform()
  })

  box.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return
    e.preventDefault()
    IMGZ.drag = { x: e.clientX, y: e.clientY, ox: IMGZ.x, oy: IMGZ.y, moved: false }
  })

  // 拖动/点击的收尾挂 window：按住拖出遮罩、在遮罩外松开也要算数
  const onDragMove = (e) => {
    const d = IMGZ.drag
    if (!d || !IMGZ.open) return
    const dx = e.clientX - d.x
    const dy = e.clientY - d.y
    if (!d.moved && Math.abs(dx) + Math.abs(dy) < 4) return
    d.moved = true
    if (!(IMGZ.natW > 0)) return
    const c = zoomClampOffset(d.ox + dx, d.oy + dy,
                              IMGZ.natW * IMGZ.scale, IMGZ.natH * IMGZ.scale,
                              window.innerWidth, window.innerHeight)
    IMGZ.x = c.x
    IMGZ.y = c.y
    applyZoomTransform()
  }
  const onDragUp = (e) => {
    const d = IMGZ.drag
    IMGZ.drag = null
    if (!d || !IMGZ.open) return
    if (d.moved) return  // 拖完松手不算点击
    if (e.target === IMGZ.img) return  // 点在图片上不关，点背景才关
    closeZoomMask()
  }
  window.addEventListener('pointermove', onDragMove)
  window.addEventListener('pointerup', onDragUp)
  window.addEventListener('pointercancel', onDragUp)
  document.body.appendChild(box)
  return box
}

function openZoomMask(url) {
  if (!url) return
  const box = ensureZoomMask()
  IMGZ.open = true
  IMGZ.url = url
  IMGZ.natW = 0
  IMGZ.natH = 0
  box.hidden = false
  IMGZ.img.removeAttribute('src')
  IMGZ.img.src = url
  // 缓存命中时 load 可能在赋 src 前就齐活，补一次同步检查
  if (IMGZ.img.complete && IMGZ.img.naturalWidth > 0) zoomMaskLoaded()
}

function closeZoomMask() {
  if (!IMGZ.open) return
  IMGZ.open = false
  IMGZ.drag = null
  IMGZ.pending = null
  IMGZ.url = ''
  if (IMGZ.box) IMGZ.box.hidden = true
  if (IMGZ.img) IMGZ.img.removeAttribute('src')
}

// 左键：不可动手的图（链接内、未加载远程图、无 src）imgAction 给 null，
// 落回链接与文本选择的既有行为
document.addEventListener('click', (e) => {
  if (IMGZ.open) return  // 遮罩开着时点击归遮罩（背景关闭在 pointerup 里做）
  const img = e.target && e.target.closest ? e.target.closest('img') : null
  if (!img || !img.isConnected) return
  const act = imgAction(imgSnapshot(img), 'left')
  if (act.act !== 'zoom') return
  e.preventDefault()
  openZoomMask(act.url)
})

// 右键：可动手的图发 imgctx 并拦下全局菜单（ui-ext 的 contextmenu 监听在本文件
// 之后才注册，stopImmediatePropagation 正好把它停掉）；不可动手的图照旧全局菜单。
document.addEventListener('contextmenu', (e) => {
  if (IMGZ.open) {
    // 遮罩里不弹菜单：编码/代码块这些全局项盖在放大视图上没有意义。
    // 右键等同于右上角的 ×，整块遮罩任意位置都算；但左键正按着拖拽时忽略，
    // 免得拖到一半图突然没了。
    e.preventDefault()
    e.stopImmediatePropagation()
    if (!IMGZ.drag) closeZoomMask()
    return
  }
  const img = e.target && e.target.closest ? e.target.closest('img') : null
  if (!img || !img.isConnected) return
  const act = imgAction(imgSnapshot(img), 'right')
  if (act.act !== 'imgctx') return
  e.preventDefault()
  e.stopImmediatePropagation()
  IMGZ.pending = { url: act.url, docId: V.docId }
  postRaw('imgctx|' + act.url + '|' + act.kind)
})

/* ---------------- 搜索与大纲 ---------------- */

function foldAscii(text) {
  // 兜底路径（正则不可用时）只折叠 ASCII；正常路径由 findOccurrences 里的 i+u 正则负责
  return text.replace(/[A-Z]/g, (c) => String.fromCharCode(c.charCodeAt(0) + 32))
}

function escapeRegex(text) {
  return text.replace(/[\\^$.*+?()[\]{}|]/g, '\\$&')
}

// 不区分大小写时交给正则的 i+u：V8 的 Canonicalize 做 Unicode 简单折叠，
// 与后端 PCRE2 的口径一致，而且下标仍落在原串上（toLowerCase 会因 ß/İ 变长而错位）。
function findOccurrences(text, query, caseSensitive, wholeWord) {
  if (!query) return []
  const out = []
  if (!caseSensitive) {
    let re = null
    try {
      re = new RegExp(escapeRegex(query), 'giu')
    } catch {
      re = null
    }
    if (re) {
      let m
      while ((m = re.exec(text)) !== null) {
        if (!wholeWord ||
            (!isWordCharAt(text, m.index - 1) && !isWordCharAt(text, m.index + m[0].length))) {
          out.push(m.index)
        }
        if (m[0] === '') re.lastIndex++
        if (out.length >= 500) break
      }
      return out
    }
  }
  const hay = caseSensitive ? text : foldAscii(text)
  const needle = caseSensitive ? query : foldAscii(query)
  let p = 0
  for (;;) {
    p = hay.indexOf(needle, p)
    if (p < 0) break
    if (!wholeWord || (!isWordCharAt(hay, p - 1) && !isWordCharAt(hay, p + needle.length))) {
      out.push(p)
    }
    p += Math.max(1, needle.length)
    if (out.length >= 500) break
  }
  return out
}

// 与 C++ 一致：整词边界只认 ASCII 字母、数字、下划线。
function isWordCharAt(text, i) {
  if (i < 0 || i >= text.length) return false
  const c = text.charCodeAt(i)
  return (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95
}

function clearHitMarks() {
  for (const el of document.querySelectorAll('.hit--block')) el.classList.remove('hit--block')
  for (const el of document.querySelectorAll('.hit--row')) el.classList.remove('hit--row')
  for (const mark of document.querySelectorAll('mark.hit')) {
    const parent = mark.parentNode
    if (!parent) continue
    while (mark.firstChild) parent.insertBefore(mark.firstChild, mark)
    parent.removeChild(mark)
    parent.normalize()
  }
}

// 把 [start, end) 字符区间包成 mark；跨节点时逐节点包。
function wrapTextRange(root, start, end) {
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT)
  let pos = 0
  let node
  const parts = []
  while ((node = walker.nextNode())) {
    const len = node.nodeValue.length
    const a = Math.max(0, start - pos)
    const b = Math.min(len, end - pos)
    if (a < b) parts.push([node, a, b])
    pos += len
    if (pos >= end) break
  }
  for (const [textNode, a, b] of parts) {
    const range = document.createRange()
    range.setStart(textNode, a)
    range.setEnd(textNode, b)
    const mark = document.createElement('mark')
    mark.className = 'hit hit--current'
    range.surroundContents(mark)
  }
}

// 锚点跳转后给目标块一次短促的闪烁：否则只有滚动位置变了，看不出跳到哪。
function animateBlock(el) {
  const target = el.firstElementChild || el
  target.classList.remove('hit--flash')
  void target.offsetWidth   // 强制回流，动画能重放
  target.classList.add('hit--flash')
  setTimeout(() => target.classList.remove('hit--flash'), 1200)
}

function flashBlock(i) {
  const rec = V.resident.get(i)
  if (rec && rec.el) animateBlock(rec.el)
  else V.flashBlock = i
}

function jumpToOffset(y) {
  if (!V.active) return
  V.lastScrollAt = performance.now()
  const max = Math.max(0, V.totalH - V.vh)
  V.off = Math.min(Math.max(0, y), max)
  V.target = V.off
  place()
}

function updateFindCount() {
  if (!findCount) return
  let text = ''
  if (FIND.open && FIND.query) {
    if (FIND.busy) {
      text = '\u2026'
    } else if (FIND.error) {
      text = FIND.error.length > 28 ? FIND.error.slice(0, 28) + '\u2026' : FIND.error
    } else if (!FIND.results.length) {
      // 命中可能全在隐藏块（脚注定义、引用定义）里：计数有，但没有能跳过去的目标
      text = FIND.total > 0 ? FIND.total + ' 处（不可跳转）' : '无结果'
    } else {
      text = (FIND.index >= 0 ? FIND.index + 1 : 0) + '/' + FIND.total
        + (FIND.truncated ? '+' : '') + (FIND.limited ? '?' : '')
    }
  }
  findCount.textContent = text
}

function openFind() {
  if (!V.active) return
  FIND.open = true
  findEl.hidden = false
  syncTools()
  findInput.focus()
  findInput.select()
}

function closeFind() {
  FIND.open = false
  FIND.seq++
  if (FIND.timer) {
    clearTimeout(FIND.timer)
    FIND.timer = 0
  }
  FIND.results = []
  FIND.index = -1
  FIND.total = 0
  FIND.truncated = false
  FIND.busy = false
  FIND.error = ''
  FIND.limited = false
  FIND.pending = null
  FIND.current = null
  if (findEl) findEl.hidden = true
  if (findInput) findInput.blur()
  syncTools()
  clearHitMarks()
  updateFindCount()
}

function scheduleSearch() {
  FIND.query = findInput.value
  FIND.results = []
  FIND.index = -1
  FIND.total = 0
  FIND.truncated = false
  FIND.error = ''
  FIND.limited = false
  FIND.pending = null
  FIND.current = null
  FIND.seq++
  clearHitMarks()
  if (FIND.timer) clearTimeout(FIND.timer)
  if (!FIND.query) {
    FIND.busy = false
    FIND.timer = 0
    updateFindCount()
    return
  }
  FIND.busy = true
  updateFindCount()
  FIND.timer = setTimeout(runSearch, 220)
}

function runSearch() {
  FIND.timer = 0
  if (!V.active || !FIND.query) return
  const seq = ++FIND.seq
  const docId = V.docId
  FIND.busy = true
  FIND.error = ''
  FIND.limited = false
  FIND.pending = null
  FIND.current = null
  clearHitMarks()
  updateFindCount()

  const url = SEARCH_URL + '?q=' + encodeURIComponent(FIND.query)
    + (FIND.cs ? '&cs=1' : '')
    + (!FIND.re && FIND.ww ? '&ww=1' : '')
    + (FIND.re ? '&re=1' : '')
  fetch(url, { cache: 'no-store' })
    .then((res) => {
      if (!res.ok) throw new Error('HTTP ' + res.status)
      return res.arrayBuffer()
    })
    .then((buf) => {
      if (seq !== FIND.seq || docId !== V.docId || !V.active) return
      if (buf.byteLength < 12) throw new Error('bad search frame')
      const dv = new DataView(buf)
      const total = dv.getUint32(0, true)
      const count = dv.getUint32(4, true)
      const flags = dv.getUint32(8, true)
      if (flags & 2) {
        let msg = '正则无效'
        if (buf.byteLength >= 16) {
          const len = dv.getUint32(12, true)
          if (len && 16 + len <= buf.byteLength) {
            msg = new TextDecoder().decode(new Uint8Array(buf, 16, len))
          }
        }
        FIND.error = msg
        FIND.results = []
        FIND.total = 0
        FIND.truncated = false
        FIND.limited = false
        FIND.index = -1
        FIND.busy = false
        updateFindCount()
        report('search-error', msg)
        return
      }
      const results = []
      let o = 12
      for (let i = 0; i < count && o + 12 <= buf.byteLength; i++) {
        results.push({
          b: dv.getUint32(o, true),
          off: dv.getUint32(o + 4, true),
          line: dv.getUint32(o + 8, true),
        })
        o += 12
      }
      FIND.results = results
      FIND.total = total
      FIND.truncated = (flags & 1) !== 0
      FIND.limited = (flags & 4) !== 0
      FIND.error = ''
      FIND.index = -1
      FIND.busy = false
      updateFindCount()
      if (results.length) goToHit(0)
    })
    .catch((err) => {
      if (seq !== FIND.seq || docId !== V.docId) return
      FIND.busy = false
      FIND.results = []
      FIND.total = 0
      FIND.index = -1
      updateFindCount()
      report('search-error', String(err))
    })
}

function goToHit(i) {
  const n = FIND.results.length
  if (!n) return
  FIND.index = ((i % n) + n) % n
  const hit = FIND.results[FIND.index]
  FIND.pending = hit
  FIND.current = null
  FIND.bigHit = null
  clearHitMarks()
  updateFindCount()
  // 巨型块直接由 maybeApplyHit 定位到命中行，先跳块顶会多取一次用不上的行窗口。
  if (!V.big.has(hit.b) && !V.bigTable.has(hit.b)) jumpToOffset(pos(hit.b) - V.vh * 0.15)
  maybeApplyHit()
}

function segIndexOfLine(i, line) {
  const n = Math.max(1, segCount(i))
  return Math.min(n - 1, Math.max(0, Math.floor(line / BIG_SEG_LINES)))
}

// 行号 → 文档 Y。与 updateBigWindows 里「视口顶在第几行」的算法同源，误差只来自
// overhead 把段的上下内边距一起算进去（几 px）。
function bigLineTop(i, line) {
  const g = bigMetrics()
  const k = segIndexOfLine(i, line)
  return pos(i) + segTop(i, k) + (k === 0 ? g.overhead : 0)
    + (line - k * BIG_SEG_LINES) * g.lineH
}

function maybeApplyHit() {
  const hit = FIND.pending
  if (!hit || !V.active) return
  if (V.bigTable.has(hit.b)) {
    // 巨型表格：先按行号算出落在哪一块，补到那一块再滚过去点那一行
    FIND.pending = null
    FIND.current = hit
    const m = V.bigTable.get(hit.b)
    const total = m && m.lines ? m.lines : 0
    const line = total ? Math.min(Math.max(0, hit.line | 0), total - 1) : 0
    FIND.bigHit = { b: hit.b, line }
    applyBigHitMark()
    return
  }
  if (V.big.has(hit.b)) {
    // 巨型块没有渲染文本可用：先滚到命中行，行窗口到位后再在那一行里包 mark。
    FIND.pending = null
    FIND.current = hit
    const m = V.big.get(hit.b)
    const total = m && m.lines ? m.lines : 0
    const line = total ? Math.min(Math.max(0, hit.line | 0), total - 1) : 0
    FIND.bigHit = { b: hit.b, line }
    jumpToOffset(bigLineTop(hit.b, line) - V.vh * 0.25)
    applyBigHitMark()
    return
  }
  if (hit.b < V.windowStart || hit.b > V.windowEnd) return
  const rec = V.resident.get(hit.b)
  if (!rec || !rec.el || !rec.text) return
  FIND.pending = null
  FIND.current = hit
  applyHitMark(rec.el, rec.text, hit.off)
}

// 在巨型块的行窗口里给命中行包 mark。行窗口是异步取的，所以由 place() 每帧调一次：
// 行还没到就直接返回，到了就一次画上并清掉待办。
function applyBigHitMark() {
  const want = FIND.bigHit
  if (!want || !V.active || !FIND.query) return
  if (V.bigTable.has(want.b)) {
    bigTableHitMark(want)
    return
  }
  const rec = V.resident.get(want.b)
  if (!rec || !rec.big) return
  const seg = rec.big.segs.get(segIndexOfLine(want.b, want.line))
  if (!seg || seg.from < 0) return
  const rel = want.line - seg.from - (seg.dropHead || 0)
  if (rel < 0 || rel >= seg.got) return
  const text = seg.code.textContent || ''
  let start = 0
  for (let n = 0; n < rel; n++) {
    const p = text.indexOf('\n', start)
    if (p < 0) return
    start = p + 1
  }
  let end = text.indexOf('\n', start)
  if (end < 0) end = text.length
  const lineText = text.slice(start, end)
  let at = -1
  let len = 0
  if (FIND.re) {
    const ms = regexMatches(lineText, FIND.query, FIND.cs)
    if (ms.length) { at = ms[0].start; len = ms[0].len }
  } else {
    const starts = findOccurrences(lineText, FIND.query, FIND.cs, FIND.ww)
    if (starts.length) { at = starts[0]; len = FIND.query.length }
  }
  if (at < 0) return
  clearHitMarks()
  wrapTextRange(seg.code, start + at, start + at + len)
  FIND.bigHit = null
}

/* ---------------- 巨型表格：分块真表格 ----------------
   一张两万行的表按行号分页取（__lines 每块有顺序缓存，顺着读很便宜），
   每 500 行渲染成一张独立的块级 <table>，全部挂 content-visibility:auto。
   实测（20000 行 × 5 列，本机 Edge）：单表一次性布局 1.8 s；逐批追加进同一张表
   4.7 s（每次追加都触发整表重排）；分块表首帧 0.14 s。把 content-visibility 加在
   tr / tbody 上在 Chromium 里不生效（仍 1.3 s），所以拆的粒度只能是块级表。

   分块表的列宽从首块的自然排版量一次，之后所有块按同一组 px 固定，避免各算各的
   宽度。块大小取偶数，斑马纹跨块才不会翻。 */

function bigTableFetch(i, from, count) {
  return fetch(LINES_URL + '?i=' + i + '&from=' + from + '&count=' + count, { cache: 'no-store' })
    .then((res) => {
      // 越界（from 超过块内行数）宿主回 404：当成「没有更多行」，不是错误
      if (res.status === 404) return null
      if (!res.ok) throw new Error('HTTP ' + res.status)
      return res.arrayBuffer()
    })
    .then((buf) => {
      if (!buf || buf.byteLength < 20) return { lines: [], total: 0 }
      const dv = new DataView(buf)
      if (dv.getUint32(0, true) !== i || dv.getUint32(4, true) !== from) {
        return { lines: [], total: 0 }
      }
      const got = dv.getUint32(8, true)
      const total = dv.getUint32(12, true)
      const len = dv.getUint32(16, true)
      if (20 + len > buf.byteLength || got === 0) return { lines: [], total }
      const lines = decoder.decode(new Uint8Array(buf, 20, len)).split('\n')
      if (lines.length && lines[lines.length - 1] === '') lines.pop()
      for (let k = 0; k < lines.length; k++) {
        if (lines[k].endsWith('\r')) lines[k] = lines[k].slice(0, -1)
      }
      return { lines, total }
    })
}

// 单元格按未转义的 | 切；`\|` 是单元格里的竖线，不是分隔符
function tableRowCells(line) {
  let s = line.trim()
  if (s.startsWith('|')) s = s.slice(1)
  if (s.endsWith('|')) s = s.slice(0, -1)
  const out = []
  let cur = ''
  for (let k = 0; k < s.length; k++) {
    const ch = s[k]
    if (ch === '\\') {
      // 反斜杠连跑要一起数：`\|` 是转义的竖线，`\\|` 是转义反斜杠 + 分隔符。
      // 两个反斜杠折成一个字面反斜杠，剩下单个若用来转义竖线就直接丢掉。
      let run = 1
      while (s[k + run] === '\\') run++
      for (let n = 0; n < Math.floor(run / 2); n++) cur += '\\'
      k += run - 1
      if (run % 2 === 1) {
        if (s[k + 1] === '|') {
          cur += '\u0000'
          k++
        } else {
          cur += '\\'
        }
      }
      continue
    }
    if (ch === '|') {
      out.push(cur)
      cur = ''
      continue
    }
    cur += ch
  }
  out.push(cur)
  return out.map((c) => c.replace(/\u0000/g, '|').trim())
}

// 源码里的单元格转纯文本：TSV 里不该出现 ** / ` / [文字](链接) 这些记号
function tableCellPlain(s) {
  return s
    .replace(/!\[[^\]]*\]\([^)]*\)/g, '')
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/`([^`]*)`/g, '$1')
    .replace(/\*\*([^*]+)\*\*/g, '$1')
    .replace(/\*([^*]+)\*/g, '$1')
    // 单元格里的换行靠 <br>：TSV 一行是一行，换行只能落成空格（与从 DOM 取文本的路径一致）
    .replace(/<br\s*\/?>/gi, ' ')
    .replace(/<[^>]+>/g, '')
    .replace(/\s+/g, ' ')
    .trim()
}

function bigTextAsTsv(text) {
  const rows = []
  for (const raw of text.split('\n')) {
    const line = raw.trim()
    if (!line || line.indexOf('|') < 0) continue
    const cells = tableRowCells(line)
    if (!cells.length) continue
    if (cells.every((c) => c === '' || /^:?-{2,}:?$/.test(c))) continue  // 表头分隔行
    rows.push(cells.map(tableCellPlain).join('\t'))
  }
  return rows.join('\n')
}

// 一块的分块表 HTML。markdown-it 的表格规则会给整表套一层 .mdv-cb 与复制按钮，
// 这里只取 <table> 本身——按钮由外层那一个负责。
function bigTableChunkHtml(header, rows, withHead) {
  if (!rows.length) return ''
  const src = header + '\n' + rows.join('\n') + '\n'
  let html
  try {
    const env = blockEnv()
    env.blockIndex = -1
    html = md.render(src, env)
  } catch (err) {
    report('bigtable-render', String(err))
    return ''
  }
  const a = html.indexOf('<table')
  const b = html.lastIndexOf('</table>')
  if (a < 0 || b < a) return ''
  let out = html.slice(a, b + 8)
  if (!withHead) out = out.replace(/<thead>[\s\S]*?<\/thead>/, '')
  out = out.replace('<table', '<table class="bt-table"')
  return src.includes('<') ? sanitize(out) : out
}

// 把量出来的列宽写进这一块。窗口或字号变了要能重写，所以不能只做一次：
// 已有的 colgroup 先清空再填，否则第二个 colgroup 会被忽略、列宽永远停在首测值。
function bigTableFixWidth(t, bt) {
  if (!bt.colW || !bt.width) return
  t.style.tableLayout = 'fixed'
  t.style.width = bt.width + 'px'
  let cg = t.querySelector('colgroup')
  if (!cg) {
    cg = document.createElement('colgroup')
    t.insertBefore(cg, t.firstChild)
  }
  while (cg.firstChild) cg.removeChild(cg.firstChild)
  for (const w of bt.colW) {
    const col = document.createElement('col')
    col.style.width = w + 'px'
    cg.appendChild(col)
  }
  t.dataset.wfixed = '1'
}

// 列宽与行高都量出来之后，统一刷到所有已插入的块上。
// contain-intrinsic-size 也要跟着改：列宽变了，同一行可能在别处折行，行高随之变化。
function bigTableApplyWidth(bt) {
  const tables = bt.host.querySelectorAll('table.bt-table')
  for (const t of tables) {
    bigTableFixWidth(t, bt)
    const rows = Number(t.dataset.rows) || 0
    t.style.containIntrinsicSize = 'auto ' + Math.round(
      (t.tHead ? bt.headH : 0) + rows * bt.rowH) + 'px'
  }
}

// 巨型表列宽：自然宽（单行最宽内容）→ 最小列宽兜底 → 折行模式按正文宽压缩。
// 关掉折行时保持自然宽，超出的部分由包装层横向滚动；压缩后仍顶在最小列宽上的
// 列加起来放不下正文宽，同样交给包装层滚动。打印不走这里，另按折行排版。
function fitTableCols(natural, min, avail, wrap) {
  const out = natural.map((w, k) => Math.max(Math.ceil(w), Math.ceil(min[k] || 0)))
  const sum = out.reduce((a, b) => a + b, 0)
  if (!wrap || !(avail > 0) || sum <= avail) return out
  // 最小列宽先保住；可压缩的只有各列超出最小列宽的那部分，按比例分掉
  let floorSum = 0
  let extraSum = 0
  for (let k = 0; k < out.length; k++) {
    const m = Math.ceil(min[k] || 0)
    floorSum += m
    extraSum += out[k] - m
  }
  if (floorSum >= avail) {
    // 列数 × 最小列宽本身就放不下：停在最小列宽，由包装层横向滚动
    return out.map((w, k) => Math.ceil(min[k] || 0))
  }
  const k2 = (avail - floorSum) / extraSum
  return out.map((w, k) => {
    const m = Math.ceil(min[k] || 0)
    return m + Math.floor((w - m) * k2)
  })
}

// 可用宽度 = 所在块的内容宽（块自身居中、左右各 1.5em 内边距）。不能读
// bt.host.clientWidth：折行模式下包装层是 fit-content，宽度反过来由表格决定。
function bigTableContentWidth(rec) {
  const el = rec && rec.el
  if (!el) return 480
  const cs = getComputedStyle(el)
  const w = el.clientWidth - (parseFloat(cs.paddingLeft) || 0) -
    (parseFloat(cs.paddingRight) || 0)
  return Math.max(160, Math.round(w) || 480)
}

// 量一次：列宽与行高。窗口尺寸、字号、行宽、折行模式变化后要能量第二次。
//
// 三个坑都在这里处理：
//   1) 不能直接量已经插进 DOM 的那一块——它带着 content-visibility:auto，被跳过时
//      宽度返回的是占位值（实测列宽只剩 min-content，「序号」会被压成竖排）。
//   2) 只按首块 500 行定列宽会偏窄：行号到 20000 是五位数，「序号」「名称」两列在
//      后半张表里会被折行。所以在离屏探针里按「表头 + 首尾各 24 行」重排一次，
//      尾样本用 __lines 单独取；样本留着重测时复用，不必再取一次。
//   3) 表头是加粗字、末列还让出了复制按钮，只量 tbody 会让表头在那些列上折行。
async function bigTableMeasure(i, rec) {
  const bt = rec.table
  if (!bt.header || bt.measuring) return
  bt.measuring = true
  try {
    const width = bigTableContentWidth(rec)
    if (!bt.sample) {
      const head = (bt.firstRows || []).slice(0, 24)
      let tail = []
      if (bt.total > 96) {
        try {
          const part = await bigTableFetch(i, Math.max(2, bt.total - 24), 24)
          if (part) tail = part.lines
        } catch (err) {
          report('bigtable-sample-error', String(err))
        }
      }
      bt.sample = head.concat(tail)
      bt.firstRows = null
    }
    const cur = V.resident.get(i)
    if (!cur || cur.table !== bt) return
    const probe = document.createElement('div')
    probe.style.cssText =
      'position:absolute;left:-100000px;top:0;visibility:hidden'
    document.body.appendChild(probe)
    probe.innerHTML = bigTableChunkHtml(bt.header, bt.sample, true)
    const t = probe.querySelector('table')
    let colW = null
    if (t) {
      // 探针里的分块表不能懒渲染，否则量到的是占位值
      t.style.contentVisibility = 'visible'
      const thead = t.tHead
      const body = t.tBodies[0]
      const th = thead && thead.rows[0]
      const headRow = th || (body && body.rows[0])
      if (headRow && headRow.cells.length) {
        // 先让表格按单行内容的自然宽排版（宽度与当前折行模式无关）：
        // 逐行量单元格，取每列最宽的一条。向上取整再补 1px——高 DPI 下列宽
        // 带小数，四舍五入后每列会少不到 1px，加粗的表头字正好在这个误差上折行。
        t.style.width = 'max-content'
        t.style.maxWidth = 'none'
        const n = headRow.cells.length
        const nat = new Array(n).fill(0)
        const min = new Array(n).fill(0)
        const rows = []
        if (th) rows.push(th)
        if (body) for (const row of body.rows) rows.push(row)
        for (const row of rows) {
          if (row.cells.length !== n) continue
          for (let k = 0; k < n; k++) {
            const w = Math.ceil(row.cells[k].getBoundingClientRect().width) + 1
            if (w > nat[k]) nat[k] = w
          }
        }
        // 最小列宽 = 两个汉字 + 该列的左右内边距（末列表头还含复制按钮的让位）
        for (let k = 0; k < n; k++) {
          const cs = getComputedStyle(headRow.cells[k])
          min[k] = 2 * (parseFloat(cs.fontSize) || 15) +
            (parseFloat(cs.paddingLeft) || 0) + (parseFloat(cs.paddingRight) || 0) + 2
        }
        colW = fitTableCols(nat, min, width, state.render.tableWrap)
      }
      // 按算出的列宽以固定布局重排，再量行高：折行模式下列被压窄，行高会变
      if (colW) {
        t.style.tableLayout = 'fixed'
        t.style.width = colW.reduce((a, b) => a + b, 0) + 'px'
        let cg = t.querySelector('colgroup')
        if (!cg) {
          cg = document.createElement('colgroup')
          t.insertBefore(cg, t.firstChild)
        }
        while (cg.firstChild) cg.removeChild(cg.firstChild)
        for (const w of colW) {
          const col = document.createElement('col')
          col.style.width = w + 'px'
          cg.appendChild(col)
        }
        void t.offsetWidth
      }
      bt.headH = thead ? thead.offsetHeight : BT_EST_HEAD
      const row0 = body && body.rows[0]
      const lastRow = body && body.rows[Math.min(bt.sample.length, body.rows.length) - 1]
      if (row0 && lastRow && lastRow !== row0) {
        const step = lastRow.rowIndex - row0.rowIndex
        const rowH = step > 0 ? (lastRow.offsetTop - row0.offsetTop) / step : 0
        if (rowH > 0) bt.rowH = Math.max(10, rowH)
      }
    }
    probe.innerHTML = ''
    document.body.removeChild(probe)
    const after = V.resident.get(i)
    if (!after || after.table !== bt) return
    if (!colW || !colW.length) return
    bt.colW = colW
    bt.width = colW.reduce((a, b) => a + b, 0)
    bigTableApplyWidth(bt)
    // 此刻只有第一批行：按已测行高外推整表高度，滚动条立刻是整表长度，
    // 后台补块期间用户也能滚到任意位置（未补到的地方由视口优先补块追上）
    const lines = Math.max(1, (V.bigTable.get(i) || {}).lines || 1)
    setHeight(i, BT_EST_HEAD + lines * bt.rowH)
  } finally {
    bt.measuring = false
  }
}

// 宽度基准变了（拖窗口、改字号、改行宽）：列宽是按当时的可用宽度量的 px，
// 不重量就会被旧值锁死——窗口拖宽只会留白，拖窄就整片折行。
let bigTableRemeasureTimer = 0
function scheduleBigTableRemeasure() {
  if (bigTableRemeasureTimer) clearTimeout(bigTableRemeasureTimer)
  bigTableRemeasureTimer = setTimeout(() => {
    bigTableRemeasureTimer = 0
    if (!V.active) return
    for (const [i, rec] of V.resident) {
      const bt = rec.table
      if (!bt || bt.measuring) continue
      bt.colW = null
      bt.width = 0
      bigTableMeasure(i, rec)
    }
  }, 150)
}

function bigTableMount(i) {
  let rec = V.resident.get(i)
  if (rec && rec.table) return rec
  if (rec) {
    if (rec.big) releaseBig(rec)
    if (rec.el) {
      if (resizeObserver) resizeObserver.unobserve(rec.el)
      if (rec.el.parentNode) rec.el.parentNode.removeChild(rec.el)
      rec.el.innerHTML = ''
      V.pool.push(rec.el)
    }
  }
  const el = document.createElement('div')
  el.className = 'blk blk--table blk--bigtable'
  el.dataset.i = String(i)
  el.style.top = (pos(i) - V.stageBase) + 'px'
  const wrap = document.createElement('div')
  wrap.className = 'mdv-cb mdv-cb--table mdv-cb--bigtable'
  wrap.innerHTML = (state.render.copyTable
    ? copyButtonHtml('table', ' data-src="b" data-big="' + i + '"')
    : '') + '<div class="bt-host"></div>'
  el.appendChild(wrap)
  docEl.appendChild(el)
  const host = wrap.lastElementChild
  host.dataset.i = String(i)
  rec = {
    el,
    table: {
      // 总行数用索引给的值：__lines 只在读到块尾时才回报 total，首块拿不到
      host, wrap, total: ((V.bigTable.get(i) || {}).lines || 0),
      next: 0, rows: 0, header: '', text: '', firstRows: null, sample: null,
      busy: false, done: false, loadingTo: false, idle: 0, timer: false, measuring: false,
      rowH: BT_EST_ROW, headH: BT_EST_HEAD, colW: null, width: 0,
    },
  }
  rec.table.t0 = performance.now()
  V.resident.set(i, rec)
  ensureObserver()
  resizeObserver.observe(host)
  bigTableFill(i)
  return rec
}

function bigTableRelease(rec) {
  const bt = rec.table
  if (!bt) return
  if (bt.idle) {
    if (bt.timer) clearTimeout(bt.idle)
    else if (window.cancelIdleCallback) window.cancelIdleCallback(bt.idle)
  }
  bt.idle = 0
  bt.done = true
  bt.text = ''
  if (resizeObserver && bt.host) resizeObserver.unobserve(bt.host)
  if (rec.el) {
    if (rec.el.parentNode) rec.el.parentNode.removeChild(rec.el)
    rec.el.innerHTML = ''
    V.pool.push(rec.el)
  }
  rec.el = null
  rec.table = null
}

function bigTableDone(i, rec) {
  const bt = rec.table
  if (!bt) return
  if (!bt.done) {
    bt.done = true
    setHeight(i, bt.host.offsetHeight)
    // 补之前的 setHeight 可能落在初始化期（只存高度不写树），这里整体重算一次
    rebuildFenwick()
    schedulePlace()
    report('perf', 'filled rows=' + bt.rows + ' ms=' + Math.round(performance.now() - bt.t0) +
      ' h=' + Math.round(bt.host.offsetHeight))
  }
  if (FIND.bigHit && FIND.bigHit.b === i) applyBigHitMark()
}
// 首块渲染不出表格（源里没有分隔行等）：退回普通块渲染，宁可慢也不能空着
function bigTableFallback(i, why) {
  report('bigtable-fallback', why)
  V.bigTable.delete(i)
  const rec = V.resident.get(i)
  if (rec) {
    bigTableRelease(rec)
    V.resident.delete(i)
  }
  rebuildFenwick()
  schedulePlace()
}

async function bigTableFill(i) {
  const rec = V.resident.get(i)
  if (!rec || !rec.table) return
  const bt = rec.table
  if (bt.done || bt.busy) return
  const docId = V.docId
  const from = bt.next
  // 收尾不靠行数：请求多少行、回来多少行，短读就是到底了（越界请求宿主回 404，
  // 前端当成空结果）。
  const want = from === 0 ? BT_ROWS + 2 : BT_ROWS
  bt.busy = true
  let part
  try {
    part = await bigTableFetch(i, from, want)
  } catch (err) {
    bt.busy = false
    bt.done = true
    report('bigtable-error', String(err))
    return
  }
  bt.busy = false
  const cur = V.resident.get(i)
  if (docId !== V.docId || !cur || !cur.table) return
  const t = cur.table
  if (!part || !part.lines.length) {
    bigTableDone(i, cur)
    return
  }
  let rows = part.lines
  if (from === 0) {
    if (rows.length < 2) {
      bigTableFallback(i, '块首两行拼不出表头')
      return
    }
    t.header = rows[0] + '\n' + rows[1]
    rows = rows.slice(2)
    t.firstRows = rows
  }
  t.text += part.lines.join('\n') + '\n'
  t.next = from + part.lines.length
  t.rows += rows.length
  const html = bigTableChunkHtml(t.header, rows, from === 0)
  if (html) {
    t.host.insertAdjacentHTML('beforeend', html)
    const table = t.host.lastElementChild
    if (table) {
      table.dataset.rows = String(rows.length)
      if (from !== 0) bigTableFixWidth(table, t)
      table.style.containIntrinsicSize = 'auto ' + Math.round(
        (from === 0 ? t.headH : 0) + rows.length * t.rowH) + 'px'
    }
  }
  if (from === 0) bigTableMeasure(i, cur)
  // 短读不等于读到底：1 MB 窗口装满时宿主也只回一部分，那时后面还有行。宿主只有
  // 扫到块尾才会给出 total，所以「有 total 且短读」才是真收尾，否则继续往后取。
  if (part.lines.length < want && part.total !== 0) {
    bigTableDone(i, cur)
    return
  }
  bigTableSchedule(i)
}

// 视口正在看的那一块还没补到：补过去（每次只走一块，由 loadingTo 自己限流）。
// 后台补块在滚动期间会让路，这里保证「正在看的地方」不会长时间空着。
let bigTableLoadAt = 0
function bigTableEnsureVisible() {
  if (!V.bigTable.size) return
  if (Math.abs(V.target - V.off) > V.vh * 1.5) return
  // 一次补块是 60–80 ms 的同步任务：每帧都起一条补块链，滚动就被它切成幻灯片。
  for (const [i, rec] of V.resident) {
    const bt = rec.table
    if (!bt || bt.done || bt.loadingTo) continue
    const top = pos(i)
    const bh = heightOf(i)
    if (V.off + V.vh <= top || V.off >= top + bh) continue
    const rowH = bt.rowH > 0 ? bt.rowH : BT_EST_ROW
    const ci = Math.floor(Math.max(0, V.off - top - BT_EST_HEAD) / rowH / BT_ROWS)
    // 多补一块缓冲：滚动速度超过「一块/补块周期」时，下一块已经在 DOM 里，
    // 不会等滚到空白区才开始同步渲染（那一下就是明显的掉帧）。
    if (bt.next < 2 + (ci + 1) * BT_ROWS) {
      const now = performance.now()
      if (now - bigTableLoadAt < 120) return
      bigTableLoadAt = now
      bigTableLoadTo(i, ci + 1)
    }
  }
}

// 后台补：每个空闲任务补一块；滚动期间让路，手停下来再继续
function bigTableSchedule(i) {
  const rec = V.resident.get(i)
  if (!rec || !rec.table) return
  const bt = rec.table
  if (bt.done || bt.idle) return
  const step = () => {
    bt.idle = 0
    // 一次补块（取行 + 解析 500 行 + 建 DOM）是 60–80 ms 的长任务。滚动期间补，
    // 每补一块就要掉几帧（实测 p99 从 25 ms 涨到 100 ms），所以等手停下来再补。
    if (performance.now() - V.lastScrollAt < 260) {
      bt.timer = true
      // 重排前必须把 idle 清掉：bigTableSchedule 自己会用 idle 判重入，
      // 带着旧 id 回调进去会被当成「已经在跑了」直接返回，补块从此停住。
      bt.idle = setTimeout(() => {
        bt.idle = 0
        bigTableSchedule(i)
      }, 150)
      return
    }
    bigTableFill(i)
  }
  if (window.requestIdleCallback) {
    bt.timer = false
    bt.idle = window.requestIdleCallback(step, { timeout: 150 })
  } else {
    bt.timer = true
    bt.idle = setTimeout(step, 16)
  }
}

// 顺序补到第 chunkIdx 块（搜索命中、跳到表格深处时用）
async function bigTableLoadTo(i, chunkIdx) {
  const rec = V.resident.get(i)
  if (!rec || !rec.table || rec.table.loadingTo) return
  rec.table.loadingTo = true
  try {
    for (let guard = 0; guard < 80; guard++) {
      const cur = V.resident.get(i)
      if (!cur || !cur.table || cur.table.done) break
      const doneChunks = Math.floor(Math.max(0, cur.table.next - 2) / BT_ROWS)
      if (doneChunks > chunkIdx) break
      const before = cur.table.next
      await bigTableFill(i)
      const after = V.resident.get(i)
      if (!after || !after.table || after.table.next === before) break
    }
  } finally {
    const cur = V.resident.get(i)
    if (cur && cur.table) cur.table.loadingTo = false
  }
}

// 命中行 → 块号 → 块内行号。行号与 __lines 同口径：0 是表头行，1 是分隔行。
function bigTableHitMark(want) {
  const rec = V.resident.get(want.b)
  if (!rec || !rec.table) return
  const bt = rec.table
  const row = Math.max(0, (want.line | 0) - 2)
  const ci = Math.floor(row / BT_ROWS)
  if (bt.next < 2 + ci * BT_ROWS) {
    bigTableLoadTo(want.b, ci)
    return
  }
  const table = bt.host.querySelectorAll('table.bt-table')[ci]
  if (!table) return
  const body = table.tBodies[0]
  const tr = body && body.rows[row - ci * BT_ROWS]
  if (!tr) return
  if (!want.placed) {
    want.placed = 1
    // 行高在整张表里并不一致（后面的行号位数多、单元格会折行），按行高乘出来的位置
    // 会偏。直接把这一块临时变成可见量真实矩形：目标行正好在视口附近，顺带就渲染了。
    const prev = table.style.contentVisibility
    table.style.contentVisibility = 'visible'
    const top = tr.getBoundingClientRect().top - rec.el.getBoundingClientRect().top
    table.style.contentVisibility = prev
    jumpToOffset(pos(want.b) + top - V.vh * 0.25)
  }
  clearHitMarks()
  tr.classList.add('hit--row')
  FIND.bigHit = null
}

// 整块源码：块还常驻且补完了就直接用内存里的，否则顺着行号取一遍
async function bigTableSource(i) {
  const rec = V.resident.get(i)
  if (rec && rec.table && rec.table.done && rec.table.text) return rec.table.text
  let text = ''
  let from = 0
  let total = 0
  for (let guard = 0; guard < 200; guard++) {
    // 越界时宿主回 404，bigTableFetch 解成 null：这里就是取完了
    const part = await bigTableFetch(i, from, from === 0 ? BT_ROWS + 2 : BT_ROWS * 4)
    if (!part || !part.lines.length) break
    total = part.total
    text += part.lines.join('\n') + '\n'
    from += part.lines.length
    if (total && from >= total) break
    // 短读且宿主没给 total：1 MB 窗口装满了，继续往后取，别当成到底
  }
  return text
}

// 打印：整表补齐再交给打印容器。打印不分块——一张表才能让各段列宽一致；
// 打印样式按纸宽折行（见 base.css 的 print 段），横向滚动模式在这里不生效。
async function bigTablePrintHtml(i) {
  const text = await bigTableSource(i)
  const lines = text.split('\n')
  if (lines.length && lines[lines.length - 1] === '') lines.pop()
  if (lines.length < 3) return ''
  const html = bigTableChunkHtml(lines[0] + '\n' + lines[1], lines.slice(2), true)
  return html ? '<div class="bt-print">' + html + '</div>' : ''
}

function regexMatches(text, pattern, caseSensitive) {
  const out = []
  let re
  try {
    re = new RegExp(pattern, 'gmu' + (caseSensitive ? '' : 'i'))
  } catch {
    return out
  }
  let m
  while ((m = re.exec(text)) !== null) {
    out.push({ start: m.index, len: m[0].length })
    if (m[0] === '') re.lastIndex++
    if (out.length >= 500) break
  }
  return out
}

function nearestHit(el, unitOff) {
  const text = el.textContent || ''
  if (FIND.re) {
    const ms = regexMatches(text, FIND.query, FIND.cs)
    if (!ms.length) return null
    let best = ms[0]
    for (const m of ms) {
      if (Math.abs(m.start - unitOff) < Math.abs(best.start - unitOff)) best = m
    }
    return best
  }
  const starts = findOccurrences(text, FIND.query, FIND.cs, FIND.ww)
  if (!starts.length) return null
  let best = starts[0]
  for (const start of starts) {
    if (Math.abs(start - unitOff) < Math.abs(best - unitOff)) best = start
  }
  return { start: best, len: FIND.query.length }
}

// 命中在「块源码」里的区间。字面搜索由后端给的偏移 + 针脚长度决定；正则得自己在源码
// 上跑一遍——后端只给偏移，不给匹配长度。
function sourceMatchSpan(src, unitOff) {
  if (!FIND.re) {
    // 后端给的偏移是块内解码文本的 UTF-16 下标，普通块上就是准的；但巨型块
    // 前端手里只有一段行窗口、代码块还剔掉了围栏行，两边不是同一份文本，按偏移
    // 直接切会切错字。这里列出源码里所有出现位置，挑离偏移最近的那处。
    const len = FIND.query.length
    const starts = findOccurrences(src, FIND.query, FIND.cs, FIND.ww)
    if (!starts.length) return { start: unitOff, len }
    let best = starts[0]
    for (const at of starts) {
      if (Math.abs(at - unitOff) < Math.abs(best - unitOff)) best = at
    }
    return { start: best, len }
  }
  const ms = regexMatches(src, FIND.query, FIND.cs)
  if (!ms.length) return null
  let best = ms[0]
  for (const m of ms) {
    if (Math.abs(m.start - unitOff) < Math.abs(best.start - unitOff)) best = m
  }
  return best
}

// 渲染后的文本是块源码的「子序列」：`#`、`|`、`**`、围栏标记这些语法字符不出现在文本里，
// 所以按字符顺序贪心对齐即可，被丢掉的源码字符自然跳过。
// 空白要单独跳过：生成的 HTML（表格、列表）在标签之间带排版换行与缩进，这些空白在
// textContent 里存在，源码里却没有对应字符，不跳过就会把后面的对齐全带偏。
// 返回 [{ t, s }]：t 是渲染文本下标，s 是该字符在源码里的下标。
function alignSourceToText(src, text) {
  const map = []
  let p = 0
  for (let t = 0; t < text.length; t++) {
    const c = text.charCodeAt(t)
    if (c === 32 || c === 9 || c === 10 || c === 13) continue
    let q = p
    while (q < src.length && src.charCodeAt(q) !== c) q++
    if (q >= src.length) break
    map.push({ t, s: q })
    p = q + 1
  }
  return map
}

// 源码区间 → 渲染文本区间。命中主要落在语法字符上（渲染后只剩零星一两字）时返回 null，
// 交给整块标注：否则表格、标题这类块会被包出半截高亮。
// 源码区间 → 渲染文本区间。
// 先用「命中的那段字面文本」直接定位：它通常原样出现在渲染文本里，而贪心对齐会被源码里
// 不渲染的东西带偏——有序列表的 `2. ` 里那个 2 就让第一个数字对到了列表序号上，
// 高亮从第二个字符开始。只有针脚里含语法字符（`**x**`、表格竖线）时才退回对齐。
function textRangeOfSourceSpan(el, src, from, to) {
  if (!src || to <= from) return null
  const text = el.textContent || ''
  const needle = src.slice(from, to)
  if (needle) {
    const at = []
    let i = text.indexOf(needle)
    while (i >= 0 && at.length < 200) {
      at.push(i)
      i = text.indexOf(needle, i + 1)
    }
    if (at.length === 1) return [at[0], at[0] + needle.length]
    if (at.length > 1) {
      // 同一段出现了多次：用对齐估个大概位置，再挑最近的
      const est = alignmentTextPos(src, text, from)
      let best = at[0]
      for (const p of at) {
        if (Math.abs(p - est) < Math.abs(best - est)) best = p
      }
      return [best, best + needle.length]
    }
  }
  return alignmentRange(src, text, from, to)
}

// 对齐估位：源码下标 from 大致落在渲染文本的哪里（只用于同文本多次出现时的取舍）。
function alignmentTextPos(src, text, from) {
  for (const m of alignSourceToText(src, text)) {
    if (m.s >= from) return m.t
  }
  return text.length
}

function alignmentRange(src, text, from, to) {
  const map = alignSourceToText(src, text)
  let a = -1
  let b = -1
  for (const m of map) {
    if (m.s >= from && m.s < to) {
      if (a < 0) a = m.t
      b = m.t + 1
    }
  }
  if (a < 0) return null
  if ((b - a) * 2 < to - from) return null
  return [a, b]
}

function applyHitMark(el, src, unitOff, center = true) {
  clearHitMarks()
  if (!FIND.query) return
  const span = sourceMatchSpan(src || '', unitOff)
  let node = null
  let range = null
  if (span) range = textRangeOfSourceSpan(el, src, span.start, span.start + span.len)
  if (range) {
    wrapTextRange(el, range[0], range[1])
    node = el.querySelector('mark.hit')
  }
  if (!node) {
    // 源码偏移对不上渲染文本（正则里 JS 编不过的写法、整段命中的是语法字符）：
    // 退回老办法，在渲染文本里找最近的一处。
    const hit = nearestHit(el, unitOff)
    if (hit) {
      wrapTextRange(el, hit.start, hit.start + hit.len)
      node = el.querySelector('mark.hit')
    }
  }
  if (!node) {
    // 命中整个落在语法字符上（例如 `^## ` 的 `## `）：渲染文本里没有对应字符，
    // 包出来只会剩半截。改成标出整块，并说明命中的是行首语法。
    const target = el.firstElementChild || el
    target.classList.add('hit--block')
    node = target
  }
  if (!center) return
  const rect = node.getBoundingClientRect()
  jumpToOffset(V.off + rect.top - V.vh * 0.4)
}

function reapplyCurrentHit() {
  if (!FIND.current || !V.active) return
  const rec = V.resident.get(FIND.current.b)
  if (rec && rec.el && rec.text) applyHitMark(rec.el, rec.text, FIND.current.off, false)
}

function resetFind() {
  closeFind()
  if (findInput) findInput.value = ''
  FIND.query = ''
  FIND.bigHit = null
}

// 标题的渲染文本（去掉 ** / 链接语法与 HTML 标签），slug 按它算而不是按源文本，
// `# **Foo**` 的锚点才和 GitHub 一致。
function headingText(src) {
  let tokens
  try { tokens = md.parseInline(src, {}) } catch { return src }
  const out = []
  const walk = (list) => {
    for (const t of list || []) {
      if (t.type === 'text' || t.type === 'code_inline') out.push(t.content)
      else if (t.type === 'image') out.push(t.content || '')
      else if (t.children) walk(t.children)
    }
  }
  walk(tokens.length ? tokens[0].children : null)
  return out.join('')
}

function slugify(text) {
  return String(text || '')
    .toLowerCase()
    .trim()
    .replace(SLUG_TAG, '')
    .replace(SLUG_DROP, '')
    .replace(/\s/g, '-')
}

function applySlugIds() {
  if (!V.active) return
  for (const [i, rec] of V.resident) {
    if (!rec.el) continue
    const h = rec.el.querySelector('h1,h2,h3,h4,h5,h6')
    if (!h) continue
    const slug = SLUG.byBlock.get(i)
    if (slug) h.id = slug
    else h.removeAttribute('id')
  }
}

function buildSlugs(items) {
  SLUG.bySlug.clear()
  SLUG.byBlock.clear()
  for (const item of items) {
    const base = slugify(headingText(item.text || ''))
    if (!base) continue
    let slug = base
    let n = 1
    while (SLUG.bySlug.has(slug)) slug = base + '-' + (n++)
    SLUG.bySlug.set(slug, item.block)
    SLUG.byBlock.set(item.block, slug)
  }
  report('slugs', SLUG.bySlug.size + '/' + items.length)
  applySlugIds()
}

function loadOutline() {
  if (!V.active) return
  const docId = V.docId
  const seq = ++OUT.seq
  OUT.items = []
  OUT.total = 0
  OUT.truncated = false
  OUT.loaded = false
  OUT.loading = true
  if (outlineEl && !outlineEl.hidden) renderOutline()

  fetch(OUTLINE_URL, { cache: 'no-store' })
    .then((res) => {
      if (!res.ok) throw new Error('HTTP ' + res.status)
      return res.arrayBuffer()
    })
    .then((buf) => {
      if (docId !== V.docId || seq !== OUT.seq || !V.active) return
      if (buf.byteLength < 12) throw new Error('bad outline frame')
      const dv = new DataView(buf)
      const total = dv.getUint32(0, true)
      const count = dv.getUint32(4, true)
      const flags = dv.getUint32(8, true)
      const items = []
      let o = 12
      for (let i = 0; i < count; i++) {
        if (o + 7 > buf.byteLength) break
        const block = dv.getUint32(o, true)
        const level = dv.getUint8(o + 4)
        const len = dv.getUint16(o + 5, true)
        o += 7
        if (o + len > buf.byteLength) break
        items.push({
          block,
          level,
          text: decoder.decode(new Uint8Array(buf, o, len)),
        })
        o += len
      }
      OUT.items = items
      OUT.total = total
      OUT.truncated = (flags & 1) !== 0
      OUT.loaded = true
      OUT.loading = false
      buildSlugs(items)
      if (!outlineEl.hidden) renderOutline()
    })
    .catch((err) => {
      if (docId !== V.docId || seq !== OUT.seq) return
      OUT.loaded = true
      OUT.loading = false
      OUT.items = []
      if (!outlineEl.hidden) renderOutline()
      report('outline-error', String(err))
    })
}

function renderOutline() {
  if (!outlineList) return
  outlineList.textContent = ''
  if (OUT.loading) {
    const note = document.createElement('div')
    note.className = 'outline-empty'
    note.textContent = '正在读取大纲\u2026'
    outlineList.appendChild(note)
    return
  }
  if (!OUT.items.length) {
    const note = document.createElement('div')
    note.className = 'outline-empty'
    note.textContent = '没有标题'
    outlineList.appendChild(note)
    return
  }
  const frag = document.createDocumentFragment()
  for (const item of OUT.items) {
    const btn = document.createElement('button')
    btn.type = 'button'
    btn.className = 'outline-item'
    btn.style.paddingLeft = (10 + (Math.max(1, item.level) - 1) * 12) + 'px'
    btn.textContent = item.text || '（无标题）'
    btn.title = item.text || ''
    btn.dataset.block = String(item.block)
    frag.appendChild(btn)
  }
  outlineList.appendChild(frag)
  if (OUT.truncated) {
    const note = document.createElement('div')
    note.className = 'outline-note'
    note.textContent = '仅显示前 ' + OUT.items.length + ' 条（共 ' + OUT.total + ' 条）'
    outlineList.appendChild(note)
  }
}

function toggleOutline() {
  if (!V.active) return
  if (!outlineEl.hidden) {
    closeOutline()
    return
  }
  outlineEl.hidden = false
  syncTools()
  renderOutline()
  if (!OUT.loaded && !OUT.loading) loadOutline()
}

function closeOutline() {
  if (outlineEl) outlineEl.hidden = true
  syncTools()
}

// 工具条：按设置决定露出哪几个按钮；两个都关掉（或搜索条/大纲开着）就整条收起。
function syncTools() {
  if (!toolsEl) return
  const findBtn = toolsEl.querySelector('[data-act="find"]')
  const outlineBtn = toolsEl.querySelector('[data-act="outline"]')
  if (findBtn) findBtn.hidden = !state.chrome.find
  if (outlineBtn) outlineBtn.hidden = !state.chrome.outline
  const anyBtn = state.chrome.find || state.chrome.outline
  const findOpen = findEl && !findEl.hidden
  const outlineOpen = outlineEl && !outlineEl.hidden
  toolsEl.hidden = !V.active || !anyBtn || findOpen || outlineOpen
}

function onOutlineClick(e) {
  const btn = e.target && e.target.closest ? e.target.closest('.outline-item') : null
  if (!btn) return
  const block = parseInt(btn.dataset.block, 10)
  if (!Number.isInteger(block)) return
  jumpToOffset(pos(block) - V.vh * 0.1)
}

function bindFindUi() {
  bindDom()
  const findBtn = toolsEl && toolsEl.querySelector('[data-act="find"]')
  const outlineBtn = toolsEl && toolsEl.querySelector('[data-act="outline"]')
  if (findBtn) findBtn.addEventListener('click', openFind)
  if (outlineBtn) outlineBtn.addEventListener('click', toggleOutline)
  if (!findEl) return

  findInput.addEventListener('input', scheduleSearch)
  findInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault()
      if (FIND.results.length) goToHit(FIND.index + (e.shiftKey ? -1 : 1))
      return
    }
    if (e.key === 'Escape') {
      e.preventDefault()
      closeFind()
    }
  })
  document.getElementById('find-prev').addEventListener('click', () => {
    if (FIND.results.length) goToHit(FIND.index - 1)
  })
  document.getElementById('find-next').addEventListener('click', () => {
    if (FIND.results.length) goToHit(FIND.index + 1)
  })
  document.getElementById('find-close').addEventListener('click', closeFind)
  findCaseBtn.addEventListener('click', () => {
    FIND.cs = !FIND.cs
    findCaseBtn.classList.toggle('on', FIND.cs)
    if (FIND.query) {
      FIND.busy = true
      updateFindCount()
      runSearch()
    }
  })
  findWordBtn.addEventListener('click', () => {
    FIND.ww = !FIND.ww
    findWordBtn.classList.toggle('on', FIND.ww)
    if (FIND.query) {
      FIND.busy = true
      updateFindCount()
      runSearch()
    }
  })
  findRegexBtn.addEventListener('click', () => {
    FIND.re = !FIND.re
    findRegexBtn.classList.toggle('on', FIND.re)
    findInput.placeholder = FIND.re ? '正则表达式' : '搜索本文档'
    if (FIND.re) {
      FIND.ww = false
      findWordBtn.classList.remove('on')
    }
    findWordBtn.disabled = FIND.re
    if (FIND.query) {
      FIND.busy = true
      FIND.error = ''
      updateFindCount()
      runSearch()
    }
  })
  document.getElementById('outline-close').addEventListener('click', closeOutline)
  outlineList.addEventListener('click', onOutlineClick)
}

function remeasureAll() {
  if (!V.active) return
  for (const [i, rec] of V.resident) {
    if (!rec.el) continue
    // 巨型块的高度是段几何算出来的，巨型表格在补块途中是按平均行高外推的：
    // 拿「当前插了多少」的元素高度覆盖回去，滚动条会骤短、视口内容被推走。
    if (rec.big) continue
    if (rec.table) {
      if (!rec.table.done) continue
      setHeight(i, (rec.table.host || rec.el).offsetHeight)
      continue
    }
    setHeight(i, rec.el.offsetHeight)
  }
  schedulePlace()
}

// 字号 / 行高 / 皮肤变了：巨型块的行高与段几何都要重算，行窗口重新取
function refreshBigLayout() {
  if (!V.active || !V.big.size) return
  big.ready = false
  bigMetrics()
  for (const rec of V.resident.values()) {
    if (rec.big) releaseBig(rec)
  }
  rebuildFenwick()
  schedulePlace()
}

/* ---------------- 渲染开关（设置里的“渲染”页） ---------------- */

// 表格折行模式挂在根节点上：普通表的样式与巨型表的测量读同一个标记，
// 不在两处各自推断。
function syncTableWrapMode() {
  document.body.classList.toggle('mdv-table-nowrap', !state.render.tableWrap)
}

function applyRenderConfig(flags) {
  const next = {
    mermaid: !!flags.mermaid,
    katex: !!flags.katex,
    highlight: !!flags.highlight,
    inlineHtml: !!flags.inlineHtml,
    localImages: !!flags.localImages,
    copyCode: !!flags.copyCode,
    copyTable: !!flags.copyTable,
    tableCopyMd: !!flags.tableCopyMd,
    // 缺失按开启处理：设置页/测试脚本只发关心的项时，不会把折行意外关掉
    tableWrap: flags.tableWrap !== false,
    lineNumbers: !!flags.lineNumbers,
    codeWs: Math.max(0, Math.min(3, flags.codeWs | 0)),
    indentedBlocks: !!flags.indentedBlocks,
  }
  // 只有进块 HTML 的开关需要重渲染；表格复制格式是点击时的事，
  // 表格折行模式只改根节点标记，普通表由 CSS 重排、巨型表重新测量
  const HTML_KEYS = ['mermaid', 'katex', 'highlight', 'inlineHtml', 'localImages',
    'copyCode', 'copyTable', 'lineNumbers', 'codeWs', 'indentedBlocks']
  const changed = HTML_KEYS.some((k) => state.render[k] !== next[k])
  const lnChanged = state.render.lineNumbers !== next.lineNumbers
  const hlChanged = state.render.highlight !== next.highlight
  const wsChanged = state.render.codeWs !== next.codeWs
  const wrapChanged = state.render.tableWrap !== next.tableWrap
  state.render = next
  if (wrapChanged) {
    syncTableWrapMode()
    if (V.active) {
      remeasureAll()
      scheduleBigTableRemeasure()
    }
  }
  // 空白标记重画过，之前点亮的选区标记不再作数
  if (wsChanged) wsClearLit()

  if (md.options.html !== next.inlineHtml) md.set({ html: next.inlineHtml })
  for (const name of MATH_RULES) {
    try {
      if (next.katex) md.enable(name)
      else md.disable(name)
    } catch {
      // 规则缺席（插件版本变化）时忽略
    }
  }

  const chrome = { find: flags.findBtn !== false, outline: flags.outlineBtn !== false }
  if (chrome.find !== state.chrome.find || chrome.outline !== state.chrome.outline) {
    state.chrome = chrome
    syncTools()
  }

  if (changed && V.active) {
    htmlCacheClear()
    rerenderResidents()
  }
  // 巨型块不在 rerenderResidents 的范围内（它按行窗口重画）：行号、高亮、空白符单独补一次
  if ((lnChanged || hlChanged || wsChanged) && V.active) {
    for (const [i, rec] of V.resident) {
      if (!rec.big) continue
      const m = V.big.get(i)
      if (hlChanged) {
        if (m) {
          m.hl = null
          m.hlBusy = false
        }
      }
      for (const seg of rec.big.segs.values()) {
        if (seg.from < 0) continue
        applyBigSeg(seg, i, seg.from, seg.got, seg.plain || '')
      }
      if (hlChanged && next.highlight) scheduleBigHighlight(i)
    }
  }
}

/* ---------------- 皮肤 ---------------- */

function applySkin(css, skinId) {
  const el = document.getElementById('skin')
  el.textContent = css || ''

  const bg = getComputedStyle(document.documentElement).getPropertyValue('--md-bg').trim()
  if (!bg) {
    el.textContent = ''
    report('skin-invalid', skinId)
    return false
  }
  state.skinId = skinId
  // 皮肤换了，后续图表按新配色重画；已经画好的图保留旧配色，等重新渲染时才更新
  if (state.mermaidState === 'ready' && state.mermaidApi) initMermaid(state.mermaidApi)
  if (V.active) refreshBigLayout()
  return true
}

/* ---------------- 字体（#fx，永远最后） ---------------- */

function applyFx(fx) {
  const ui = fx.ui || '"Segoe UI", "Microsoft YaHei UI", system-ui, sans-serif'
  const mono = fx.mono || '"Cascadia Mono", "Consolas", "Courier New", monospace'
  const body = fx.body || ui // 正文未设时继承界面槽位，解析成字面值写出

  const sizePx = Number.isFinite(fx.size) && fx.size > 0 ? fx.size : 15
  const lh = Number.isFinite(fx.lh) && fx.lh > 0 ? fx.lh : 1.65
  // measure 为 0/缺失表示「不限宽」
  const measure = Number.isFinite(fx.measure) && fx.measure > 0 ? fx.measure + 'em' : 'none'

  const size = sizePx + 'px'

  const css = [
    ':root{',
    '--fx-ui:' + ui + ';',
    '--fx-body:' + body + ';',
    '--fx-mono:' + mono + ';',
    '--fx-size:' + size + ';',
    '--fx-lh:' + lh + ';',
    '--fx-measure:' + measure + ';',
    '}',
    // 皮肤可以写 body{font-family:…}，这里用 !important 物理压住
    'body.mdv-root{',
    'font-family:var(--fx-body)!important;',
    'font-size:' + size + '!important;',
    'line-height:' + lh + '!important;',
    '}',
    'code,kbd,samp,pre{font-family:var(--fx-mono)!important;}',
    '.ui{font-family:var(--fx-ui)!important;}',
    '.measure,.blk{max-width:' + measure + '!important;}',
  ].join('')

  document.getElementById('fx').textContent = css

  // 字号/行高变化会改块高，下一帧重新测量常驻窗口
  requestAnimationFrame(() => {
    if (!V.active) return
    remeasureAll()
    refreshBigLayout()
    scheduleBigTableRemeasure()
  })
}

/* ---------------- Mermaid（按需） ---------------- */

function loadMermaid() {
  // 打包成 IIFE 后 window.mermaid 是模块命名空间（{default: api}），
  // 真正可用的对象只在 onload 里解包过一次，必须用缓存的那一个：
  // 直接返回 window.mermaid 会让第二次渲染拿到没有 render 方法的命名空间对象。
  if (state.mermaidState === 'ready' && state.mermaidApi) return Promise.resolve(state.mermaidApi)
  if (state.mermaidState === 'loading') return state.mermaidPromise

  state.mermaidState = 'loading'
  state.mermaidPromise = new Promise((resolve, reject) => {
    const s = document.createElement('script')
    s.src = 'mermaid.js'
    s.async = true
    s.onload = () => {
      const api = window.mermaid && (window.mermaid.default || window.mermaid)
      if (!api) {
        state.mermaidState = 'failed'
        reject(new Error('mermaid global missing'))
        return
      }
      initMermaid(api)
      state.mermaidApi = api
      state.mermaidState = 'ready'
      resolve(api)
    }
    s.onerror = () => {
      state.mermaidState = 'failed'
      reject(new Error('mermaid.js load failed'))
    }
    document.head.appendChild(s)
  })
  return state.mermaidPromise
}

let mermaidSeq = 0
// 每次换文档 +1；异步渲染回来时对不上就丢弃（DOM 已经被整片替换）
let renderGeneration = 0

/* 图表的配色不跟 mermaid 自带的 light/dark 主题走，而是把皮肤变量映射成
   themeVariables —— 否则四套皮肤里只有两套能对上，solarized 一定错色。 */
function mermaidThemeVariables() {
  const root = getComputedStyle(document.documentElement)
  const v = (name) => root.getPropertyValue(name).trim()
  const bg = v('--md-bg')
  const fg = v('--md-fg')
  const muted = v('--md-fg-muted')
  const border = v('--md-border')
  const panel = v('--md-block-bg')
  const alt = v('--md-bg-alt')

  return {
    fontFamily: getComputedStyle(document.body).fontFamily,
    background: bg,
    primaryColor: panel,
    primaryTextColor: fg,
    primaryBorderColor: border,
    secondaryColor: alt,
    tertiaryColor: alt,
    lineColor: muted,
    textColor: fg,
    nodeTextColor: fg,
    titleColor: v('--md-fg-heading'),
    edgeLabelBackground: bg,
    clusterBkg: alt,
    clusterBorder: border,
    // sequenceDiagram
    actorBkg: panel,
    actorBorder: border,
    actorTextColor: fg,
    actorLineColor: border,
    signalColor: fg,
    signalTextColor: fg,
    labelBoxBkgColor: panel,
    labelBoxBorderColor: border,
    loopTextColor: fg,
    noteBkgColor: alt,
    noteTextColor: fg,
    noteBorderColor: border,
  }
}

function initMermaid(api) {
  api.initialize({
    startOnLoad: false,
    securityLevel: 'strict',
    theme: 'base',
    themeVariables: mermaidThemeVariables(),
    // 标签必须用 SVG <text> 而不是 <foreignObject>：后者不在 DOMPurify 的
    // SVG 白名单里，会被整块剥掉，结果就是"有框没字"。两个 htmlLabels 都要写。
    htmlLabels: false,
    flowchart: { htmlLabels: false, useMaxWidth: false },
    sequence: { useMaxWidth: false },
  })
}

// mermaid 会把 %%{init}%% 的 themeVariables 原样写进 SVG 的 <style>：
// 远程 url(...) 能借 CSP 的 img-src 免网络图片确认发起请求，只剥这一类。
// SVG 内部的 url(#marker)/url(#grad) 是箭头与渐变引用，换成 none 会把它们吃掉。
const stripRemoteCssUrl = css =>
  css.replace(/url\s*\(\s*['"]?(?:https?:)?\/\/[^)'"]*['"]?\s*\)/gi, 'none')

function stripSvgStyleUrls(svg) {
  return svg
    .replace(/<style[^>]*>([\s\S]*?)<\/style>/gi, (m, body) => '<style>' + stripRemoteCssUrl(body) + '</style>')
    .replace(/style\s*=\s*"([^"]*)"/gi, (m, css) => 'style="' + stripRemoteCssUrl(css) + '"')
}

async function renderMermaidBlocks(root, generation) {
  const pending = root.querySelectorAll('[data-mermaid="pending"]')
  if (!pending.length) return

  let api
  try {
    api = await loadMermaid()
  } catch {
    // 库缺失：全部保留源码块
    pending.forEach((el) => el.setAttribute('data-mermaid', 'failed'))
    report('mermaid-unavailable')
    return
  }

  for (const el of pending) {
    if (generation !== renderGeneration || !state.render.mermaid) return
    const srcEl = el.querySelector('code')
    const src = srcEl ? srcEl.textContent : ''
    const id = 'mdv-mermaid-' + (++mermaidSeq)

    // mermaid 默认把渲染用的临时节点插到 document.body（预览左上角），量完才移除，
    // 短图会闪一下。给它一个屏外容器，临时节点只活在那里。
    const host = document.createElement('div')
    host.setAttribute('aria-hidden', 'true')
    host.style.cssText =
      'position:fixed;left:-100000px;top:0;overflow:hidden;pointer-events:none'
    host.style.width = Math.max(200, el.clientWidth || 600) + 'px'
    document.body.appendChild(host)

    try {
      const out = await Promise.race([
        api.render(id, src, host),
        new Promise((_, reject) => setTimeout(() => reject(new Error('timeout')), 10000)),
      ])
      if (generation !== renderGeneration || !state.render.mermaid) return
      const svg = DOMPurify.sanitize(out.svg, {
        USE_PROFILES: { svg: true, svgFilters: true },
        // mermaid 把配色与字体写进 SVG 内联 <style>，剥掉就只剩框没有字
        ADD_TAGS: ['style'],
      })
      el.innerHTML = stripSvgStyleUrls(svg)
      el.setAttribute('data-mermaid', 'done')
    } catch (err) {
      // 线上没有控制台，失败原因只能回流到日志
      report('mermaid-error', (err && (err.name + ': ' + err.message)) || String(err))
      try {
        api.parseError && api.parseError(null)
      } catch { /* 忽略清理异常 */ }
      document.querySelectorAll('[id^="dmermaid"], [id^="mdv-mermaid-"]').forEach((n) => {
        if (!n.closest('.mermaid-block')) n.remove()
      })
      el.setAttribute('data-mermaid', 'failed')
    } finally {
      host.remove()
    }
  }
}

/* ---------------- 打印 ----------------
   虚拟滚动下 DOM 里只有视口块，打印前把全文按块渲染进 #print-root，
   @media print 只显示它；对话框由宿主调 ShowPrintUI 打开。 */

function cleanupPrint() {
  if (PRINT.timer) {
    clearTimeout(PRINT.timer)
    PRINT.timer = 0
  }
  if (PRINT.root) {
    PRINT.root.remove()
    PRINT.root = null
  }
  PRINT.busy = false
}

// 已允许的远程图直接放行；不在这里发起新的网络图确认。
// 键必须和记录时一致（host）：拿 origin 去查永远查不到，打印出来就全是占位框。
function allowPrintImages(root) {
  for (const img of root.querySelectorAll('img[data-mdv-src]')) {
    let host = ''
    try { host = new URL(img.dataset.mdvSrc).host } catch { continue }
    if (netAllowedNow(host)) img.src = img.dataset.mdvSrc
  }
}

function waitImages(root) {
  const imgs = [...root.querySelectorAll('img')]
  if (!imgs.length) return Promise.resolve()
  const all = Promise.all(imgs.map((img) => (
    img.decode ? img.decode().catch(() => {}) : Promise.resolve()
  )))
  return Promise.race([all, new Promise((resolve) => setTimeout(resolve, 4000))])
}

async function buildPrintView() {
  const docId = V.docId
  const total = V.blockCount
  const parts = []
  for (let from = 0; from < total; from += PRINT_BATCH) {
    if (docId !== V.docId) throw new Error('文档已切换')
    const count = Math.min(PRINT_BATCH, total - from)
    const res = await fetch(BLOCKS_URL + '?from=' + from + '&count=' + count, { cache: 'no-store' })
    if (!res.ok) throw new Error('HTTP ' + res.status)
    const items = parseFrames(await res.arrayBuffer())
    for (const it of items) {
      if (it.flags & 0x20) continue
      if (V.bigTable.has(it.i)) {
        // 巨型表格的帧只有前 1 MB，打印要整表：按行号取全再分块拼回去
        const html = await bigTablePrintHtml(it.i)
        if (html) {
          parts.push('<div class="pblk">' + html + '</div>')
          continue
        }
      }
      let html = renderBlockHtml(it.text, it.flags, it.i)
      if (it.i === V.lastVisible && FOOT.html) html += FOOT.html
      if (it.flags & 0x80) html += '<p>（块过大，打印时已截断）</p>'
      parts.push('<div class="pblk">' + html + '</div>')
    }
  }
  if (docId !== V.docId) throw new Error('文档已切换')

  const root = document.createElement('div')
  root.id = 'print-root'
  root.innerHTML = parts.join('')
  document.body.appendChild(root)
  allowPrintImages(root)
  // 图表只有渲染成 SVG 才能打印；源码块原样保留
  await renderMermaidBlocks(root, state.generation)
  await waitImages(root)
  return root
}

async function startPrint() {
  if (!V.active) {
    postRaw('status|没有可打印的文档')
    return
  }
  if (PRINT.busy) return
  if (V.fileSize > PRINT_MAX_FILE) {
    postRaw('status|文档超过 2 MB，暂不支持打印')
    return
  }
  PRINT.busy = true
  PRINT.docId = V.docId
  postRaw('status|正在准备打印…')
  const t0 = performance.now()
  try {
    const root = await buildPrintView()
    if (PRINT.docId !== V.docId || !V.active) {
      root.remove()
      PRINT.busy = false
      return
    }
    PRINT.root = root
    const n = root.querySelectorAll('.pblk').length
    report('print-ready', (performance.now() - t0).toFixed(0) + '|' + n)
    postRaw('print-ready')
    // 正常会收到 afterprint；收不到时 5 分钟后兜底清掉，别把全文 DOM 留在页面里
    PRINT.timer = setTimeout(cleanupPrint, 5 * 60 * 1000)
  } catch (err) {
    cleanupPrint()
    const msg = String((err && err.message) || err)
    postRaw('status|打印准备失败：' + msg)
    report('print-error', msg)
  }
}

/* ---------------- 整篇渲染（只在 Edge 开发模式使用） ---------------- */

function renderDocument(source, docBase) {
  document.body.classList.add('dev')
  document.body.classList.remove('virtual')
  const t0 = performance.now()
  state.generation = ++renderGeneration

  if (docBase) state.docBase = docBase

  const body = stripFrontMatter(source)
  state.docText = body
  const html = sanitize(md.render(body, { docBase: state.docBase, docText: body }))
  docEl.innerHTML = html
  processNetImages(docEl)

  const ms = performance.now() - t0
  renderMermaidBlocks(docEl, state.generation)

  const box = docEl.getBoundingClientRect()
  const metrics = 'vp=' + window.innerWidth + 'x' + window.innerHeight
    + ' dpr=' + (window.devicePixelRatio || 1).toFixed(2)
    + ' col=' + Math.round(box.width) + '@' + Math.round(box.left)

  report('rendered', ms.toFixed(1) + '|' + source.length + '|' + metrics)
  return ms
}

/* ---------------- 宿主通道 ----------------
   控制帧格式：<type>|<字段…>，小载荷；块正文只走 mdblocks.localhost 的二进制帧。 */

const violations = []
window.__mdvCspViolations = violations

document.addEventListener('securitypolicyviolation', (e) => {
  violations.push(e.violatedDirective + ' ' + (e.blockedURI || '-'))
  document.documentElement.dataset.cspViolations = String(violations.length)
  // 线上没有开发者工具，违规只能靠日志看见
  report('csp', e.violatedDirective + ' ' + (e.blockedURI || '-'))
})

function postRaw(text) {
  const host = window.chrome && window.chrome.webview
  if (host) host.postMessage(text)
}

function report(kind, detail) {
  postRaw('event|' + kind + '|' + (detail == null ? '' : detail))
}

function splitFirst(text) {
  const at = text.indexOf('|')
  if (at < 0) return [text, '']
  return [text.slice(0, at), text.slice(at + 1)]
}

function onHostMessage(event) {
  const raw = event.data
  if (typeof raw !== 'string') return

  const [type, rest] = splitFirst(raw)

  if (type === 'index') {
    const f = rest.split('|')
    applyIndex(f[0],
      parseInt(f[1], 10) || 0,
      parseInt(f[2], 10) || 0,
      parseInt(f[3], 10) || 0,
      parseInt(f[4], 10) || 0,
      parseInt(f[5], 10) || 0,
      f[6] === '1',
      f[7] || '',   // 文档键：路径*大小*写入时间，网络图答复按它记
      f[8] || '')   // 会话 id：换了它说明 DO 重启过，旧记忆整份作废
    return
  }

  if (type === 'index-full') {
    const f = rest.split('|')
    applyIndexFull(parseInt(f[0], 10) || 0, parseInt(f[1], 10) || 0)
    return
  }

  if (type === 'selectall') {
    selectAllText()
    return
  }

  if (type === 'copy') {
    // Opus 菜单的「复制」：有选区就只复制选区，没有就整篇渲染文本
    const picked = String(window.getSelection() || '')
    if (picked) postRaw('copyset|0|' + picked)
    else copyRendered()
    return
  }

  if (type === 'print') {
    startPrint()
    return
  }

  if (type === 'big') {
    applyBigTable(rest)
    return
  }

  if (type === 'clear') {
    closeZoomMask()
    resetVirtual()
    return
  }

  if (type === 'zoom') {
    // 宿主右键菜单的「放大查看」：开的是 imgctx 时刻记下的那张图。
    // 换过文档后 URL 的语义不再成立（mdblocks 按当前文档目录解析），丢掉。
    const p = IMGZ.pending
    IMGZ.pending = null
    if (p && p.url && p.docId === V.docId) openZoomMask(p.url)
    return
  }

  if (type === 'net') {
    const mode = parseInt(rest, 10)
    setNetMode(Number.isInteger(mode) ? mode : 1)
    return
  }

  if (type === 'linkwait') {
    // 1 = 弹窗后等 3 秒；0 = 只弹窗、直接可点。只影响下一次弹窗。
    OPEN.wait = rest === '0' ? 0 : kOpenConfirmDelay
    return
  }

  if (type === 'cfg') {
    const f = rest.split('|')
    // 后加的项：老宿主不发时按「全开、TSV、表格折行」处理
    const flag = (i) => (f[i] === undefined ? true : f[i] === '1')
    applyRenderConfig({
      mermaid: f[0] === '1',
      katex: f[1] === '1',
      highlight: f[2] === '1',
      inlineHtml: f[3] === '1',
      localImages: f[4] === '1',
      copyCode: flag(5),
      copyTable: flag(6),
      tableCopyMd: f[7] === '1',
      findBtn: flag(8),
      outlineBtn: flag(9),
      lineNumbers: flag(10),
      // 第 12 个字段是空白符档位（0 关 / 1 仅选中 / 2 边界 / 3 全部）；老宿主没有这一项，按关处理
      codeWs: f[11] === undefined ? 0 : parseInt(f[11], 10) || 0,
      // 老宿主不发这一项：按关处理，与 state.render 的默认值一致
      indentedBlocks: f[12] === '1',
      // 老宿主不发这一项：按开处理，升级后观感不变
      tableWrap: flag(13),
    })
    return
  }

  if (type === 'skin') {
    const [id, css] = splitFirst(rest)
    applySkin(css, id || 'light')
    return
  }

  if (type === 'fx') {
    const f = rest.split('|') // ui|body|mono|size|lineHeight|measureEm
    applyFx({
      ui: f[0],
      body: f[1],
      mono: f[2],
      size: parseFloat(f[3]),
      lh: parseFloat(f[4]),
      measure: parseInt(f[5], 10),
    })
  }
}

/* ---------------- 外部链接确认 ----------------
   链接可能指向本地程序，交给宿主前必须人工过目：页面内弹窗；「打开」要不要等
   3 秒由设置决定（linkwait 帧），等待只是把按钮填满再放开，弹窗本身没有关闭开关。
   href 只经 textContent 写入，不参与 HTML 拼接。 */
const kOpenConfirmDelay = 3
const OPEN = { wait: kOpenConfirmDelay }  // 秒；0 = 不等，弹窗一出现就能点

const kWarnIcon =
  '<svg class="mdv-modal-ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
  'stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
  '<path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z"/>' +
  '<path d="M12 9v4M12 17h.01"/></svg>'

function confirmOpenExternal(href, target) {
  const sendTo = target || href
  if (document.querySelector('.mdv-modal-overlay')) return
  const prevFocus = document.activeElement
  const overlay = document.createElement('div')
  overlay.className = 'mdv-modal-overlay'
  overlay.innerHTML =
    '<div class="mdv-modal" role="dialog" aria-modal="true">' +
    '<div class="mdv-modal-head">' + kWarnIcon +
    '<h2 class="mdv-modal-title"></h2></div>' +
    '<p class="mdv-modal-warn"></p>' +
    '<div class="mdv-modal-url"></div>' +
    '<div class="mdv-modal-actions">' +
    '<button type="button" class="mdv-cancel"></button>' +
    '<button type="button" class="mdv-open" disabled>' +
    '<span class="mdv-fill"></span><span class="mdv-txt"></span></button>' +
    '</div></div>'
  const modal = overlay.querySelector('.mdv-modal')
  modal.querySelector('.mdv-modal-title').textContent = '打开外部链接'
  modal.querySelector('.mdv-modal-warn').textContent =
    '此链接会交给系统打开，可能是程序或脚本。请确认来源可信。'
  modal.querySelector('.mdv-modal-url').textContent = href
  const btnCancel = modal.querySelector('.mdv-cancel')
  const btnOpen = modal.querySelector('.mdv-open')
  const fill = modal.querySelector('.mdv-fill')
  const label = modal.querySelector('.mdv-txt')
  btnCancel.textContent = '取消'

  let timer = 0
  const close = (open) => {
    if (timer) clearInterval(timer)
    timer = 0
    overlay.remove()
    if (prevFocus && prevFocus.isConnected) prevFocus.focus()
    if (open) postRaw('open|' + sendTo)
  }

  // 等待的秒数由宿主下发；0 就是「弹窗照旧、直接可点」。进度用按钮自身的填充表达，
  // 长满那一刻才放开按钮，比单纯换数字更能看出还要多久。
  const wait = OPEN.wait > 0 ? OPEN.wait : 0
  if (wait > 0) {
    const total = wait * 1000
    const t0 = performance.now()
    label.textContent = wait + ' 秒后可打开'
    timer = setInterval(() => {
      const p = Math.min(1, (performance.now() - t0) / total)
      fill.style.width = (p * 100).toFixed(2) + '%'
      if (p < 1) {
        label.textContent = Math.max(1, Math.ceil(wait * (1 - p))) + ' 秒后可打开'
        return
      }
      clearInterval(timer)
      timer = 0
      fill.style.width = '100%'
      label.textContent = '打开'
      btnOpen.disabled = false
    }, 16)
  } else {
    label.textContent = '打开'
    btnOpen.disabled = false
  }

  btnCancel.addEventListener('click', () => close(false))
  btnOpen.addEventListener('click', () => {
    if (!btnOpen.disabled) close(true)
  })
  overlay.addEventListener('click', (ev) => {
    if (ev.target === overlay) close(false)
  })
  overlay.addEventListener('keydown', (ev) => {
    if (ev.key === 'Escape') {
      ev.preventDefault()
      close(false)
      return
    }
    if (ev.key === 'Tab') {
      // 焦点只在两个按钮间循环，别落到弹层后面的文档上
      const focusables = [btnCancel, btnOpen]
      const i = focusables.indexOf(document.activeElement)
      const next = ev.shiftKey ? (i <= 0 ? 1 : i - 1) : (i === 0 ? 1 : 0)
      ev.preventDefault()
      focusables[next].focus()
    }
  })

  document.body.appendChild(overlay)
  btnCancel.focus()
}

document.addEventListener('click', (e) => {
  const a = e.target && e.target.closest ? e.target.closest('a') : null
  if (!a) return
  e.preventDefault()
  // 不能用 data-external：纯 Markdown 块不经过 DOMPurify（sanitizeBlock 对不含 "<"
  // 的块直接返回），那个属性只在含内联 HTML 的块里有。一律现场解析，绝对地址
  // 与 mailto: 之类会被 resolveDocUrl 原样返回。
  const raw = a.getAttribute('href')
  if (!raw) return
  if (raw.charAt(0) === '#') {
    if (!V.active) return
    const frag = raw.slice(1)
    // 脚注角标跳到文末脚注区；反向链接没有块号映射，先不处理
    if (/^fn\d/.test(frag)) {
      V.target = Math.max(0, V.totalH - V.vh)
      kick()
      return
    }
    if (/^fnref\d/.test(frag)) return
    let slug = frag
    try { slug = decodeURIComponent(frag) } catch { /* 保留原文 */ }
    const block = SLUG.bySlug.get(slug)
    if (block !== undefined) {
      jumpToOffset(pos(block) - V.vh * 0.1)
      flashBlock(block)
    } else {
      report('anchor-missing', slug)
    }
    return
  }
  // 确认框里显示用户写的原始地址（相对路径比虚拟主机地址好认），发回宿主的是
  // 解析成绝对地址的那份：宿主按 https://mddoc.localhost/ 前缀还原磁盘路径。
  confirmOpenExternal(raw, resolveDocUrl(raw))
}, true)

/* ---------------- 开发模式（只在 Edge 里用，线上走 postMessage） ---------------- */

async function devBoot() {
  bindDom()
  document.body.classList.add('dev')
  const params = new URLSearchParams(location.search)
  const skin = params.get('skin')
  if (skin) {
    try {
      const res = await fetch('skins/' + skin + '.css')
      if (res.ok) applySkin(await res.text(), skin)
    } catch { /* 皮肤缺席时留在默认层 */ }
  }

  const doc = params.get('doc')
  if (!doc) return
  const res = await fetch(doc)
  if (!res.ok) {
    docEl.textContent = 'cannot load ' + doc
    return
  }
  const text = await res.text()
  renderDocument(text, new URL('.', res.url).href)
}

function bindInput() {
  bindDom()
  bindFindUi()
  window.addEventListener('wheel', onWheel, { passive: false })
  window.addEventListener('keydown', onKeyDown)
  window.addEventListener('resize', () => {
    if (!V.active) return
    V.vh = window.innerHeight
    V.trackH = 0
    place()
    // 分块表的列宽是按可用宽度量的 px，拖完要重量（防抖在里层）
    scheduleBigTableRemeasure()
  })
  thumbEl.addEventListener('pointerdown', onThumbDown)
  thumbEl.addEventListener('pointermove', onThumbMove)
  thumbEl.addEventListener('pointerup', onThumbUp)
  thumbEl.addEventListener('pointercancel', onThumbUp)
  sbEl.addEventListener('pointerdown', onTrackDown)
  document.addEventListener('selectionchange', () => {
    refreshPins()
    // 宿主会用 DVPLUGINMSG_TESTSELECTION 问「有没有选区」，据此决定「复制」是否可用。
    const sel = window.getSelection()
    const on = !!(sel && sel.rangeCount && !sel.getRangeAt(0).collapsed)
    if (on !== SEL.reported) {
      SEL.reported = on
      postRaw('sel|' + (on ? '1' : '0'))
    }
    // 边界档：选区里的空白按「全部」显示
    wsScheduleSelection()
  })
  document.addEventListener('mousedown', onSelDown)
  document.addEventListener('mousemove', onSelMove)
  document.addEventListener('mouseup', onSelUp)
  window.addEventListener('blur', onSelUp)
  if (document.fonts && document.fonts.ready) {
    document.fonts.ready.then(() => {
      if (V.active) remeasureAll()
    })
  }
}

window.addEventListener('error', (e) => {
  report('js-error', (e.message || 'error') + ' @' + (e.filename || '') + ':' + (e.lineno || 0))
})
window.addEventListener('afterprint', () => {
  cleanupPrint()
  postRaw('print-done')
})

window.addEventListener('unhandledrejection', (e) => {
  report('js-reject', String(e.reason && e.reason.stack ? e.reason.stack : e.reason))
})

bindInput()

if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener('message', onHostMessage)
  const resources = performance.getEntriesByType ? performance.getEntriesByType('resource') : []
  const appScript = resources.find((r) => r.name.endsWith('/app.js'))
  postRaw(
    'ready|js=' + Math.round(performance.now()) +
      (appScript ? '|appjs=' + Math.round(appScript.duration) : '')
  )
  document.body.classList.add('virtual')
} else {
  // 开发模式没有宿主推档位，直接放行（远程资源仍受 CSP 约束）
  NET.mode = 0
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', devBoot)
  } else {
    devBoot()
  }
}

window.__mdv = { md, renderDocument, applySkin, applyFx, applyRenderConfig, state, resolveDocUrl,
  DOC_HOST, V, SLUG }

// 只给 hosttest 用：准备打印容器并回流规模，不弹对话框
window.__mdv.printTest = () => buildPrintView().then((root) => {
  const out = {
    blocks: root.querySelectorAll('.pblk').length,
    chars: root.textContent.length,
    mermaid: root.querySelectorAll('[data-mermaid="done"]').length,
    pending: root.querySelectorAll('[data-mermaid="pending"]').length,
    imgs: root.querySelectorAll('img').length,
  }
  root.remove()
  out.leftover = document.getElementById('print-root') !== null
  return JSON.stringify(out)
}).catch((e) => 'err=' + ((e && e.message) || e))
