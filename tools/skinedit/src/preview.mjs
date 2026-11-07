/* 皮肤编辑器的预览页脚本（esbuild 入口，产物 tools/skinedit/web/preview.js）。
 *
 * 与插件 app.js 的分工：app.js 为超大文档做分块渲染，脚注靠宿主帧里的 applyDefs
 * 另拼；这里只渲染一份固定样例，全文一次 md.render 即可，脚注直接用
 * markdown-it-footnote 出 <section class="footnotes">。DOM 形状必须与 base.css
 * 的契约一致（body.dev、#doc、div.mdv-cb > pre > code.hljs.has-ln、空 span 行号）。
 *
 * 颜色不在这里管：父页面（shell.js）同源直接写 documentElement 的 inline 变量，
 * 模板皮肤本身由本文件按 ?skin= 载入 <style id="skin">。
 */

import MarkdownIt from 'markdown-it'
import deflist from 'markdown-it-deflist'
import taskLists from 'markdown-it-task-lists'
import sub from 'markdown-it-sub'
import sup from 'markdown-it-sup'
import mark from 'markdown-it-mark'
import footnote from 'markdown-it-footnote'
import { alert } from '@mdit/plugin-alert'
import texmath from 'markdown-it-texmath'
import katex from 'katex'
import DOMPurify from 'dompurify'
import hljs from 'highlight.js/lib/core'

import python from 'highlight.js/lib/languages/python'
import javascript from 'highlight.js/lib/languages/javascript'
import css from 'highlight.js/lib/languages/css'
import diff from 'highlight.js/lib/languages/diff'
import c from 'highlight.js/lib/languages/c'
import xml from 'highlight.js/lib/languages/xml'
import bash from 'highlight.js/lib/languages/bash'

for (const [name, lang] of Object.entries({
  python, javascript, css, diff, c, html: xml, xml, bash,
})) hljs.registerLanguage(name, lang)

const esc = (s) => String(s).replace(/[&<>"']/g,
  (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c])

const md = new MarkdownIt({ html: true, linkify: true })
  .use(deflist)
  .use(taskLists, { enabled: true, label: true })
  .use(sub).use(sup).use(mark)
  .use(footnote)
  .use(alert)
  .use(texmath, {
    engine: katex,
    delimiters: 'dollars',
    katexOptions: { throwOnError: false, strict: 'ignore' },
  })

/* markdown-it 默认只放行 data:image/(gif|png|jpeg|webp)：样例里的图片是
   data:image/svg+xml，会被当成普通文字吐出来。这里只多放行 data:image/，
   其余协议仍按它的黑白名单走（输出还要过 DOMPurify）。 */
const baseValidateLink = md.validateLink.bind(md)
md.validateLink = (url) => /^data:image\//i.test(url) || baseValidateLink(url)

/* 行号只由空 <span> + CSS 计数器画：伪元素内容不进 textContent，
   复制按钮取的仍是源码原文。格子挂在 <code> 里，避开 .code-note 推低首行。 */
const displayLines = (text) => {
  let n = 1
  for (let i = 0; i < text.length - 1; i++) if (text.charCodeAt(i) === 10) n++
  return n
}
const gutter = (n) => '<span class="mdv-ln" aria-hidden="true">' +
  '<span></span>'.repeat(n) + '</span>'
const widthCls = (n) => 'ln-d' + Math.min(6, String(n).length)

const copyBtn = '<button type="button" class="mdv-copy" data-k="code"' +
  ' title="复制代码" aria-label="复制代码">' +
  '<svg class="mdv-i" aria-hidden="true" focusable="false"><use href="#i-copy"></use></svg>' +
  '</button>'

function codeBody(code, lang) {
  return lang && hljs.getLanguage(lang)
    ? hljs.highlight(code, { language: lang, ignoreIllegals: true }).value
    : esc(code)
}

md.renderer.rules.fence = (tokens, idx) => {
  const t = tokens[idx]
  const lang = (t.info || '').trim().split(/\s+/)[0].toLowerCase()
  const code = t.content
  const n = displayLines(code)
  return '<div class="mdv-cb">' + copyBtn + '<pre><code class="hljs language-' +
    (lang || 'none') + ' has-ln ' + widthCls(n) + '">' +
    gutter(n) + codeBody(code, lang) + '</code></pre></div>\n'
}

md.renderer.rules.code_block = (tokens, idx) => {
  const code = tokens[idx].content
  const n = displayLines(code)
  return '<div class="mdv-cb"><pre><code class="hljs has-ln ' + widthCls(n) + '">' +
    gutter(n) + esc(code) + '</code></pre></div>\n'
}

/* 样例不放外链图片：非 data: 的一律换成 1x1 透明占位。
   插件那条 __asset 重写路由在预览侧不存在，硬留 src 只会得到破图。 */
const BLANK = 'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7'
md.renderer.rules.image = (tokens, idx) => {
  const t = tokens[idx]
  const src = t.attrGet('src') || ''
  const alt = esc(t.content || '')
  const title = t.attrGet('title')
  return '<img src="' + esc(/^data:image\//i.test(src) ? src : BLANK) +
    '" alt="' + alt + '"' + (title ? ' title="' + esc(title) + '"' : '') + '>'
}

const sanitize = (html) => DOMPurify.sanitize(html, {
  WHOLE_DOCUMENT: false,
  RETURN_DOM_FRAGMENT: false,
  ADD_TAGS: ['use'],
  ADD_ATTR: ['href', 'data-k'],
  FORBID_TAGS: ['style', 'link', 'meta', 'base'],
  FORBID_ATTR: ['srcset'],
})

/* 大纲面板在预览里是静态占位，但联动表有 #outline .outline-item 这条规则。
   不填的话 outline-* 变量永远找不到可跳的元素。 */
function fillOutline(docEl) {
  const list = document.getElementById('outline-list')
  if (!list) return
  const frag = document.createDocumentFragment()
  for (const h of docEl.querySelectorAll('h1, h2, h3')) {
    const item = document.createElement('div')
    item.className = 'outline-item'
    item.textContent = h.textContent
    frag.append(item)
  }
  list.append(frag)
}

async function boot() {
  document.body.classList.add('dev')
  const p = new URLSearchParams(location.search)

  // 开发时在 dev-server 下跑：base.css / katex.css 来自插件的 web 目录。
  // 真实宿主映射到 assets 根，不带 root，相对路径即可。
  // 两张表必须排在 <style id="skin"> 之前，否则 base.css 的默认变量会盖掉皮肤。
  const root = p.get('root') || ''
  const anchor = document.getElementById('skin')
  for (const file of ['base.css', 'katex.css']) {
    const link = document.createElement('link')
    link.rel = 'stylesheet'
    link.href = root + file
    document.head.insertBefore(link, anchor)
  }

  const skin = p.get('skin')
  if (skin) {
    const skinRoot = p.get('skinRoot') || 'skins/'
    try {
      const r = await fetch(skinRoot + encodeURIComponent(skin) + '.css')
      if (r.ok) document.getElementById('skin').textContent = await r.text()
    } catch { /* 模板缺席就留在 base.css 默认层 */ }
  }

  const docUrl = p.get('doc') || 'samples/skin-preview.md'
  try {
    const r = await fetch(docUrl)
    if (!r.ok) throw new Error('HTTP ' + r.status)
    const docEl = document.getElementById('doc')
    docEl.innerHTML = sanitize(md.render(await r.text()))
    // 真机的脚注区由宿主自己拼，不带 markdown-it-footnote 的分隔 hr。
    // 留着它会让 --md-hr 在预览里比真机多一条线。
    for (const sep of docEl.querySelectorAll('hr.footnotes-sep')) sep.remove()
    fillOutline(docEl)
    document.documentElement.dataset.previewReady = '1'
    // iframe 的 load 早于这段异步渲染；父页面等这条消息才建联动索引。
    if (window.parent !== window) {
      window.parent.postMessage({ type: 'skinedit-preview-ready' }, location.origin)
    }
  } catch (e) {
    document.getElementById('doc').textContent = '预览样例读取失败：' + e.message
    document.documentElement.dataset.previewReady = 'error'
  }
}

boot()
