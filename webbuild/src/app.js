import MarkdownIt from 'markdown-it'
import deflist from 'markdown-it-deflist'
import taskLists from 'markdown-it-task-lists'
import sub from 'markdown-it-sub'
import sup from 'markdown-it-sup'
import mark from 'markdown-it-mark'
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
   - 单块高亮闸门：≤2000 行且 ≤256 KiB，超限只出纯文本。
   ============================================================ */

const HIGHLIGHT_MAX_LINES = 2000
const HIGHLIGHT_MAX_BYTES = 256 * 1024
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

function utf8Bytes(text) {
  // 只用于高亮闸门判定，TextEncoder 的精确计数不是必需
  return new TextEncoder().encode(text).length
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
  const tooHuge = lines > HIGHLIGHT_MAX_LINES || bytes > HIGHLIGHT_MAX_BYTES

  let body
  if (lang && state.render.highlight && !tooHuge && hljs.getLanguage(lang)) {
    body = hljs.highlight(code, { language: lang, ignoreIllegals: true }).value
  } else {
    body = escapeHtml(code)
  }

  const cls = 'language-' + (lang || 'none')
  const note = tooHuge
    ? '<div class="code-note">代码块过大（' + lines + ' 行 / '
      + Math.round(bytes / 1024) + ' KiB），已关闭高亮</div>'
    : ''

  return '<pre>' + note + '<code class="hljs ' + cls + '">' + body + '</code></pre>\n'
}

/* ---------------- 清洗与 URL 改写 ---------------- */

const TRANSPARENT_IMG = 'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7'

// 网络图片的确认状态（每文档重置）。确认面板画在预览页里，不再弹原生窗。
const NET = {
  mode: 1,             // 0 不询问 / 1 每主机每文档 / 2 每文档
  allowed: new Set(),
  denied: new Set(),
  docDecided: null,
  queue: [],
  current: null,
  panel: null,
  title: null,
}

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
    // 本地图片开关关闭：data/blob 属于文档自身，仍保留；网络图交给下面的确认流程
    if (!state.render.localImages && !/^(?:data|blob):/i.test(src) && !isRemoteUrl(src)) {
      node.removeAttribute('src')
      node.setAttribute('title', '本地图片已关闭')
      return
    }
    // 绝对路径/../ 已被上面的 hook 换成本地 __asset，不要再走网络图确认
    if (/^https:\/\/mdblocks\.localhost\//i.test(src)) return
    if (isRemoteUrl(src)) {
      let absolute = null
      try { absolute = new URL(src, location.href) } catch { absolute = null }
      if (absolute && absolute.protocol === 'https:') {
        // 先放透明占位，宿主确认后再设真 src；CSP 只放行 https:
        node.setAttribute('data-mdv-src', absolute.href)
        node.setAttribute('src', TRANSPARENT_IMG)
      } else {
        node.removeAttribute('src')  // http / 其他协议：不发起请求，只留 alt
      }
      return
    }
    // 不设 loading=lazy：块级虚拟化已经只保留视口附近的块，原生懒加载在
    // translate3d 的虚拟滚动里判定不准，会让图片一直不发起请求。
    node.setAttribute('src', resolveImageUrl(src))
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

function resolveImageUrl(raw) {
  if (!raw) return raw
  if (/^(?:data|blob):/i.test(raw)) return raw
  if (/^\/\//.test(raw)) return raw  // 协议相对 URL（https: 由清洗钩子接管）
  // Windows 盘符绝对路径；markdown-it 会把反斜杠编码成 %5C，先解回来
  if (/^[a-zA-Z]:[\\/%]/.test(raw)) return assetUrl(decodePath(raw))
  const decoded = decodePath(raw)
  if (/^[a-zA-Z]:[\\/]/.test(decoded)) return assetUrl(decoded)
  if (/^\\\\/.test(decoded)) return assetUrl(decoded)  // UNC
  // 其它协议（mailto 等）原样交给浏览器
  if (/^[a-z][a-z0-9+.-]*:/i.test(raw)) return raw
  return assetUrl(raw)  // 文档目录内（含 ../，宿主决定是否放行）
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
    ADD_ATTR: ['data-mermaid', 'data-external', 'data-state'],
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
  },
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
  resident: new Map(),
  pool: [],
  bigPool: [],
  pinned: new Set(),     // 选区覆盖、暂时不换出的块号
  pinsDirty: false,      // 钉块集合缩小过：即使窗口没变也要回收一次
  big: new Map(),        // 块号 -> { lines }（>2000 行的块）
  bigPending: null,      // 页面还没就绪时收到的巨型块表
  pending: new Set(),
  fetching: new Set(),
  queue: [],
  pumpRaf: 0,
  placeRaf: 0,
  raf: 0,
  lastTick: 0,
  lastWheelAt: 0,
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
  V.totalH += delta
  V.topsDirty = true
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

function renderBlockHtml(text, flags, i) {
  try {
    const env = blockEnv()
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
      return sanitize(md.renderer.render(tokens, md.options, env))
    }
    return sanitize(md.render(text, env))
  } catch (err) {
    report('render-error', String(err))
    return '<pre>' + escapeHtml(text) + '</pre>'
  }
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
      if (!rec || rec.el !== entry.target) continue
      setHeight(i, entry.target.offsetHeight)
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

function showNetPrompt(origin) {
  const panel = ensureConsentPanel()
  let host = origin
  try { host = new URL(origin).host } catch { /* 保留原文 */ }
  NET.title.textContent = '允许加载来自 ' + host + ' 的网络图片吗？'
  panel.hidden = false
}

function hideNetPrompt() {
  if (NET.panel) NET.panel.hidden = true
}

function ensureNetConsent(origin) {
  if (NET.mode === 0) return Promise.resolve(true)
  if (NET.mode === 2) {
    if (NET.docDecided !== null) return Promise.resolve(NET.docDecided)
  } else {
    if (NET.allowed.has(origin)) return Promise.resolve(true)
    if (NET.denied.has(origin)) return Promise.resolve(false)
  }
  return new Promise((resolve) => {
    NET.queue.push({ origin, resolve })
    pumpNetConsent()
  })
}

// 一次只问一个来源；拒绝/允许只作用于当前来源（每文档档位除外）
function pumpNetConsent() {
  if (NET.current) return
  while (NET.queue.length) {
    const item = NET.queue[0]
    if (NET.mode === 0) {
      NET.queue.shift()
      item.resolve(true)
      continue
    }
    if (NET.mode === 2 && NET.docDecided !== null) {
      NET.queue.shift()
      item.resolve(NET.docDecided)
      continue
    }
    if (NET.mode === 1 && NET.allowed.has(item.origin)) {
      NET.queue.shift()
      item.resolve(true)
      continue
    }
    if (NET.mode === 1 && NET.denied.has(item.origin)) {
      NET.queue.shift()
      item.resolve(false)
      continue
    }
    NET.queue.shift()
    NET.current = item
    showNetPrompt(item.origin)
    return
  }
}

function decideNetConsent(allow) {
  const item = NET.current
  if (!item) return
  NET.current = null
  if (NET.mode === 2) {
    NET.docDecided = allow
  } else if (NET.mode === 1) {
    if (allow) NET.allowed.add(item.origin)
    else NET.denied.add(item.origin)
  }
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

function resetNetConsent() {
  // 只清“正在等答复”的问答与每文档决定；允许/拒绝的来源记住到页面结束
  // （页面随 DO 进程存活，关预览窗口、换文件都不清）。
  for (const item of NET.queue) item.resolve(false)
  if (NET.current) NET.current.resolve(false)
  NET.queue = []
  NET.current = null
  NET.docDecided = null
  hideNetPrompt()
}

function processNetImages(root) {
  const imgs = root.querySelectorAll('img[data-mdv-src]')
  for (const img of imgs) {
    if (img.dataset.mdvNet === '1') continue
    img.dataset.mdvNet = '1'
    let origin = ''
    try { origin = new URL(img.dataset.mdvSrc).origin } catch { continue }
    ensureNetConsent(origin).then((allow) => {
      if (!img.isConnected) return
      if (allow) {
        img.src = img.dataset.mdvSrc
      } else {
        img.removeAttribute('src')
        img.title = '网络图片未加载'
      }
    })
  }
}

function renderItem(item) {
  const i = item.i
  if (!V.active || i >= V.blockCount || item.docId !== V.docId) return

  if (item.flags & 0x20) {  // hidden：front matter / 链接与脚注定义
    V.hidden[i] = 1
    setHeight(i, 0)
    return
  }
  if (V.hidden[i]) return
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
  V.topsDirty = true
  rec.text = item.text
  rec.flags = item.flags
  let html = renderBlockHtml(item.text, item.flags, i)
  // 脚注区挂在最后一个块上：没有独立块号，也不参与索引
  if (i === V.lastVisible && FOOT.html) html += FOOT.html
  rec.el.innerHTML = html
  processNetImages(rec.el)
  setHeight(i, rec.el.offsetHeight)
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
  el.innerHTML = '<div class="big-frame"><pre class="big-pre"><code></code></pre></div>'
  const frame = el.firstElementChild
  return { el, pre: frame.firstElementChild, code: frame.firstElementChild.firstElementChild }
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
      el: item.el, pre: item.pre, code: item.code,
      from: -1, got: 0, want: null, loading: false, shortAt: -1,
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
      const g = bigMetrics()
      curSeg.code.textContent = decoder.decode(new Uint8Array(buf, 20, len))
      curSeg.pre.style.transform =
        'translateY(' + ((from - k * BIG_SEG_LINES) * g.lineH).toFixed(1) + 'px)'
      curSeg.from = from
      curSeg.got = got
      if (got < count) curSeg.shortAt = from
      if (total > 0) updateBigLines(i, total)
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
  V.big.set(i, { lines })
  const rec = V.resident.get(i)
  if (rec && rec.big) releaseBig(rec)
  rebuildFenwick()
  schedulePlace()
}

function applyBigTable(text) {
  const map = new Map()
  if (text) {
    for (const part of text.split(',')) {
      const cut = part.indexOf(':')
      if (cut <= 0) continue
      const i = parseInt(part.slice(0, cut), 10)
      const lines = parseInt(part.slice(cut + 1), 10)
      if (Number.isInteger(i) && i >= 0 && Number.isInteger(lines) && lines > 0) {
        map.set(i, { lines })
      }
    }
  }
  V.bigPending = map
  if (!V.active) return
  V.bigPending = null
  V.big = map
  bigMetrics()

  for (const [i, rec] of [...V.resident]) {
    if (map.has(i)) {
      ensureBigResident(i)
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
    if (rec.big) {
      releaseBig(rec)
    } else {
      if (resizeObserver) resizeObserver.unobserve(rec.el)
      rec.el.innerHTML = ''
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
        if (it.i >= V.windowStart && it.i <= V.windowEnd) V.queue.push(it)
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
  let n = 0
  while (V.queue.length && n < 10) {
    const item = V.queue.shift()
    renderItem(item)
    V.pending.delete(item.i)
    n++
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
    V.stageBase = anchor
    recycleOutside(a, b)
    V.pinsDirty = false
    // 跳跃滚动时队列里会攒下大量已经过时的窗口块，别让它们挡住当前窗口
    if (V.queue.length) {
      V.queue = V.queue.filter((it) => it.i >= a && it.i <= b)
    }
    V.topsDirty = true
  } else if (Math.abs(anchor - V.stageBase) > V.vh * 4) {
    V.stageBase = anchor
    V.topsDirty = true
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
    for (const [i, rec] of V.resident) {
      if (rec.el) rec.el.style.top = (pos(i) - V.stageBase) + 'px'
    }
    V.topsDirty = false
  }
  updateBigWindows()
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
  const trackH = sbEl.clientHeight
  if (trackH <= 0) return
  const thumbH = Math.max(28, Math.min(trackH, (V.vh / V.totalH) * trackH))
  const maxTop = Math.max(0, trackH - thumbH)
  const top = scrollable > 0 ? (V.off / scrollable) * maxTop : 0
  thumbEl.style.height = thumbH.toFixed(1) + 'px'
  thumbEl.style.transform = 'translateY(' + top.toFixed(1) + 'px)'
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
  resetFind()
  OUT.seq++
  OUT.items = []
  OUT.total = 0
  OUT.truncated = false
  OUT.loaded = false
  OUT.loading = false
  if (outlineEl) outlineEl.hidden = true
  if (toolsEl) toolsEl.hidden = true

  V.docId++
  V.active = false
  if (V.pumpRaf) cancelAnimationFrame(V.pumpRaf)
  if (V.placeRaf) cancelAnimationFrame(V.placeRaf)
  if (V.raf) cancelAnimationFrame(V.raf)
  V.pumpRaf = 0
  V.placeRaf = 0
  V.raf = 0
  V.queue.length = 0
  V.pending.clear()
  V.fetching.clear()
  if (resizeObserver) resizeObserver.disconnect()
  for (const rec of V.resident.values()) {
    if (rec.big) {
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
  SEL.all = false
  onSelUp()
  cleanupPrint()
  resetNetConsent()
}

function applyIndex(base, prefix, est, fileSize, enc, lastVisible, complete) {
  resetVirtual()
  bindDom()
  document.body.classList.add('virtual')
  document.body.classList.remove('dev')

  state.docBase = base || state.docBase
  state.generation = ++renderGeneration

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
    V.big = V.bigPending
    V.bigPending = null
    bigMetrics()
  }

  if (toolsEl) toolsEl.hidden = false
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
    e.preventDefault()
    openFind()
    return
  }
  if (e.key === 'Escape') {
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
  if (toolsEl) toolsEl.hidden = true
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
  if (V.active && toolsEl && outlineEl && outlineEl.hidden) toolsEl.hidden = false
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
  if (!V.big.has(hit.b)) jumpToOffset(pos(hit.b) - V.vh * 0.15)
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
  const rec = V.resident.get(want.b)
  if (!rec || !rec.big) return
  const seg = rec.big.segs.get(segIndexOfLine(want.b, want.line))
  if (!seg || seg.from < 0) return
  const rel = want.line - seg.from
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
    // 不从后端偏移直接切：有序列表这类块里它实测会差 1（`2. 2026-…` 报了 4、实际 3），
    // 切出来的针脚多一个字就整段找不着。改在源码里列出所有出现位置、挑离偏移最近的。
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
  if (toolsEl) toolsEl.hidden = true
  renderOutline()
  if (!OUT.loaded && !OUT.loading) loadOutline()
}

function closeOutline() {
  if (outlineEl) outlineEl.hidden = true
  if (V.active && toolsEl && (!findEl || findEl.hidden)) toolsEl.hidden = false
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
    if (rec.el) setHeight(i, rec.el.offsetHeight)
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

function applyRenderConfig(flags) {
  const next = {
    mermaid: !!flags.mermaid,
    katex: !!flags.katex,
    highlight: !!flags.highlight,
    inlineHtml: !!flags.inlineHtml,
    localImages: !!flags.localImages,
  }
  const changed = Object.keys(next).some((k) => state.render[k] !== next[k])
  state.render = next

  if (md.options.html !== next.inlineHtml) md.set({ html: next.inlineHtml })
  for (const name of MATH_RULES) {
    try {
      if (next.katex) md.enable(name)
      else md.disable(name)
    } catch {
      // 规则缺席（插件版本变化）时忽略
    }
  }
  if (changed && V.active) rerenderResidents()
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
// url(...) 能借 CSP 的 img-src 免网络图片确认发起远程请求，一律剥掉。
function stripSvgStyleUrls(svg) {
  return svg
    .replace(/<style([\s\S]*?)<\/style>/gi, (m, body) => '<style>' + body.replace(/url\s*\([^)]*\)/gi, 'none') + '</style>')
    .replace(/style\s*=\s*"([^"]*)"/gi, (m, css) => 'style="' + css.replace(/url\s*\([^)]*\)/gi, 'none') + '"')
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
function allowPrintImages(root) {
  for (const img of root.querySelectorAll('img[data-mdv-src]')) {
    let origin = ''
    try { origin = new URL(img.dataset.mdvSrc).origin } catch { continue }
    const allowed = NET.mode === 0 ||
      (NET.mode === 1 && NET.allowed.has(origin)) ||
      (NET.mode === 2 && NET.docDecided === true)
    if (allowed) img.src = img.dataset.mdvSrc
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
  const html = sanitize(md.render(body, { docBase: state.docBase }))
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
  const pos = text.indexOf('|')
  if (pos < 0) return [text, '']
  return [text.slice(0, pos), text.slice(pos + 1)]
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
      f[6] === '1')
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
    resetVirtual()
    return
  }

  if (type === 'net') {
    const mode = parseInt(rest, 10)
    setNetMode(Number.isInteger(mode) ? mode : 1)
    return
  }

  if (type === 'cfg') {
    const f = rest.split('|')
    applyRenderConfig({
      mermaid: f[0] === '1',
      katex: f[1] === '1',
      highlight: f[2] === '1',
      inlineHtml: f[3] === '1',
      localImages: f[4] === '1',
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
   链接可能指向本地程序，交给宿主前必须人工过目：页面内弹窗 + 3 秒倒计时。
   href 只经 textContent 写入，不参与 HTML 拼接。 */
const kOpenConfirmDelay = 3

function confirmOpenExternal(href) {
  if (document.querySelector('.mdv-modal-overlay')) return
  const prevFocus = document.activeElement
  const overlay = document.createElement('div')
  overlay.className = 'mdv-modal-overlay'
  overlay.innerHTML =
    '<div class="mdv-modal" role="dialog" aria-modal="true">' +
    '<h2 class="mdv-modal-title"></h2>' +
    '<p class="mdv-modal-warn"></p>' +
    '<div class="mdv-modal-url"></div>' +
    '<div class="mdv-modal-actions">' +
    '<button type="button" class="mdv-cancel"></button>' +
    '<button type="button" class="mdv-danger mdv-open" disabled></button>' +
    '</div></div>'
  const modal = overlay.querySelector('.mdv-modal')
  modal.querySelector('.mdv-modal-title').textContent = '打开外部链接'
  modal.querySelector('.mdv-modal-warn').textContent =
    '此链接会交给系统打开，可能是程序或脚本。请确认来源可信。'
  modal.querySelector('.mdv-modal-url').textContent = href
  const btnCancel = modal.querySelector('.mdv-cancel')
  const btnOpen = modal.querySelector('.mdv-open')
  btnCancel.textContent = '取消'
  btnOpen.textContent = '打开（' + kOpenConfirmDelay + '）'

  let timer = 0
  const close = (open) => {
    if (timer) clearInterval(timer)
    timer = 0
    overlay.remove()
    if (prevFocus && prevFocus.isConnected) prevFocus.focus()
    if (open) postRaw('open|' + href)
  }

  timer = setInterval(() => {
    const left = (btnOpen.dataset.left ? parseInt(btnOpen.dataset.left, 10) : kOpenConfirmDelay) - 1
    if (left > 0) {
      btnOpen.dataset.left = String(left)
      btnOpen.textContent = '打开（' + left + '）'
      return
    }
    clearInterval(timer)
    timer = 0
    btnOpen.textContent = '打开'
    btnOpen.disabled = false
  }, 1000)

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
  const href = a.getAttribute('data-external') || a.getAttribute('href')
  if (!href) return
  if (href.charAt(0) === '#') {
    if (!V.active) return
    const frag = href.slice(1)
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
  confirmOpenExternal(href)
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
    place()
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

window.__mdv = { md, renderDocument, applySkin, applyFx, state, resolveDocUrl, DOC_HOST, V, SLUG }

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
