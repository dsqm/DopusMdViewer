/* ============================================================
   KaTeX 的 TeX 兼容层。

   KaTeX 没有 physics / bussproofs 宏包。固定参数个数的命令交给 KaTeX
   原生宏（TEX_MACROS）；可选参数、变参、矩阵体和多行环境别名在交给
   KaTeX 之前改写成标准写法（prepareTex）。

   mathText() 与预览渲染共用同一份改写，复制出来的文字和屏幕上看到的一致。
   ============================================================ */

import katex from 'katex'
import 'katex/contrib/mhchem'
import katexPluginModule from '@vscode/markdown-it-katex'

// 这个包是带 __esModule 的 CJS，默认导入拿到的是 exports 对象
const katexPlugin = katexPluginModule.default || katexPluginModule

export const TEX_MACROS = {
  '\\abs': '\\left|#1\\right|',
  '\\norm': '\\left\\|#1\\right\\|',
  '\\bra': '\\left\\langle #1\\right|',
  '\\ket': '\\left|#1\\right\\rangle',
  '\\braket': '\\left\\langle #1\\right\\rangle',
  '\\comm': '\\left[#1,#2\\right]',
  '\\acomm': '\\left\\{#1,#2\\right\\}',
  '\\pb': '\\left\\{#1,#2\\right\\}',
  '\\ketbra': '\\left|#1\\right\\rangle\\!\\left\\langle #2\\right|',
  '\\op': '\\left|#1\\right\\rangle\\!\\left\\langle #2\\right|',
  '\\dyad': '\\left|#1\\right\\rangle\\!\\left\\langle #2\\right|',
  '\\eval': '\\left.#1\\right|',
  '\\vu': '\\hat{\\mathbf{#1}}',
  '\\vv': '\\vec{#1}',
  '\\va': '\\vec{#1}',
  '\\vb': '\\mathbf{#1}',
  '\\vr': '\\mathbf{r}',
  '\\Tr': '\\operatorname{Tr}',
  '\\tr': '\\operatorname{tr}',
  '\\var': '\\operatorname{Var}',
  '\\order': '\\mathcal{O}',
}

export const KATEX_OPTIONS = {
  throwOnError: false,
  strict: false,
  trust: false,
  // 只要 HTML：MathML 副本会让每块 DOM 翻倍，滚动时挂载成本高一半。
  // 复制走 mathText()（单独按 mathml 渲染取文本），不受影响。
  output: 'html',
  macros: TEX_MACROS,
}

// 读取 {…} / […] / (…) 的配对内容；转义字符整体跳过，避免 \{ \} 干扰计数。
function texGroup(src, at, open, close) {
  if (src[at] !== open) return null
  let depth = 0
  for (let i = at; i < src.length; i++) {
    const c = src[i]
    if (c === '\\') { i++; continue }
    if (c === open) depth++
    else if (c === close && --depth === 0) return { text: src.slice(at + 1, i), end: i + 1 }
  }
  return null
}

function texSkipSpace(src, at) {
  while (at < src.length && /\s/.test(src[at])) at++
  return at
}

// bussproofs 用 & 分隔同一层的多个前提；矩阵等环境里的 & 要原样保留
function texInferArg(src) {
  let out = ''
  let env = 0
  for (let i = 0; i < src.length; i++) {
    if (src.startsWith('\\begin', i)) { env++; out += '\\begin'; i += 5; continue }
    if (src.startsWith('\\end', i)) { env = Math.max(0, env - 1); out += '\\end'; i += 3; continue }
    if (src[i] === '&' && env === 0) { out += '\\quad '; continue }
    out += src[i]
  }
  return out
}

const TEX_ARITY = {
  dv: [2, 3], pdv: [2, 3], fdv: [2, 3],
  expval: [1, 2], ev: [1, 2],
  // mqty 必须带一个 (...) 参数：min 写成 0 会让缺参数时拼出 {undefined}，
  // 按「参数不完整就保留原文」处理才不会被吃掉半截命令。
  mel: [3, 3], infer: [2, 2], mqty: [1, 1],
}

function texExpand(name, opt, args) {
  switch (name) {
    case 'dv':
    case 'pdv':
    case 'fdv': {
      const d = name === 'pdv' ? '\\partial' : name === 'fdv' ? '\\delta' : 'd'
      const degree = opt || (args.length > 2 ? '2' : '')
      const sup = degree ? '^{' + degree + '}' : ''
      const num = d + sup + ' ' + args[0]
      const den = args.length > 2
        ? d + ' ' + args[1] + ' ' + d + ' ' + args[2]
        : d + ' ' + args[1] + sup
      return '\\frac{' + num + '}{' + den + '}'
    }
    case 'expval':
    case 'ev':
      return args.length > 1
        ? '\\left\\langle ' + args[1] + '\\middle|' + args[0] + '\\middle|' + args[1] + '\\right\\rangle'
        : '\\left\\langle ' + args[0] + '\\right\\rangle'
    case 'mel':
      return '\\left\\langle ' + args[0] + '\\middle|' + args[1] + '\\middle|' + args[2] + '\\right\\rangle'
    case 'mqty':
      return '\\begin{pmatrix}' + args[0] + '\\end{pmatrix}'
    case 'infer':
      // 证明树按「前提在上、结论在下」叠成假分数，规则名挂在右侧
      return '\\frac{' + texInferArg(args[1]) + '}{' + texInferArg(args[0]) + '}' +
        (opt ? '\\,(' + opt + ')' : '')
  }
  return ''
}

export function texRewrite(src) {
  const re = /\\(dv|pdv|fdv|expval|ev|mel|mqty|infer)(?![A-Za-z])/g
  let out = ''
  let last = 0
  let m
  while ((m = re.exec(src))) {
    const name = m[1]
    const start = m.index
    let at = start + m[0].length
    let opt = ''
    if (name === 'dv' || name === 'pdv' || name === 'fdv' || name === 'infer') {
      at = texSkipSpace(src, at)
      const o = texGroup(src, at, '[', ']')
      if (o) { opt = o.text; at = o.end }
    }
    const [min, max] = TEX_ARITY[name]
    const args = []
    if (name === 'mqty') {
      at = texSkipSpace(src, at)
      const g = texGroup(src, at, '(', ')')
      if (g) { args.push(g.text); at = g.end }
    } else {
      for (let k = 0; k < max; k++) {
        at = texSkipSpace(src, at)
        const g = texGroup(src, at, '{', '}')
        if (!g) break
        args.push(g.text)
        at = g.end
      }
    }
    if (args.length < min) {
      // 参数不完整时按原文保留，避免把用户写的半截命令吃掉
      at = start + m[0].length
      out += src.slice(last, at)
      last = at
      continue
    }
    out += src.slice(last, start) + texExpand(name, opt.trim(), args.map((a) => texRewrite(a)))
    last = at
    re.lastIndex = at
  }
  return out + src.slice(last)
}

export function prepareTex(tex) {
  return texRewrite(String(tex)
    .replace(/\\begin\{(multline\*?)\}/g, '\\begin{gathered}')
    .replace(/\\end\{(multline\*?)\}/g, '\\end{gathered}'))
}

function texLabel(src) {
  return src.replace(/\s+/g, ' ').trim()
    .replace(/&/g, '&amp;').replace(/"/g, '&quot;')
    .replace(/</g, '&lt;').replace(/>/g, '&gt;')
}

// 插件通过 engine.renderToString 调用，包一层让所有公式走同一套改写
export const katexEngine = {
  renderToString(tex, opts) {
    const src = prepareTex(tex)
    // HTML 输出不带 MathML，给根节点补 aria-label，读屏软件至少能读到 TeX 源
    return katex.renderToString(src, opts).replace('<span class="katex"',
      '<span class="katex" role="math" aria-label="' + texLabel(src) + '"')
  },
}

export function setupMath(md) {
  md.use(katexPlugin, { katex: katexEngine, macros: TEX_MACROS, ...KATEX_OPTIONS })
}
