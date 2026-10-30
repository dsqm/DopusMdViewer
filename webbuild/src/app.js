import MarkdownIt from 'markdown-it'
import footnote from 'markdown-it-footnote'
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
   约定（票 05 / 06 / 07 / 08 / 11）
   - 文档正文只从 mdprev.local 同源取，相对资源走 mddoc.local。
   - 颜色全在 #base / #skin，字体全在 #fx（#fx 永远最后）。
   - 单块高亮闸门：≤2000 行且 ≤256 KiB，超限只出纯文本。
   - Mermaid 只在出现 ```mermaid 时按需拉 mermaid.js。
   ============================================================ */

const HIGHLIGHT_MAX_LINES = 2000
const HIGHLIGHT_MAX_BYTES = 256 * 1024
const DOC_BASE_DEFAULT = 'https://mddoc.local/'
const DOC_HOST = 'https://mddoc.local/'

const LANGUAGES = {
  javascript, typescript, python, c, cpp, csharp, java, go, rust,
  json, bash, xml, css, sql, yaml, ini, markdown, diff, powershell,
}

for (const [name, lang] of Object.entries(LANGUAGES)) hljs.registerLanguage(name, lang)

/* ---------------- 渲染管线 ---------------- */

const md = new MarkdownIt({
  html: true,
  linkify: false,
  breaks: false,
  typographer: false,
  highlight: null,
})

md.use(footnote)
  .use(deflist)
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

const escapeHtml = md.utils.escapeHtml

/* front matter（开头 --- 包的 YAML 段）不参与渲染。C++ 侧扫描块时也要按同一条
   规则跳过，否则块号会整体偏移一段。 */
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

  if (lang === 'mermaid') {
    return '<div class="mermaid-block" data-mermaid="pending">'
      + '<pre class="mermaid-src"><code>' + escapeHtml(code) + '</code></pre>'
      + '</div>\n'
  }

  const lines = countLines(code)
  const bytes = utf8Bytes(code)
  const tooHuge = lines > HIGHLIGHT_MAX_LINES || bytes > HIGHLIGHT_MAX_BYTES

  let body
  if (lang && !tooHuge && hljs.getLanguage(lang)) {
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

DOMPurify.addHook('afterSanitizeAttributes', (node) => {
  if (node.tagName === 'IMG') {
    const src = node.getAttribute('src')
    if (src) node.setAttribute('src', resolveDocUrl(src))
    node.setAttribute('loading', 'lazy')
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
  source: '',
  mermaidState: 'idle',
  mermaidApi: null,
}

const docEl = () => document.getElementById('doc')

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
    '.measure{max-width:' + measure + '!important;}',
  ].join('')

  document.getElementById('fx').textContent = css
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
    if (generation !== renderGeneration) return
    const srcEl = el.querySelector('code')
    const src = srcEl ? srcEl.textContent : ''
    const id = 'mdv-mermaid-' + (++mermaidSeq)
    try {
      const out = await Promise.race([
        api.render(id, src),
        new Promise((_, reject) => setTimeout(() => reject(new Error('timeout')), 10000)),
      ])
      if (generation !== renderGeneration) return
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
    }
  }
}

/* ---------------- 渲染入口 ---------------- */

function renderDocument(source, docBase) {
  const t0 = performance.now()
  const generation = ++renderGeneration

  if (docBase) state.docBase = docBase
  state.source = source

  const body = stripFrontMatter(source)
  const html = sanitize(md.render(body, { docBase: state.docBase }))
  docEl().innerHTML = html

  // 解析 + 清洗 + 挂 DOM 的耗时；M4 的对照基线就取这个数
  const ms = performance.now() - t0

  renderMermaidBlocks(docEl(), generation)

  // 附上布局度量：排查"内容列偏心/超宽"这类问题只有数字说话
  const box = docEl().getBoundingClientRect()
  const metrics = 'vp=' + window.innerWidth + 'x' + window.innerHeight
    + ' dpr=' + (window.devicePixelRatio || 1).toFixed(2)
    + ' col=' + Math.round(box.width) + '@' + Math.round(box.left)

  report('rendered', ms.toFixed(1) + '|' + source.length + '|' + metrics)
  return ms
}

/* ---------------- 宿主通道 ----------------
   帧格式（票 11 第 5 节）：<type>|<字段…>|<载荷>，载荷永远在最后一个字段，
   所以正文与 CSS 里出现竖线不会打乱解析。C++ 侧因此不需要 JSON 库。 */

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

  if (type === 'doc') {
    const [base, text] = splitFirst(rest)
    renderDocument(text, base || state.docBase)
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
  if (href && href.charAt(0) !== '#') postRaw('open|' + href)
}, true)

if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener('message', onHostMessage)
  postRaw('ready')
}

/* ---------------- 开发模式（只在 Edge 里用，线上走 postMessage） ---------------- */

async function devBoot() {
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
    docEl().textContent = 'cannot load ' + doc
    return
  }
  const text = await res.text()
  renderDocument(text, new URL('.', res.url).href)
}

if (!(window.chrome && window.chrome.webview)) {
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', devBoot)
  } else {
    devBoot()
  }
}

window.__mdv = { md, renderDocument, applySkin, applyFx, state, resolveDocUrl, DOC_HOST }
