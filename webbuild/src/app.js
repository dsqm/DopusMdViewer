import MarkdownIt from 'markdown-it'
import deflist from 'markdown-it-deflist'
import taskLists from 'markdown-it-task-lists'
import sub from 'markdown-it-sub'
import sup from 'markdown-it-sup'
import mark from 'markdown-it-mark'
import texmath from 'markdown-it-texmath'
import katex from 'katex'
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
   约定（票 03 / 05 / 06 / 07 / 08 / 11）
   - 全文不进 DOM：自绘滚动条 + 估算总高 + 常驻窗口（800 块量级）。
   - 块正文走 https://mdblocks.local/__blocks 的二进制帧，批量取。
   - 颜色全在 #base / #skin，字体全在 #fx（#fx 永远最后）。
   - 单块高亮闸门：≤2000 行且 ≤256 KiB，超限只出纯文本。
   ============================================================ */

const HIGHLIGHT_MAX_LINES = 2000
const HIGHLIGHT_MAX_BYTES = 256 * 1024
const DOC_BASE_DEFAULT = 'https://mddoc.local/'
const DOC_HOST = 'https://mddoc.local/'
const BLOCKS_URL = 'https://mdblocks.local/__blocks'
const LINES_URL = 'https://mdblocks.local/__lines'
const DEFS_URL = 'https://mdblocks.local/__defs'
const SEARCH_URL = 'https://mdblocks.local/__search'
const OUTLINE_URL = 'https://mdblocks.local/__outline'

const OVERSCAN = 120          // 视口上下各留多少块
const MAX_FETCH_BLOCKS = 100  // 单次请求块数（票 03：50–200）
const MAX_INFLIGHT = 3

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

/* 跨块语义（票 05）：链接引用定义与脚注定义散在别的块里，逐块渲染时拿不到。
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
  .use(texmath, {
    engine: katex,
    delimiters: 'dollars',
    katexOptions: {
      throwOnError: false,
      strict: false,
      trust: false,
      output: 'htmlAndMathml',
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
    if (/^https:\/\/mdblocks\.local\//i.test(src)) return
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
// 而 mdblocks.local 的请求由 C++ 合成，可带正确的 Content-Type。
// 绝对路径与 ../ 也统一走 __asset，宿主按设置决定放不放行。
function assetUrl(path) {
  return 'https://mdblocks.local/__asset?p=' + encodeURIComponent(path)
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

const MATH_RULES = ['math_inline', 'math_inline_double', 'math_block', 'math_block_eqno']

const V = {
  active: false,
  docId: 0,
  blockCount: 0,
  prefixCount: 0,
  estTotal: 0,
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
}

const decoder = new TextDecoder('utf-8')
const KIND_NAMES = ['none', 'para', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6',
  'fence', 'table', 'list', 'quote', 'html', 'hr', 'fm']

/* 页内搜索与大纲（M6）。命中在 C++ 里按源字节找，块内偏移换成解码文本的
   UTF-16 单元下标；前端在渲染后的文本里找最近的一处真实出现位置再包 mark——
   源码里的 `**`、链接语法不在渲染文本里，纯按下标切会偏。 */
const FIND = {
  open: false,
  query: '',
  cs: false,
  results: [],
  index: -1,
  total: 0,
  truncated: false,
  busy: false,
  seq: 0,
  timer: 0,
  pending: null,
  current: null,
}

const OUT = {
  items: [],
  loaded: false,
  loading: false,
  total: 0,
  truncated: false,
  seq: 0,
}

let docEl = null
let sbEl = null
let thumbEl = null
let toolsEl = null
let findEl = null
let findInput = null
let findCount = null
let findCaseBtn = null
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

function renderLoose(text) {
  const env = blockEnv()
  const tokens = md.parse(text, env)
  for (const t of tokens) {
    if (t.type === 'paragraph_open' || t.type === 'paragraph_close') t.hidden = false
  }
  return md.renderer.render(tokens, md.options, env)
}

function renderBlockHtml(text, flags) {
  try {
    const html = (flags & 0x10) ? renderLoose(text) : md.render(text, blockEnv())
    return sanitize(html)
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
    let html = renderBlockHtml(rec.text, rec.flags)
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
  let html = renderBlockHtml(item.text, item.flags)
  // 脚注区挂在最后一个块上：没有独立块号，也不参与索引
  if (i === V.lastVisible && FOOT.html) html += FOOT.html
  rec.el.innerHTML = html
  processNetImages(rec.el)
  setHeight(i, rec.el.offsetHeight)
  queueMermaid(rec.el, V.docId)
  if (FIND.pending && FIND.pending.b === i) maybeApplyHit()
  else if (FIND.current && FIND.current.b === i) applyHitMark(rec.el, FIND.current.off, false)
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
      curSeg.want = null
      if (got < count) curSeg.shortAt = from
      if (total > 0) updateBigLines(i, total)
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
  if (a !== V.windowStart || b !== V.windowEnd) {
    V.windowStart = a
    V.windowEnd = b
    V.stageBase = anchor
    recycleOutside(a, b)
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
  resetNetConsent()
}

function applyIndex(base, prefix, est, fileSize, enc, lastVisible) {
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

/* ---------------- 搜索与大纲（M6） ---------------- */

function foldAscii(text) {
  // 与 C++ 的搜索保持一致：只折叠 ASCII 字母，长度不变
  return text.replace(/[A-Z]/g, (c) => String.fromCharCode(c.charCodeAt(0) + 32))
}

function findOccurrences(text, query, caseSensitive) {
  if (!query) return []
  const hay = caseSensitive ? text : foldAscii(text)
  const needle = caseSensitive ? query : foldAscii(query)
  const out = []
  let p = 0
  for (;;) {
    p = hay.indexOf(needle, p)
    if (p < 0) break
    out.push(p)
    p += Math.max(1, needle.length)
    if (out.length >= 500) break
  }
  return out
}

function clearHitMarks() {
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
    } else if (!FIND.results.length) {
      text = '无结果'
    } else {
      text = (FIND.index >= 0 ? FIND.index + 1 : 0) + '/' + FIND.total
        + (FIND.truncated ? '+' : '')
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
  FIND.pending = null
  FIND.current = null
  clearHitMarks()
  updateFindCount()

  const url = SEARCH_URL + '?q=' + encodeURIComponent(FIND.query)
    + (FIND.cs ? '&cs=1' : '')
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
      const results = []
      let o = 12
      for (let i = 0; i < count && o + 8 <= buf.byteLength; i++) {
        results.push({ b: dv.getUint32(o, true), off: dv.getUint32(o + 4, true) })
        o += 8
      }
      FIND.results = results
      FIND.total = total
      FIND.truncated = (flags & 1) !== 0
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
  clearHitMarks()
  updateFindCount()
  jumpToOffset(pos(hit.b) - V.vh * 0.15)
  maybeApplyHit()
}

function maybeApplyHit() {
  const hit = FIND.pending
  if (!hit || !V.active) return
  if (V.big.has(hit.b)) {
    FIND.pending = null
    return
  }
  if (hit.b < V.windowStart || hit.b > V.windowEnd) return
  const rec = V.resident.get(hit.b)
  if (!rec || !rec.el || !rec.text) return
  FIND.pending = null
  FIND.current = hit
  applyHitMark(rec.el, hit.off)
}

function applyHitMark(el, unitOff, center = true) {
  clearHitMarks()
  const query = FIND.query
  if (!query) return
  const occurrences = findOccurrences(el.textContent || '', query, FIND.cs)
  if (!occurrences.length) return

  let best = occurrences[0]
  let bestDist = Math.abs(occurrences[0] - unitOff)
  for (const start of occurrences) {
    const dist = Math.abs(start - unitOff)
    if (dist < bestDist) {
      best = start
      bestDist = dist
    }
  }
  wrapTextRange(el, best, best + query.length)
  if (!center) return
  const mark = el.querySelector('mark.hit')
  if (!mark) return
  const rect = mark.getBoundingClientRect()
  jumpToOffset(V.off + rect.top - V.vh * 0.4)
}

function reapplyCurrentHit() {
  if (!FIND.current || !V.active) return
  const rec = V.resident.get(FIND.current.b)
  if (rec && rec.el && rec.text) applyHitMark(rec.el, FIND.current.off, false)
}

function resetFind() {
  closeFind()
  if (findInput) findInput.value = ''
  FIND.query = ''
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
  // 皮肤换了，后续图表要按新配色重画（已画好的等 M6 换皮肤重渲染时再处理）
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
      el.innerHTML = svg
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
   控制帧格式：<type>|<字段…>，小载荷；块正文只走 mdblocks.local 的二进制帧。 */

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
      parseInt(f[5], 10) || 0)
    return
  }

  if (type === 'index-full') {
    const f = rest.split('|')
    applyIndexFull(parseInt(f[0], 10) || 0, parseInt(f[1], 10) || 0)
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

document.addEventListener('click', (e) => {
  const a = e.target && e.target.closest ? e.target.closest('a') : null
  if (!a) return
  e.preventDefault()
  const href = a.getAttribute('data-external') || a.getAttribute('href')
  if (!href) return
  if (href.charAt(0) === '#') {
    // 脚注角标跳到文末脚注区；反向链接没有块号映射，先不处理
    if (/^#fn\d/.test(href) && V.active) {
      V.target = Math.max(0, V.totalH - V.vh)
      kick()
    }
    return
  }
  postRaw('open|' + href)
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
  if (document.fonts && document.fonts.ready) {
    document.fonts.ready.then(() => {
      if (V.active) remeasureAll()
    })
  }
}

window.addEventListener('error', (e) => {
  report('js-error', (e.message || 'error') + ' @' + (e.filename || '') + ':' + (e.lineno || 0))
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

window.__mdv = { md, renderDocument, applySkin, applyFx, state, resolveDocUrl, DOC_HOST, V }
