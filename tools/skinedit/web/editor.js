/* 皮肤编辑器主体（票 11）。由窗口交互原型 D 变体移植而来，去掉原型壳与假数据。
   左栏 108 项表单 + 右栏自渲染预览；文件动作由宿主通道提供，见后续阶段。 */

import { GROUPS, DEFAULTS, parseSkin } from './skinfile.mjs'



const SKIN_LABELS = {
  light: '明亮',
  dark: '深色',
  'solarized-light': 'Solarized 浅',
  'solarized-dark': 'Solarized 深',
}

/* 变量的中文名：左侧表单显示它，右侧联动时报的也是它。
   命名约定：<对象>字 在前、<对象>背景 在后；不带「色」字，不用「底」。 */
const NAMES = {
  fg: '正文字', 'fg-muted': '次要字', 'fg-heading': '标题字',
  'h1-fg': '一级标题字', 'h2-fg': '二级标题字', 'h3-fg': '三级标题字',
  'h4-fg': '四级标题字', 'h5-fg': '五级标题字', 'h6-fg': '六级标题字',
  bg: '页面背景', 'bg-alt': '面板背景', border: '边框',
  'h1-border': '一级标题下边线', 'h2-border': '二级标题下边线', 'h3-border': '三级标题下边线',
  'h4-border': '四级标题下边线', 'h5-border': '五级标题下边线', 'h6-border': '六级标题下边线',

  link: '链接字', 'link-hover': '链接字（悬停）', danger: '危险字',
  selection: '选中背景', hit: '命中背景', 'hit-current': '当前命中背景', 'mark-bg': '高亮背景',

  'block-fg': '代码块字', 'block-bg': '代码块背景', 'block-border': '代码块边框',
  'code-fg': '行内码字', 'code-bg': '行内码背景',
  'code-ln-fg': '行号字', 'code-ln-bg': '行号背景',
  'code-ws-fg': '空白标记字',
  'copy-fg': '复制按钮字', 'copy-bg': '复制按钮背景', 'copy-hover-fg': '复制按钮字（悬停）',

  'quote-fg': '引用字', 'quote-bg': '引用背景', 'quote-bar': '引用竖条',
  'quote-2-fg': '嵌套引用字', 'quote-2-bg': '嵌套引用背景', 'quote-2-bar': '嵌套引用竖条',
  'table-head-fg': '表头字', 'table-head-bg': '表头背景', 'table-head-border': '表头边框',
  'table-bg': '表格背景', 'table-odd-bg': '奇行背景', 'table-even-bg': '偶行背景',
  'table-row-hover': '行悬停背景', 'table-border': '表格边框',

  'alert-note': '说明字', 'alert-note-bg': '说明背景', 'alert-note-title': '说明标题字',
  'alert-tip': '提示字', 'alert-tip-bg': '提示背景', 'alert-tip-title': '提示标题字',
  'alert-important': '重要字', 'alert-important-bg': '重要背景', 'alert-important-title': '重要标题字',
  'alert-warning': '警告字', 'alert-warning-bg': '警告背景', 'alert-warning-title': '警告标题字',
  'alert-caution': '注意字', 'alert-caution-bg': '注意背景', 'alert-caution-title': '注意标题字',

  'outline-fg': '大纲字', 'outline-bg': '大纲背景', 'outline-border': '大纲边框',
  'outline-hover-bg': '大纲悬停背景',
  'find-fg': '搜索条字', 'find-bg': '搜索条背景', 'find-border': '搜索条边框',
  'tools-bg': '工具条背景', 'icon-active': '图标选中',
  scrollbar: '滚动条滑块', 'scrollbar-hover': '滚动条滑块（悬停）', 'scrollbar-track': '滚动条轨道',

  icon: '图标线条', 'icon-hover': '图标线条（悬停）', 'icon-bg': '图标背景', 'icon-border': '图标边框',

  'syn-keyword': '关键字', 'syn-string': '字符串', 'syn-comment': '注释',
  'syn-number': '数字', 'syn-builtin': '内建名', 'syn-title': '函数名', 'syn-type': '类型名',
  'syn-variable': '变量名', 'syn-name': '标签名', 'syn-selector': '选择器', 'syn-meta': '元信息',
  'syn-deletion': '删除行字', 'syn-deletion-bg': '删除行背景',
  'syn-addition': '新增行字', 'syn-addition-bg': '新增行背景',

  'footnote-fg': '脚注字', 'footnote-bg': '脚注背景', 'footnote-border': '脚注分隔线',
  'ul-marker': '无序列表符号', 'ol-marker': '有序列表符号', 'list-marker': '列表符号',
  hr: '分隔线',

  radius: '圆角', 'radius-code': '代码块圆角', 'radius-alert': '提示框圆角',
  'radius-ui': '小按钮圆角', 'space-block': '块间距',
}

/* RULES 里查不到的 10 项：它们是总开关，或者消费者在预览里不出现。
   这 10 句是手写的——除此之外所有「影响」都从 RULES 反查，不再写第二份真相。 */
const HINT = {
  'bg-alt': '内层浅底：键盘键帽、弹窗按钮悬停、图片占位框、大纲悬停的默认值',
  'fg-muted': '次要文字：页脚、定义列表的说明、表注；标题以外的弱化文字都取它',
  'fg-heading': '所有标题（h1–h6）的总开关；单独改过某一级，那一级就不再跟它',
  border: '所有细线的总开关：表格、引用竖条、代码块、图标边框、图片占位都取它',
  danger: '危险字：外部链接确认弹窗的标题与主按钮，正文里用不到',
  selection: '拖选文字时的背景（原生 ::selection），不影响搜索命中的高亮',
  'code-ws-fg': '空白符标记的颜色（只在该功能打开时可见）；默认跟随次要字',
  'radius-code': '代码块外框圆角；默认跟随总圆角',
  'radius-alert': '提示框外框圆角；默认跟随总圆角',
  'radius-ui': '小按钮（复制、搜索、大纲条目）的圆角；默认是本值的 0.7 倍',
  radius: '所有圆角的总开关：代码块、提示框、表格、小按钮；单独配过的项不再跟它',
  'space-block': '块与块之间的垂直间距（段落、列表、表格、提示框的上下外边距）',
}


/* 表单里的分组标题：只影响显示，皮肤文件里的分组注释仍是契约里的原名。
   改名是为了跟「字在前、背景在后」的命名对上，并去掉中英混写。 */
const GROUP_TITLES = {
  '底与字': '字与背景',
  '图标（工具条按钮与复制按钮）': '图标',
  '语法高亮 token': '语法高亮',
  '滚动条与界面件': '滚动条与界面元素',
}

/* 显示名的集合：折叠状态按显示名记（切皮肤重建表单时靠它认回每一组）。 */
const GROUP_TITLE_SET = Object.fromEntries(
  GROUPS.map((g) => [GROUP_TITLES[g.title] || g.title, true]))

/* 表单里的显示顺序：字在前、背景在后。皮肤文件里仍是 GROUPS 的固定顺序，
   两者分开——文件顺序是对外契约，表单顺序是给人看的。 */
const DISPLAY = {
  '底与字': ['fg', 'fg-muted', 'fg-heading',
             'h1-fg', 'h2-fg', 'h3-fg', 'h4-fg', 'h5-fg', 'h6-fg',
             'bg', 'bg-alt', 'border',
             'h1-border', 'h2-border', 'h3-border', 'h4-border', 'h5-border', 'h6-border'],
  '链接与交互': ['link', 'link-hover', 'danger', 'selection', 'hit', 'hit-current', 'mark-bg'],
  '代码': ['block-fg', 'block-bg', 'block-border',
           'code-fg', 'code-bg', 'code-ln-fg', 'code-ln-bg', 'code-ws-fg',
           'copy-fg', 'copy-bg', 'copy-hover-fg'],
  '引用与表格': ['quote-fg', 'quote-bg', 'quote-bar',
                 'quote-2-fg', 'quote-2-bg', 'quote-2-bar',
                 'table-head-fg', 'table-head-bg', 'table-head-border',
                 'table-bg', 'table-odd-bg', 'table-even-bg', 'table-row-hover', 'table-border'],
  '提示框': ['alert-note', 'alert-note-bg', 'alert-note-title',
             'alert-tip', 'alert-tip-bg', 'alert-tip-title',
             'alert-important', 'alert-important-bg', 'alert-important-title',
             'alert-warning', 'alert-warning-bg', 'alert-warning-title',
             'alert-caution', 'alert-caution-bg', 'alert-caution-title'],
  '滚动条与界面件': ['outline-fg', 'outline-bg', 'outline-border', 'outline-hover-bg',
                     'find-fg', 'find-bg', 'find-border', 'tools-bg', 'icon-active',
                     'scrollbar', 'scrollbar-hover', 'scrollbar-track'],
  '图标（工具条按钮与复制按钮）': ['icon', 'icon-hover', 'icon-bg', 'icon-border'],
  '列表、分隔线与脚注': ['footnote-fg', 'footnote-bg', 'footnote-border',
                         'ul-marker', 'ol-marker', 'list-marker', 'hr'],
  '形状': ['radius', 'radius-code', 'radius-alert', 'radius-ui', 'space-block'],
}

/* 元素 → 变量。sel 用来在预览里找元素；变量顺序不重要。
   末尾的规则更具体，命中时优先用它（best() 从后往前找）。 */
const RULES = [
  { sel: '#doc', name: '文档页', vars: ['bg', 'fg'] },
  { sel: '#doc h1', name: '一级标题', vars: ['h1-fg', 'h1-border', 'space-block'] },
  { sel: '#doc h2', name: '二级标题', vars: ['h2-fg', 'h2-border', 'space-block'] },
  { sel: '#doc h3', name: '三级标题', vars: ['h3-fg', 'h3-border', 'space-block'] },
  { sel: '#doc h4', name: '四级标题', vars: ['h4-fg', 'h4-border', 'space-block'] },
  { sel: '#doc h5', name: '五级标题', vars: ['h5-fg', 'h5-border', 'space-block'] },
  { sel: '#doc h6', name: '六级标题', vars: ['h6-fg', 'h6-border', 'space-block'] },
  { sel: '#doc p', name: '正文段落', vars: ['fg', 'space-block'] },
  { sel: '#doc a', name: '链接', vars: ['link', 'link-hover'] },
  { sel: '#doc code', name: '行内代码', vars: ['code-bg', 'code-fg'] },
  { sel: '#doc blockquote', name: '引用块', vars: ['quote-bar', 'quote-fg', 'quote-bg'] },
  { sel: '#doc blockquote blockquote', name: '嵌套引用', vars: ['quote-2-bar', 'quote-2-fg', 'quote-2-bg'] },
  { sel: '#doc li', name: '列表项', vars: ['list-marker', 'space-block'] },
  { sel: '#doc ul > li', name: '无序列表项', vars: ['ul-marker'] },
  { sel: '#doc ol > li', name: '有序列表项', vars: ['ol-marker'] },
  { sel: '#doc dl, #doc dt, #doc dd', name: '定义列表', vars: ['space-block'] },
  { sel: '#doc hr', name: '分隔线', vars: ['hr'] },
  { sel: '#doc mark.hit--current', name: '当前命中', vars: ['hit-current'] },
  { sel: '#doc mark.hit', name: '搜索命中', vars: ['hit'] },
  { sel: '#doc mark', name: '高亮标记', vars: ['mark-bg'] },
  { sel: '#doc table', name: '表格', vars: ['table-bg', 'table-border', 'radius'] },
  { sel: '#doc thead th', name: '表头单元格', vars: ['table-head-bg', 'table-head-fg', 'table-head-border'] },
  { sel: '#doc tbody tr:nth-child(odd) td', name: '表格奇行', vars: ['table-odd-bg', 'table-border'] },
  { sel: '#doc tbody tr:nth-child(even) td', name: '表格偶行', vars: ['table-even-bg', 'table-border'] },
  { sel: '#doc thead th, #doc tbody td', name: '表格单元格（悬停）', vars: ['table-row-hover'] },
  { sel: '#doc .markdown-alert', name: '提示框', vars: ['space-block', 'radius'] },
  { sel: '#doc .markdown-alert-note', name: '提示框（说明）', vars: ['alert-note', 'alert-note-bg', 'alert-note-title'] },
  { sel: '#doc .markdown-alert-tip', name: '提示框（提示）', vars: ['alert-tip', 'alert-tip-bg', 'alert-tip-title'] },
  { sel: '#doc .markdown-alert-important', name: '提示框（重要）', vars: ['alert-important', 'alert-important-bg', 'alert-important-title'] },
  { sel: '#doc .markdown-alert-warning', name: '提示框（警告）', vars: ['alert-warning', 'alert-warning-bg', 'alert-warning-title'] },
  { sel: '#doc .markdown-alert-caution', name: '提示框（注意）', vars: ['alert-caution', 'alert-caution-bg', 'alert-caution-title'] },
  { sel: '#doc .mdv-cb', name: '代码块', vars: ['block-bg', 'block-fg', 'block-border', 'radius'] },
  { sel: '#doc .mdv-ln', name: '代码行号', vars: ['code-ln-fg', 'code-ln-bg'] },
  { sel: '#doc .mdv-copy', name: '复制按钮', vars: ['copy-fg', 'copy-bg', 'copy-hover-fg', 'icon-border'] },
  { sel: '#doc .footnotes', name: '脚注区', vars: ['footnote-fg', 'footnote-bg', 'footnote-border'] },
  { sel: '#doc .hljs-keyword', name: '语法·关键字', vars: ['syn-keyword'] },
  { sel: '#doc .hljs-string, #doc .hljs-regexp', name: '语法·字符串', vars: ['syn-string'] },
  { sel: '#doc .hljs-comment', name: '语法·注释', vars: ['syn-comment'] },
  { sel: '#doc .hljs-number', name: '语法·数字', vars: ['syn-number'] },
  { sel: '#doc .hljs-built_in, #doc .hljs-property, #doc .hljs-attribute', name: '语法·内建名/属性', vars: ['syn-builtin'] },
  { sel: '#doc .hljs-title', name: '语法·函数名', vars: ['syn-title'] },
  { sel: '#doc .hljs-type', name: '语法·类型名', vars: ['syn-type'] },
  { sel: '#doc .hljs-variable', name: '语法·变量名', vars: ['syn-variable'] },
  { sel: '#doc .hljs-name, #doc .hljs-tag', name: '语法·标签名', vars: ['syn-name'] },
  { sel: '#doc .hljs-selector-class, #doc .hljs-selector-id', name: '语法·选择器', vars: ['syn-selector'] },
  { sel: '#doc .hljs-meta', name: '语法·预处理/元信息', vars: ['syn-meta'] },
  { sel: '#doc .hljs-deletion', name: '语法·删除行', vars: ['syn-deletion', 'syn-deletion-bg'] },
  { sel: '#doc .hljs-addition', name: '语法·新增行', vars: ['syn-addition', 'syn-addition-bg'] },
  { sel: '#tools', name: '工具条（界面元素）', vars: ['tools-bg', 'icon', 'icon-hover', 'icon-bg', 'icon-border'] },
  { sel: '#find', name: '搜索条（界面元素）', vars: ['find-fg', 'find-bg', 'find-border', 'icon', 'icon-active'] },
  { sel: '#outline', name: '大纲面板（界面元素）', vars: ['outline-bg', 'outline-fg', 'outline-border'] },
  { sel: '#outline .outline-item', name: '大纲条目', vars: ['outline-fg', 'outline-hover-bg'] },
  { sel: '#sb-thumb', name: '自绘滚动条', vars: ['scrollbar', 'scrollbar-hover', 'scrollbar-track'] },
]

/* 变量 → 用到它的元素中文名。悬停行的提示框里显示「影响：…」。
   RULES 改了这里自动跟着改——这是「不手写 108 条说明」换到的东西。 */
const VAR_TIPS = (() => {
  const m = {}
  for (const r of RULES) {
    for (const v of r.vars) {
      if (v === 'space-block' || v === 'radius') continue // 几乎每个块都有，写进提示没信息量
      ;(m[v] || (m[v] = [])).push(r.name)
    }
  }
  return m
})()

/* 提示框底色可以「跟随主色」：文件里就写 color-mix，关系留在文件里。
   这样改主色时底色自己会变——不需要按钮，也不需要编辑器记住任何东西。 */
const DERIVED_RE = /^color-mix\(\s*in srgb\s*,\s*var\(\s*--md-([a-z0-9-]+)\s*\)\s*([\d.]+)%\s*,\s*transparent\s*\)$/i

function derivedOf(raw) {
  const m = DERIVED_RE.exec(String(raw).trim())
  return m ? { from: m[1], pct: m[2] } : null
}

/* 派生底色在界面上要显示「最终是什么颜色」：按 alpha 叠在页面背景上算一次。
   只用来画色块，不写回任何地方。 */
function blendOver(fgHex, bgHex, alpha) {
  const a = Math.max(0, Math.min(1, alpha))
  const out = [1, 3, 5].map((i) => {
    const f = parseInt(fgHex.slice(i, i + 2), 16)
    const b = parseInt(bgHex.slice(i, i + 2), 16)
    return Math.round(f * a + b * (1 - a))
  })
  return '#' + out.map((n) => n.toString(16).padStart(2, '0')).join('')
}

/* 主色变了：派生行的色块要跟着重算（值本身不用动，CSS 会自己级联）。 */
function syncDerivedSwatches() {
  document.querySelectorAll('.row.derived .sw').forEach((sw) => {
    const accent = hexOf(resolveVar(sw.dataset.derived).value)
    const bg = hexOf(resolveVar('bg').value)
    const pctEl = sw.closest('.row').querySelector('.pct')
    const p = pctEl ? parseFloat(pctEl.value) : NaN
    if (accent && bg && Number.isFinite(p)) sw.value = blendOver(accent, bg, p / 100)
  })
}

const HL_CSS = `
  .__hl { outline: 2px solid #f59f00 !important; outline-offset: 1px;
          box-shadow: 0 0 0 4px rgba(245, 159, 0, .18) !important; }
  .__sel { outline: 2px solid #1f6feb !important; outline-offset: 1px;
           box-shadow: 0 0 0 5px rgba(31, 111, 235, .16) !important; }
`

const S = {
  assetRoot: '',
  sample: 'samples/skin-preview.md',
  variant: 'D',
  skin: 'light',
  w: 1180,
  h: 820,
  values: {},
  overrides: {},
  /* 特殊值的「上一个颜色」：勾掉「透明」时恢复它。只活在内存里，不进文件。 */
  lastColor: {},
  scheme: 'light',
  schemeAuto: 'light',
  widgets: false,
  hold: false,
  link: true,
  query: '',
  onlyChanged: false,
  /* 首次渲染时各组的开合：first = 只开第一组。?fold=all|none 可覆盖。 */
  openInit: 'first',
  /* 每组的展开状态按组建在内存里：切皮肤重建表单时不丢，默认只有第一组展开。 */
  open: {},
  frame: null,
  hoverVarName: null,
}

/* ---------------- 小工具 ---------------- */

function el(tag, cls, text) {
  const n = document.createElement(tag)
  if (cls) n.className = cls
  if (text != null) n.textContent = text
  return n
}

const isColorVal = (v) => /^#[0-9a-f]{3,8}$/i.test(v) || /^rgba?\(/i.test(v)

function hexOf(v) {
  if (/^#[0-9a-f]{6}$/i.test(v)) return v
  if (/^#[0-9a-f]{3}$/i.test(v)) return '#' + v.slice(1).split('').map((c) => c + c).join('')
  const m = /^rgba?\(([^)]+)\)/i.exec(v)
  if (m) {
    const p = m[1].split(/[ ,/]+/).map(Number)
    return '#' + p.slice(0, 3).map((n) =>
      Math.max(0, Math.min(255, Math.round(n))).toString(16).padStart(2, '0')).join('')
  }
  return null
}

function frameDoc() {
  try {
    return S.frame && S.frame.contentDocument
  } catch {
    return null
  }
}

/* ---------------- 应用到预览 ---------------- */

function applyAll() {
  const d = frameDoc()
  if (!d) return
  const root = d.documentElement

  for (const k of Object.keys(S.overrides)) root.style.removeProperty('--md-' + k)
  if (!S.hold) {
    for (const [k, v] of Object.entries(S.overrides)) root.style.setProperty('--md-' + k, v)
  }

  // 「按住看原皮」= 摘掉皮肤、回到 base.css 默认层。
  // 用 media="not all" 而不是 disabled：后者在 style 元素上关不掉（实测）。
  const skinEl = d.getElementById('skin')
  if (skinEl) skinEl.media = S.hold ? 'not all' : ''

  // color-scheme 不是 --md-* 变量，它得真的落在文档上才会影响原生控件
  root.style.colorScheme = S.hold ? '' : S.scheme

  for (const id of ['tools', 'outline', 'sb']) {
    const node = d.getElementById(id)
    if (node) node.hidden = !S.widgets
  }
  if (S.widgets) {
    pollOutline()
    const thumb = d.getElementById('sb-thumb')
    if (thumb && !thumb.style.height) {
      thumb.style.height = '28%'
      thumb.style.top = '10%'
    }
  }
  installLink(d)
}

function fillOutline(d) {
  const list = d.getElementById('outline-list')
  if (!list || list.childElementCount) return
  const heads = [...d.querySelectorAll('#doc h1, #doc h2, #doc h3')].slice(0, 40)
  for (const h of heads) {
    const b = d.createElement('button')
    b.type = 'button'
    b.className = 'outline-item'
    b.textContent = h.textContent
    b.style.paddingLeft = 10 + (Number(h.tagName[1]) - 1) * 12 + 'px'
    list.append(b)
  }
}

function pollOutline() {
  let n = 0
  const tick = () => {
    const d = frameDoc()
    const list = d && d.getElementById('outline-list')
    if (list && list.childElementCount) return
    const heads = d ? d.querySelectorAll('#doc h1, #doc h2, #doc h3') : []
    if (heads.length) return fillOutline(d)
    if (++n < 25) setTimeout(tick, 200)
  }
  tick()
}

/* ---------------- 左右联动 ---------------- */

/* 从被点的元素往上找，最近的、能对上规则的祖先说了算。 */
function ruleOf(target, d) {
  let node = target
  while (node && node !== d.body && node.nodeType === 1) {
    let hit = null
    for (const r of RULES) {
      if (node.matches(r.sel)) hit = r
    }
    if (hit) return { rule: hit, node }
    node = node.parentElement
  }
  return null
}

function installLink(d) {
  if (!d.getElementById('__proto-hl')) {
    const st = d.createElement('style')
    st.id = '__proto-hl'
    st.textContent = HL_CSS
    d.head.append(st)
  }
  if (d.__protoLink) return
  d.__protoLink = true

  d.addEventListener('mousemove', (e) => {
    if (!S.link) return
    const r = ruleOf(e.target, d)
    showChip(r ? r.rule.name : '', e.clientX, e.clientY, false)
  }, true)

  d.addEventListener('mouseleave', () => showChip('', 0, 0, false), true)

  d.addEventListener('click', (e) => {
    if (!S.link) return
    const r = ruleOf(e.target, d)
    if (!r) return
    e.preventDefault()
    e.stopPropagation()
    d.querySelectorAll('.__sel').forEach((n) => n.classList.remove('__sel'))
    r.node.classList.add('__sel')
    selectVars(r.rule)
    showChip(r.rule.name + ' · 左侧 ' + r.rule.vars.length + ' 项',
             e.clientX, e.clientY, true)
  }, true)
}

function selectVars(rule) {
  document.querySelectorAll('[data-var]').forEach((row) => {
    row.classList.toggle('used', rule.vars.includes(row.dataset.var))
  })
  const first = document.querySelector('[data-var].used')
  if (first) first.scrollIntoView({ block: 'center', behavior: 'smooth' })
}

function clearUsed() {
  document.querySelectorAll('[data-var].used').forEach((r) => r.classList.remove('used'))
}

/* 悬停左侧变量 → 点亮预览里用到它的元素 */
function highlightVar(name) {
  const d = frameDoc()
  if (!d) return
  d.querySelectorAll('.__hl').forEach((n) => n.classList.remove('__hl'))
  if (!name) return
  let budget = 80
  for (const r of RULES) {
    if (!r.vars.includes(name)) continue
    for (const node of d.querySelectorAll(r.sel)) {
      node.classList.add('__hl')
      if (--budget <= 0) return
    }
  }
}

/* 预览上方的浮动提示（中文名） */
let chipTimer = 0
function showChip(text, x, y, pin) {
  const chip = document.getElementById('chip')
  if (!text) {
    if (!pin) chip.hidden = true
    return
  }
  chip.hidden = false
  chip.textContent = text
  chip.classList.toggle('pin', !!pin)
  const box = document.getElementById('preview-box').getBoundingClientRect()
  chip.style.left = Math.min(box.width - 220, Math.max(8, x + 14)) + 'px'
  chip.style.top = Math.max(6, y - 30) + 'px'
  if (!pin) {
    clearTimeout(chipTimer)
    chipTimer = setTimeout(() => { chip.hidden = true }, 2200)
  }
}

/* ---------------- 皮肤 ---------------- */

async function loadSkin(id) {
  S.skin = id
  S.overrides = {}
  S.lastColor = {}
  let text = ''
  try {
    const res = await fetch(S.assetRoot + 'skins/' + id + '.css')
    if (res.ok) text = await res.text()
  } catch { /* prototype: 读不到就落回 DEFAULTS */ }
  if (text) {
    const sk = parseSkin(text)
    S.values = sk.vars
    S.scheme = sk.scheme
    // 皮肤没写 color-scheme 时，初值是按 --md-bg 亮度推的；记下来好在界面上标注
    S.schemeAuto = sk.scheme
    S.schemeInferred = !sk.schemeExplicit
  } else {
    S.values = { ...DEFAULTS }
    S.scheme = 'light'
    S.schemeAuto = 'light'
    S.schemeInferred = true
  }
}

/* 载入时的解析值（不看 overrides）：用来判断「值是不是真的变了」。
   「改过」的判据必须是值不同，不是被点过——否则取消透明勾选后写回的
   transparent 跟原值一样，却仍算一处改动、还点不灭「有改动」徽标。 */
function resolvedBase(name, depth = 0) {
  const raw = String(S.values[name] ?? DEFAULTS[name] ?? '').trim()
  const m = /^var\(\s*--md-([a-z0-9-]+)\s*\)$/.exec(raw)
  if (!m || depth > 8) return raw
  return resolvedBase(m[1], depth + 1)
}

function setValue(name, value) {
  const v = String(value).trim()
  if (v === resolvedBase(name)) delete S.overrides[name]
  else S.overrides[name] = v
  applyAll()
  markChanged()
  // 派生行的色块是按「主色 × 页面背景」算出来的，主色一动就要重画
  syncDerivedSwatches()
}

function markChanged() {
  const dirty = Object.keys(S.overrides).length > 0
  document.querySelectorAll('[data-var]').forEach((row) => {
    row.classList.toggle('changed', row.dataset.var in S.overrides)
  })
  const badge = document.getElementById('dirty')
  if (badge) badge.hidden = !dirty
  // 内置模板只读：原型里没有用户皮肤，所以「保存」一直禁用；
  // 真编辑器里它只在打开的是用户皮肤时可用。
  document.querySelectorAll('[data-act="save"]').forEach((b) => { b.disabled = true })
}

/* 皮肤里大量默认值是 var(--上级)：色块要显示「最终是什么颜色」，不能显示 var 文本。
   顺着链往下找字面值，并记住它继承自谁（表单上标 ↳，说明改上级会带动它）。 */
function rawOf(name) {
  return S.overrides[name] ?? S.values[name] ?? DEFAULTS[name] ?? ''
}

function resolveVar(name, depth = 0) {
  const raw = rawOf(name)
  const m = /^var\(\s*--md-([a-z0-9-]+)\s*\)$/.exec(String(raw).trim())
  if (!m || depth > 8) return { raw, value: raw, from: null }
  const parent = m[1]
  const r = resolveVar(parent, depth + 1)
  return { raw, value: r.value, from: r.from || parent }
}

/* ---------------- 控件 ---------------- */

/* 悬停提示：变量名 + 继承关系 + 影响哪些元素。
   「影响」一律从 RULES 反查；查不到的那 10 项用手写的 HINT 兜底。 */
function tipOf(name, from) {
  const bits = ['--md-' + name]
  if (from) bits.push('继承 --md-' + from + '，改上级会跟着变')
  const els = VAR_TIPS[name]
  if (els) bits.push('影响：' + els.join('、'))
  else if (HINT[name]) bits.push(HINT[name])
  return bits.join('\n')
}

/* transparent / currentColor 不是颜色：色块表达不了，用勾选框覆盖。
   勾上写关键字并记住上一个颜色，取消即恢复——这个「上一个颜色」只在内存里。 */
function specialRow(slot, name, value, txt, sw) {
  const kw = value === 'transparent' ? 'transparent' : value === 'currentColor' ? 'currentColor' : null
  if (!kw) return
  sw.disabled = true
  sw.classList.add('kw')

  const lbl = el('label', 'tgl')
  const chk = el('input')
  chk.type = 'checkbox'
  chk.checked = true
  lbl.append(chk, el('span', null, kw === 'transparent' ? '透明' : '跟随'))
  lbl.title = kw === 'transparent'
    ? '当前是 transparent：透出下一层（页面或代码块底色）。取消勾选会恢复到上一个颜色。'
    : '当前是 currentColor：跟随该处的文字色（引用块里就是引用文字色）。'

  chk.addEventListener('change', () => {
    if (chk.checked) {
      const prev = resolveVar(name).value
      if (isColorVal(prev)) S.lastColor[name] = prev
      setValue(name, kw)
      txt.value = kw
    } else {
      const back = S.lastColor[name] || (kw === 'currentColor'
        ? resolveVar('fg').value
        : (isColorVal(resolveVar('bg').value) ? resolveVar('bg').value : '#ffffff'))
      setValue(name, back)
      txt.value = back
    }
    sw.disabled = chk.checked
  })
  slot.append(lbl)
}

/* 左侧行与右侧预览的联动：悬停点亮、点击定位。派生行与普通行共用。 */
function bindRowLink(row, name) {
  row.addEventListener('mouseenter', () => {
    if (!S.link) return
    highlightVar(name)
    S.hoverVarName = name
  })
  row.addEventListener('mouseleave', () => {
    if (S.hoverVarName === name) {
      S.hoverVarName = null
      highlightVar(null)
    }
  })
  row.addEventListener('click', () => {
    const d = frameDoc()
    if (!d) return
    d.querySelectorAll('.__sel').forEach((n) => n.classList.remove('__sel'))
    let first = null
    for (const r of RULES) {
      if (!r.vars.includes(name)) continue
      const node = d.querySelector(r.sel)
      if (node) { node.classList.add('__hl'); first = first || { r, node } }
    }
    if (first) {
      first.node.classList.add('__sel')
      first.node.scrollIntoView({ block: 'center' })
    }
  })
  return row
}

const ALERT_BG_RE = /^alert-([a-z]+)-bg$/
const NL = String.fromCharCode(10)

/* 派生行：文件里是 color-mix，界面上是一句「跟随主色的 12%」。
   主色一变，预览自己会变（CSS 级联），这里只负责把色块重画一下。 */
function rowDerivedBg(name, der) {
  const row = el('div', 'row derived')
  row.dataset.var = name
  row.title = '--md-' + name + '\n跟随 --md-' + der.from + ' 的 ' + der.pct +
    '%：改主色自动变，改这里的百分比就是改系数\n值：' + rawOf(name)

  const accent = hexOf(resolveVar(der.from).value)
  const bg = hexOf(resolveVar('bg').value)
  const sw = el('input', 'sw')
  sw.type = 'color'
  sw.value = accent && bg ? blendOver(accent, bg, Number(der.pct) / 100) : (accent || '#ffffff')
  sw.dataset.derived = der.from
  sw.title = '点色块选一个固定颜色 → 断开派生'
  row.append(sw)

  const key = el('span', 'k', NAMES[name] || name)
  const inh = el('i', 'inhmark', '↳')
  inh.title = '跟随 --md-' + der.from
  key.append(inh)
  row.append(key)

  const pct = el('input', 'v pct')
  pct.value = der.pct + '%'
  pct.spellcheck = false
  pct.title = '不透明度（0–100）；写在这里的百分比会原样进文件'
  row.append(pct)

  const slot = el('span', 'slot')
  row.append(slot)

  const writePct = () => {
    const n = parseFloat(pct.value)
    if (!Number.isFinite(n) || n < 0 || n > 100) return
    const v = 'color-mix(in srgb, var(--md-' + der.from + ') ' + n + '%, transparent)'
    setValue(name, v)
    row.title = ['--md-' + name,
      '跟随 --md-' + der.from + ' 的 ' + n + '%：改主色自动变，改这里的百分比就是改系数',
      '值：' + v].join(NL)
  }
  pct.addEventListener('input', writePct)
  pct.addEventListener('blur', () => { pct.value = (derivedOf(rawOf(name)) || der).pct + '%' })
  sw.addEventListener('input', () => {
    setValue(name, sw.value)
    row.replaceWith(rowNormal(name))
  })
  return bindRowLink(row, name)
}

/* 提示框底色还是固定色时，槽里给一个「改为跟随主色」的入口。 */
function relinkBtn(name) {
  const fam = ALERT_BG_RE.exec(name)[1]
  const b = el('button', 'relink', '↳')
  b.type = 'button'
  b.title = '改为跟随主色：--md-alert-' + fam + ' 的 12%'
  b.addEventListener('click', (e) => {
    e.stopPropagation()
    setValue(name, 'color-mix(in srgb, var(--md-alert-' + fam + ') 12%, transparent)')
    render()
  })
  return b
}

function rowNormal(name) {
  const { raw, value, from } = resolveVar(name)
  const der = derivedOf(rawOf(name))
  if (der) return rowDerivedBg(name, der)

  const row = el('div', 'row' + (from ? ' inh' : ''))
  row.dataset.var = name
  row.title = tipOf(name, from)

  const color = isColorVal(value)
  const sw = el('input', 'sw' + (color ? '' : ' kw'))
  sw.type = 'color'
  sw.value = hexOf(value) || '#ffffff'
  if (!color) sw.title = '这个值不是颜色（' + value + '）'
  row.append(sw)

  const key = el('span', 'k', NAMES[name] || name)
  if (from) {
    const inh = el('i', 'inhmark', '↳')
    inh.title = '继承自 --md-' + from
    key.append(inh)
  }
  row.append(key)

  const txt = el('input', 'v')
  // 显示解析后的值（色块旁边那个 hex），var() 原文放 title：输入框写不下被截断反而更糊涂
  txt.value = value
  txt.title = from ? raw : ''
  txt.spellcheck = false

  sw.addEventListener('input', () => {
    txt.value = sw.value
    setValue(name, sw.value)
  })
  txt.addEventListener('input', () => setValue(name, txt.value.trim()))

  row.append(txt)

  // 勾选槽固定宽度：非特殊值行也留空，否则同组的值框右边界会参差
  const slot = el('span', 'slot')
  row.append(slot)
  specialRow(slot, name, value, txt, sw)
  if (ALERT_BG_RE.test(name)) slot.append(relinkBtn(name))

  return bindRowLink(row, name)
}

function rowCompact(name) {
  const { raw, value, from } = resolveVar(name)
  const row = el('label', 'prow')
  row.dataset.var = name
  const sw = el('input', 'psw' + (isColorVal(value) ? '' : ' kw'))
  sw.type = 'color'
  sw.value = hexOf(value) || '#ffffff'
  sw.addEventListener('input', () => setValue(name, sw.value))
  row.append(sw, el('span', 'pn', NAMES[name] || name))
  row.title = '--md-' + name + ': ' + raw + (from ? '（继承 --md-' + from + '）' : '')
  row.addEventListener('mouseenter', () => highlightVar(name))
  row.addEventListener('mouseleave', () => highlightVar(null))
  return row
}

function buildForm(compact) {
  const box = el('div')
  GROUPS.forEach((g, gi) => {
    // 表单顺序按 DISPLAY 排（字在前、背景在后）；没列到的仍按契约顺序接在后面，
    // 免得将来加变量忘了登记就从表单里消失。
    const ordered = (DISPLAY[g.title] || []).filter((n) => g.names.includes(n))
    const rest = g.names.filter((n) => !ordered.includes(n))
    const names = ordered.concat(rest)
    const title = GROUP_TITLES[g.title] || g.title

    const sec = el('section', 'grp')
    sec.dataset.total = String(names.length)
    sec.dataset.grp = title
    if (!compact) {
      // 默认只展开第一组；之后每组记住自己的状态（切皮肤重建表单不丢）
      if (!(title in S.open)) S.open[title] = S.openInit === 'all' ? true : S.openInit === 'none' ? false : gi === 0
      sec.classList.toggle('closed', !S.open[title])

      // 组头用 div：里面还要放「一次生成」按钮，button 套 button 是非法结构
      const head = el('div', 'grp-h')
      head.tabIndex = 0
      head.append(el('span', 'chev', '▾'), el('span', 'grp-t', title),
                  el('span', 'grp-n', String(names.length)))
      const body = el('div', 'grp-b')
      const toggle = () => {
        const closed = sec.classList.toggle('closed')
        S.open[title] = !closed
      }
      head.addEventListener('click', toggle)
      head.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggle() }
      })
      sec.append(head, body)
      for (const n of names) body.append(rowNormal(n))
    } else {
      sec.append(el('div', 'pgrp-t', title))
      const grid = el('div', 'pgrid')
      for (const n of names) grid.append(rowCompact(n))
      sec.append(grid)
    }
    box.append(sec)
  })
  const empty = el('div', 'empty')
  empty.hidden = true
  box.append(empty)
  return box
}

/* 动作按钮的文案集中在 ACTS：改一处，四个变体一起变。
   「…」只给会弹对话框的（打开/另存为）；「皮肤目录」是弹层，不带省略号。 */
const ACTS = {
  open: ['打开皮肤…', '打开一份 .css 皮肤文件'],
  save: ['保存', '模板是只读的：先「另存为…」存成自己的皮肤，之后才能直接保存'],
  saveAs: ['另存为…', '存成一份新的皮肤文件'],
  reset: ['恢复模板', '丢掉全部改动，回到所选模板的原始值'],
  lang: ['中 / EN', '切换界面语言'],
  about: ['关于', '版本、资产目录与第三方许可'],
  drawer: ['☰ 表单', '收起或展开表单栏'],
  more: ['⋯', '更多'],
}

function act(kind, primary) {
  const [label, title] = ACTS[kind]
  const b = el('button', 'ghostbtn' + (primary ? ' primary' : ''), label)
  b.type = 'button'
  b.title = title
  b.dataset.act = kind
  if (kind === 'save') b.disabled = true
  return b
}

/* ---------------- 表单：搜索与折叠 ---------------- */

/* 左栏顶部那一行：搜索框 + 「仅改动」+ 一键展开/折叠。
   搜索匹配中文名，也匹配 --md-xxx（写 CSS 的人按变量名找）。 */
function formTools() {
  const bar = el('div', 'rail-tools')

  const wrap = el('div', 'search')
  const input = el('input', 'search-in')
  input.type = 'search'
  input.placeholder = '搜索变量名或 --md-xxx'
  input.spellcheck = false
  input.value = S.query
  const count = el('span', 'search-n')
  wrap.append(input, count)

  const only = el('button', 'ghostbtn foldbtn', '仅改动')
  only.type = 'button'
  only.title = '只显示改过的变量（与搜索叠加）'
  only.setAttribute('aria-pressed', String(S.onlyChanged))

  const fold = el('button', 'ghostbtn foldbtn', allOpen() ? '全部折叠' : '全部展开')
  fold.type = 'button'
  fold.title = '把所有分组一次收起或展开'

  input.addEventListener('input', () => {
    S.query = input.value
    applyFilter()
  })
  only.addEventListener('click', () => {
    S.onlyChanged = !S.onlyChanged
    only.setAttribute('aria-pressed', String(S.onlyChanged))
    applyFilter()
  })
  fold.addEventListener('click', () => {
    const next = !allOpen()
    document.querySelectorAll('section.grp').forEach((sec) => {
      S.open[sec.dataset.grp] = next
      sec.classList.toggle('closed', !next)
    })
    fold.textContent = next ? '全部折叠' : '全部展开'
    applyFilter()
  })

  bar.append(wrap, only, fold)
  return bar
}

function allOpen() {
  const vals = Object.entries(S.open).filter(([k]) => k in GROUP_TITLE_SET)
  return vals.length > 0 && vals.every(([, v]) => v)
}

function applyFilter() {
  const q = S.query.trim().toLowerCase()
  let total = 0
  let changedShown = 0
  document.querySelectorAll('section.grp').forEach((sec) => {
    let shown = 0
    sec.querySelectorAll('[data-var]').forEach((row) => {
      const name = row.dataset.var
      const hay = ((NAMES[name] || '') + ' --md-' + name).toLowerCase()
      const isChanged = row.classList.contains('changed')
      const hit = (!q || hay.includes(q)) && (!S.onlyChanged || isChanged)
      row.hidden = !hit
      if (hit) {
        shown++
        if (isChanged) changedShown++
      }
    })
    sec.hidden = shown === 0
    const cnt = sec.querySelector('.grp-n')
    if (cnt) cnt.textContent = (q || S.onlyChanged) ? shown + '/' + sec.dataset.total : sec.dataset.total
    // 搜索或过滤时把命中的组展开；否则回到这组自己的展开状态
    if (q || S.onlyChanged) sec.classList.toggle('closed', false)
    else sec.classList.toggle('closed', !S.open[sec.dataset.grp])
    total += shown
  })
  const n = document.querySelector('.search-n')
  if (n) n.textContent = q ? total + ' 项' : (S.onlyChanged ? changedShown + ' 项改动' : '')

  // 列表整片为空时必须说话，否则看着像加载失败
  const empty = document.querySelector('.empty')
  if (empty) {
    empty.hidden = total > 0
    empty.textContent = q
      ? '没有匹配「' + S.query.trim() + '」的变量'
      : '这套模板还没有改动：改任意一项，它就会出现在这里'
  }
}

function sourceSelect() {
  const sel = el('select', 'sel')
  const tpl = el('optgroup')
  tpl.label = '模板（程序自带）'
  for (const [id, label] of Object.entries(SKIN_LABELS)) {
    const o = el('option', null, label)
    o.value = id
    o.selected = id === S.skin
    tpl.append(o)
  }
  sel.append(tpl)
  sel.addEventListener('change', async () => {
    await loadSkin(sel.value)
    render()
  })
  return sel
}

function holdButton() {
  const b = el('button', 'ghostbtn', '按住看默认')
  b.type = 'button'
  b.title = '按住时暂时摘掉皮肤，看 base.css 的默认样式（松开还原）'
  const on = (e) => { e.preventDefault(); S.hold = true; applyAll(); b.setAttribute('aria-pressed', 'true') }
  const off = () => { S.hold = false; applyAll(); b.setAttribute('aria-pressed', 'false') }
  b.addEventListener('pointerdown', on)
  b.addEventListener('pointerup', off)
  b.addEventListener('pointerleave', off)
  b.addEventListener('pointercancel', off)
  return b
}

function widgetsButton() {
  const b = el('button', 'ghostbtn', '界面元素')
  b.type = 'button'
  b.title = '显示搜索条、大纲、自绘滚动条；它们会挤占正文宽度'
  b.setAttribute('aria-pressed', String(S.widgets))
  b.addEventListener('click', () => {
    S.widgets = !S.widgets
    b.setAttribute('aria-pressed', String(S.widgets))
    applyAll()
  })
  return b
}

function linkButton() {
  const b = el('button', 'ghostbtn', '联动')
  b.type = 'button'
  b.title = '悬停左侧变量点亮右侧元素；点右侧元素定位左侧变量'
  b.setAttribute('aria-pressed', String(S.link))
  b.addEventListener('click', () => {
    S.link = !S.link
    b.setAttribute('aria-pressed', String(S.link))
    if (!S.link) {
      highlightVar(null)
      const d = frameDoc()
      if (d) d.querySelectorAll('.__sel').forEach((n) => n.classList.remove('__sel'))
      clearUsed()
      showChip('', 0, 0, false)
    }
  })
  return b
}

function newFrame() {
  const f = el('iframe')
  const q = new URLSearchParams({ skin: S.skin, doc: S.sample,
    skinRoot: S.assetRoot + 'skins/', root: S.assetRoot })
  f.src = 'preview.html?' + q.toString()
  f.addEventListener('load', () => applyAll())
  S.frame = f
  return f
}

/* color-scheme 不是 ——md-* 变量：它只影响原生控件（复选框、滚动条、输入框）。
   两态开关，初值按 --md-bg 亮度推断；推断出来的值与坚持不改时都标在右侧。 */
function schemeRow() {
  const row = el('div', 'schemerow')
  row.append(el('span', 'slabel', '原生控件'))

  const seg = el('div', 'seg')
  for (const [v, label] of [['light', '浅'], ['dark', '深']]) {
    const b = el('button', 'segbtn', label)
    b.type = 'button'
    b.dataset.v = v
    b.setAttribute('aria-pressed', String(S.scheme === v))
    b.addEventListener('click', () => {
      S.scheme = v
      S.schemeInferred = false
      seg.querySelectorAll('.segbtn').forEach((x) =>
        x.setAttribute('aria-pressed', String(x.dataset.v === v)))
      row.querySelector('.shint').textContent = '已指定'
      applyAll()
    })
    seg.append(b)
  }
  row.append(seg)

  const hint = el('span', 'shint', S.schemeInferred ? '按背景亮度推断' : '')
  hint.title = '深色皮肤不写这一条，任务列表的勾选框会是亮白的'
  row.append(hint)
  return row
}

/* ---------------- 变体 ---------------- */

/* B 样式的顶栏：所有动作常驻 */
function topBar() {
  const top = el('div', 'topbar')
  top.append(sourceSelect(), el('span', 'grow'), linkButton(), widgetsButton(), holdButton())
  const reset = act('reset')
  reset.disabled = false
  reset.addEventListener('click', async () => { await loadSkin(S.skin); render() })
  top.append(reset, aboutButton())
  return top
}

/* 关于：宿主 hello 里带的程序目录、资产目录、皮肤目录。中英切换推迟后这只是个只读浮层。 */
function aboutButton() {
  const wrap = el('span', 'popwrap')
  const b = el('button', 'ghostbtn', '关于')
  b.type = 'button'
  b.title = '程序目录与资产路径'
  const pop = el('div', 'pop about')
  pop.hidden = true
  pop.append(el('div', 'pop-t', '皮肤编辑器'))
  const info = window.__skinedit || {}
  for (const [k, v] of [['程序目录', info.exe], ['资产目录', info.web], ['皮肤目录', info.skins]]) {
    const row = el('div', 'about-row')
    row.append(el('span', 'about-k', k), el('span', 'about-v', v || '—'))
    pop.append(row)
  }
  pop.append(el('div', 'pop-h', '程序目录不可写时直接拒绝启动；外部目录只读。'))
  b.addEventListener('click', (e) => { e.stopPropagation(); pop.hidden = !pop.hidden })
  document.addEventListener('click', () => { pop.hidden = true })
  wrap.append(b, pop)
  return wrap
}

function previewBox(floating) {
  const box = el('div', 'preview')
  box.id = 'preview-box'
  const chip = el('div', 'chip')
  chip.id = 'chip'
  chip.hidden = true
  box.append(newFrame(), chip)
  if (floating) {
    const tools = el('div', 'pv-tools')
    tools.append(linkButton(), widgetsButton(), holdButton())
    box.append(tools)
  }
  return box
}

function renderD() {
  const app = el('div', 'app')
  app.dataset.v = 'D'

  const rail = el('div', 'rail')
  const head = el('div', 'rail-head')
  const fname = el('span', 'fname', (SKIN_LABELS[S.skin] || S.skin) + '.css')
  fname.title = '当前皮肤（模板，只读）'
  const dirty = el('span', 'dirty', '有改动')
  dirty.id = 'dirty'
  dirty.hidden = true
  head.append(fname, dirty, el('span', 'grow'))
  const body = el('div', 'rail-body pane-scroll')
  body.append(buildForm(false))
  rail.append(head, formTools(), schemeRow(), body)

  const splitter = el('div', 'splitter')
  const app2 = el('div', 'appbody')
  app2.append(rail, splitter, previewBox())

  splitter.addEventListener('pointerdown', dragSplitter(rail, app2, 6))

  app.append(topBar(), app2)
  return app
}

function dragSplitter(rail, host, splitW) {
  return (e) => {
    const sp = e.currentTarget
    sp.setPointerCapture(e.pointerId)
    sp.classList.add('dragging')
    const startX = e.clientX
    const startW = rail.getBoundingClientRect().width
    const move = (ev) => {
      const maxW = Math.min(520, host.clientWidth - 420 - splitW)
      const w = Math.max(280, Math.min(maxW, startW + ev.clientX - startX))
      rail.style.flex = '0 0 ' + w + 'px'
      rail.style.width = w + 'px'
    }
    const up = () => {
      sp.classList.remove('dragging')
      sp.removeEventListener('pointermove', move)
      window.removeEventListener('pointerup', up)
    }
    sp.addEventListener('pointermove', move)
    window.addEventListener('pointerup', up)
  }
}

function render() {
  const host = document.getElementById('app')
  host.replaceChildren()
  host.append(renderD())
  markChanged()
  applyFilter()
}

function boot() {
  const p = new URLSearchParams(location.search)
  S.skin = p.get('skin') || 'light'
  // 开发时在 dev-server 下跑：资产根指到插件的 web，样例指到 tools 下。
  // 真实宿主不带这些参数，skinedit.localhost 映射的就是 assets 根。
  if (p.get('dev') === '1') {
    S.assetRoot = '/web/'
    S.sample = '/tools/skinedit/samples/skin-preview.md'
  }
  for (const pair of (p.get('override') || '').split(',')) {
    const i = pair.indexOf(':')
    if (i > 0) S.overrides[pair.slice(0, i)] = pair.slice(i + 1)
  }
  S.widgets = p.get('widgets') === '1'
  S.hold = p.get('hold') === '1'
  S.query = p.get('q') || ''
  S.openInit = p.get('fold') || 'first'
  S.onlyChanged = p.get('only') === '1'
  const hover = p.get('hover')

  // 宿主在脚本挂上前就可能推过 hello；再问一次不吃亏，重复推不会出事。
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', (e) => {
      if (e.data && e.data.type === 'hello') window.__skinedit = e.data
    })
    window.chrome.webview.postMessage({ type: 'hello?', href: location.href })
  }

  const pre = { ...S.overrides }
  loadSkin(S.skin).then(() => {
    Object.assign(S.overrides, pre)
    render()
    if (hover) setTimeout(() => highlightVar(hover), 2500)
  })
}

boot()
