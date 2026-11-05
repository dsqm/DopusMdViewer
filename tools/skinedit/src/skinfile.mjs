// skinfile.mjs — 皮肤文件的读与写（编辑器用，纯函数，不碰 DOM 与文件系统）。
//
// 一份皮肤固定是三段：
//   <header>   /* name: 显示名
//                 id: 文件名主干 */
//   <root>     :root { color-scheme: …; 66 个 --md-* }
//   <extra>    :root 之后的原文，逐字带回
//
// 读：抽出 66 个变量、color-scheme、未识别声明与 extra，缺的补 DEFAULTS。
// 写：头注释 + :root（十个分组、固定顺序、全量 66 行）+ 未识别声明 + extra。
// 因此 emitSkin(parseSkin(x)) 与 x 的 ":root" 起部分逐字相同（头注释会被重写成
// 只有 name/id，模板里的说明文字不保留）。这条等式是 test/skinfile.test.mjs 的主断言。

/* 分组顺序即写出顺序，与 web/skins/light.css 一致。 */
export const GROUPS = [
  { title: '底与字', names: ['bg', 'bg-alt', 'fg', 'fg-muted', 'fg-heading', 'border'] },
  { title: '链接与交互', names: ['link', 'link-hover', 'danger', 'selection', 'hit', 'hit-current', 'mark-bg'] },
  { title: '代码', names: ['code-bg', 'code-fg', 'code-ln-fg', 'block-bg', 'block-fg', 'block-border'] },
  { title: '引用与表格', names: ['quote-bar', 'quote-fg', 'quote-bg',
                                 'table-bg', 'table-odd-bg', 'table-even-bg',
                                 'table-head-bg', 'table-head-fg', 'table-border', 'table-row-hover'] },
  { title: '提示框', names: ['alert-note', 'alert-note-bg', 'alert-tip', 'alert-tip-bg',
                             'alert-important', 'alert-important-bg',
                             'alert-warning', 'alert-warning-bg',
                             'alert-caution', 'alert-caution-bg'] },
  { title: '滚动条与界面件', names: ['scrollbar', 'scrollbar-hover', 'outline-bg', 'outline-fg'] },
  { title: '图标（工具条按钮与复制按钮）', names: ['icon', 'icon-hover', 'icon-bg'] },
  { title: '语法高亮 token', names: ['syn-keyword', 'syn-string', 'syn-comment', 'syn-number',
                                     'syn-builtin', 'syn-title', 'syn-type', 'syn-variable',
                                     'syn-name', 'syn-selector', 'syn-meta',
                                     'syn-deletion', 'syn-deletion-bg',
                                     'syn-addition', 'syn-addition-bg'] },
  { title: '列表、分隔线与脚注', names: ['list-marker', 'hr', 'footnote-fg'] },
  { title: '形状', names: ['radius', 'space-block'] },
]

export const ORDER = GROUPS.flatMap((g) => g.names)

/* 与 web/base.css 的 :root 一致，皮肤缺项就落回这里。测试会拿 base.css 比对防漂。
   list-marker 的 currentColor = 跟随正文（引用块里就是引用文字色）。 */
export const DEFAULTS = {
  bg: '#ffffff', 'bg-alt': '#f6f8fa', fg: '#1f2328', 'fg-muted': '#656d76',
  'fg-heading': '#1f2328', border: '#d0d7de',

  link: '#0969da', 'link-hover': '#0550ae', danger: '#cf222e', selection: '#b6d7ff',
  hit: '#fff3b0', 'hit-current': '#ffd33d', 'mark-bg': '#fff8c5',

  'code-bg': '#eff1f3', 'code-fg': '#1f2328', 'code-ln-fg': '#656d76',
  'block-bg': '#f6f8fa', 'block-fg': '#1f2328', 'block-border': '#d0d7de',

  'quote-bar': '#d0d7de', 'quote-fg': '#57606a', 'quote-bg': 'transparent',
  'table-bg': 'transparent', 'table-odd-bg': 'transparent', 'table-even-bg': '#f6f8fa',
  'table-head-bg': '#f6f8fa', 'table-head-fg': '#1f2328', 'table-border': '#d0d7de',
  'table-row-hover': 'transparent',

  'alert-note': '#0969da', 'alert-note-bg': '#ddf4ff',
  'alert-tip': '#1a7f37', 'alert-tip-bg': '#dafbe1',
  'alert-important': '#8250df', 'alert-important-bg': '#fbefff',
  'alert-warning': '#9a6700', 'alert-warning-bg': '#fff8c5',
  'alert-caution': '#cf222e', 'alert-caution-bg': '#ffebe9',

  scrollbar: '#c9d1d9', 'scrollbar-hover': '#adb5bd',
  'outline-bg': '#ffffff', 'outline-fg': '#1f2328',

  icon: '#1f2328', 'icon-hover': '#0969da', 'icon-bg': '#ffffff',

  'syn-keyword': '#cf222e', 'syn-string': '#0a3069', 'syn-comment': '#6e7781',
  'syn-number': '#0550ae', 'syn-builtin': '#0550ae', 'syn-title': '#8250df',
  'syn-type': '#953800', 'syn-variable': '#953800', 'syn-name': '#116329',
  'syn-selector': '#116329', 'syn-meta': '#6e7781',
  'syn-deletion': '#82071e', 'syn-deletion-bg': '#ffebe9',
  'syn-addition': '#116329', 'syn-addition-bg': '#dafbe1',

  'list-marker': 'currentColor', hr: '#d0d7de', 'footnote-fg': '#656d76',

  radius: '6px', 'space-block': '1em',
}

/* ---------------- 扫描 ---------------- */

/* 从 openIdx 处的 "{" 起找到配对的 "}"，跳过字符串与注释。返回 "}" 的下标。 */
function matchBrace(text, openIdx) {
  let depth = 0
  let inStr = null
  for (let i = openIdx; i < text.length; i++) {
    const c = text[i]
    const n = text[i + 1]
    if (inStr) {
      if (c === '\\') { i++; continue }
      if (c === inStr) inStr = null
      continue
    }
    if (c === '/' && n === '*') { i = text.indexOf('*/', i + 2); if (i < 0) return -1; i++; continue }
    if (c === '"' || c === "'") { inStr = c; continue }
    if (c === '{') depth++
    else if (c === '}') { depth--; if (depth === 0) return i }
  }
  return -1
}

function stripComments(s) {
  return s.replace(/\/\*[\s\S]*?\*\//g, '')
}

/* 顶层分号切分：:root 里不会出现嵌套花括号，按 ";" 切即可（值里的字符串已排除）。 */
function splitDecls(block) {
  const out = []
  let cur = ''
  let inStr = null
  for (let i = 0; i < block.length; i++) {
    const c = block[i]
    if (inStr) {
      cur += c
      if (c === '\\') { cur += block[++i] ?? ''; continue }
      if (c === inStr) inStr = null
      continue
    }
    if (c === '"' || c === "'") { inStr = c; cur += c; continue }
    if (c === ';') { out.push(cur); cur = ''; continue }
    cur += c
  }
  if (cur.trim()) out.push(cur)
  return out
}

/* #rrggbb / #rgb → 亮度 0..1；认不出返回 null。 */
export function luminanceOf(value) {
  const m = /^#([0-9a-f]{3,8})$/i.exec(String(value).trim())
  if (!m) return null
  let h = m[1]
  if (h.length === 3) h = h.split('').map((c) => c + c).join('')
  if (h.length !== 6 && h.length !== 8) return null
  const r = parseInt(h.slice(0, 2), 16) / 255
  const g = parseInt(h.slice(2, 4), 16) / 255
  const b = parseInt(h.slice(4, 6), 16) / 255
  return 0.2126 * r + 0.7152 * g + 0.0722 * b
}

/* ---------------- 读 ---------------- */

/**
 * @param {string} text 皮肤全文（已解码）
 * @returns {{name:string, id:string, scheme:'light'|'dark', schemeExplicit:boolean,
 *            vars:Record<string,string>, filled:string[], duplicates:string[],
 *            unknown:string[], extra:string}}
 */
export function parseSkin(text) {
  let src = String(text).replace(/^\uFEFF/, '')

  // 头注释：文件开头的第一个块注释，且含 name: 行
  let name = ''
  let id = ''
  const head = /^\s*\/\*([\s\S]*?)\*\//.exec(src)
  if (head && /\bname\s*:/.test(head[1])) {
    const n = /name\s*:\s*([^\n*]+)/.exec(head[1])
    if (n) name = n[1].trim()
    const d = /id\s*:\s*([^\n*]+)/.exec(head[1])
    if (d) id = d[1].trim()
    src = src.slice(0, head.index) + ' '.repeat(head[0].length) + src.slice(head.index + head[0].length)
  }

  const rootAt = src.indexOf(':root')
  if (rootAt < 0) throw new Error('no :root')
  const brace = src.indexOf('{', rootAt)
  if (brace < 0) throw new Error('no :root {')
  const end = matchBrace(src, brace)
  if (end < 0) throw new Error('unbalanced :root')

  const block = src.slice(brace + 1, end)

  const vars = {}
  const duplicates = []
  let schemeRaw = null
  const unknown = []

  for (const chunk of splitDecls(block)) {
    const clean = stripComments(chunk).trim()
    if (!clean) continue
    const colon = clean.indexOf(':')
    if (colon < 0) continue
    const prop = clean.slice(0, colon).trim()
    const value = clean.slice(colon + 1).replace(/\s+/g, ' ').trim()
    if (prop.startsWith('--md-')) {
      const key = prop.slice(5)
      if (!ORDER.includes(key)) { unknown.push(`${prop}: ${value};`); continue }
      if (key in vars) duplicates.push(key)
      vars[key] = value
    } else if (prop.toLowerCase() === 'color-scheme') {
      schemeRaw = value
    } else {
      unknown.push(`${prop}: ${value};`)
    }
  }

  const filled = []
  for (const key of ORDER) {
    if (!(key in vars)) { vars[key] = DEFAULTS[key]; filled.push(key) }
  }

  let scheme
  let schemeExplicit = false
  if (schemeRaw) {
    const low = schemeRaw.toLowerCase()
    if (low.includes('dark')) { scheme = 'dark'; schemeExplicit = true }
    else if (low.includes('light')) { scheme = 'light'; schemeExplicit = true }
  }
  if (!schemeExplicit) {
    const lum = luminanceOf(vars.bg)
    scheme = lum === null ? 'light' : (lum < 0.5 ? 'dark' : 'light')
  }

  const before = src.slice(0, rootAt)
  const after = src.slice(end + 1)
  const extra = before.trim() ? before.trimEnd() + '\n\n' + after : after

  return { name: name || '', id, scheme, schemeExplicit, vars, filled, duplicates, unknown, extra }
}

/* ---------------- 写 ---------------- */

/**
 * @param {{name:string,id:string,scheme:'light'|'dark',vars:Record<string,string>,
 *          unknown?:string[],extra?:string}} skin
 */
export function emitSkin(skin) {
  const { name, id, scheme, vars, unknown = [], extra = '' } = skin
  const lines = [':root {', `  color-scheme: ${scheme === 'dark' ? 'dark' : 'light'};`]
  for (const g of GROUPS) {
    lines.push('', `  /* —— ${g.title} —— */`)
    for (const n of g.names) {
      const v = vars[n] ?? DEFAULTS[n]
      lines.push(`  --md-${n}: ${v};`)
    }
  }
  if (unknown.length) {
    lines.push('', '  /* —— 未识别声明，原样保留 —— */')
    for (const u of unknown) lines.push(`  ${u}`)
  }
  lines.push('}')
  const body = `/* name: ${name}\n   id: ${id} */\n\n` + lines.join('\n')
  return body + (extra ? extra : '\n')
}

/* ---------------- 编码 ---------------- */

/* 字节 → 文本：UTF-8 严格解码失败再按 GB18030 试一次（记事本另存 ANSI 很常见）。 */
export function decodeSkinBytes(bytes) {
  try {
    return new TextDecoder('utf-8', { fatal: true }).decode(bytes)
  } catch {
    return new TextDecoder('gb18030', { fatal: true }).decode(bytes)
  }
}
