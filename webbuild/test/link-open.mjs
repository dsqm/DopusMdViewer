// 链接地址解析的回归测试（构建期用，不进包）。
//
// 背景：纯 Markdown 块不经过 DOMPurify（sanitizeBlock 对不含 "<" 的块直接返回），
// 链接上从来没有 data-external；点击时改为现场调用 resolveDocUrl。这里抠真实实现
// 逐组断言，防止再退回「把 a.png 这种相对路径直接交给宿主」。
//
// 跑：node test/link-open.mjs（统一入口 test/run-all.mjs）

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const APP = path.resolve(HERE, '..', 'src', 'app.js')

const src = fs.readFileSync(APP, 'utf8')

// 按花括号配对抠出一个顶层函数源码（与 img-zoom.mjs 同一套路）
function grab(name) {
  const at = src.indexOf('function ' + name + '(')
  if (at < 0) throw new Error('app.js 里找不到 ' + name)
  let depth = 0
  for (let i = src.indexOf('{', at); i < src.length; i++) {
    if (src[i] === '{') depth++
    else if (src[i] === '}') {
      depth--
      if (depth === 0) return src.slice(at, i + 1)
    }
  }
  throw new Error(name + ' 的花括号不配对')
}

// resolveDocUrl 读 state.docBase，注入一个可控的 state
const state = { docBase: 'https://mddoc.localhost/' }
const resolveDocUrl = new Function('state', grab('resolveDocUrl') + '; return resolveDocUrl')(state)

let failed = 0

function check(name, got, want) {
  if (got === want) {
    console.log('  v ' + name)
  } else {
    failed++
    console.log('  x ' + name + '  → ' + JSON.stringify(got) + '，期望 ' + JSON.stringify(want))
  }
}

console.log('链接地址解析 resolveDocUrl')

// ---- 相对路径：一律升成文档虚拟主机下的绝对地址 ----
check('相对图片', resolveDocUrl('zoom-big.png'), 'https://mddoc.localhost/zoom-big.png')
check('相对文档', resolveDocUrl('15-image-zoom.md'), 'https://mddoc.localhost/15-image-zoom.md')
check('当前目录', resolveDocUrl('.'), 'https://mddoc.localhost/')
check('上一级', resolveDocUrl('../x.md'), 'https://mddoc.localhost/x.md')
check('带锚点', resolveDocUrl('a.md#L10'), 'https://mddoc.localhost/a.md#L10')
check('中文与空格', resolveDocUrl('中文 文档.md'),
      'https://mddoc.localhost/%E4%B8%AD%E6%96%87%20%E6%96%87%E6%A1%A3.md')

// ---- 已经是绝对地址的：原样返回，不重复拼接 ----
check('https 外链', resolveDocUrl('https://example.com/a'), 'https://example.com/a')
check('http 外链', resolveDocUrl('http://example.com/a'), 'http://example.com/a')
check('协议相对', resolveDocUrl('//example.com/a'), '//example.com/a')
check('mailto', resolveDocUrl('mailto:a@b.c'), 'mailto:a@b.c')
check('页内锚点', resolveDocUrl('#sec'), '#sec')
check('空串', resolveDocUrl(''), '')

// ---- 文档目录带子路径时，相对链接要落在同一层 ----
state.docBase = 'https://mddoc.localhost/sub/'
check('子目录相对', resolveDocUrl('a.png'), 'https://mddoc.localhost/sub/a.png')
check('子目录上一级', resolveDocUrl('../b.png'), 'https://mddoc.localhost/b.png')

console.log('------------------------------------------------------------')
if (failed) {
  console.log(`${failed} 条失败`)
  process.exit(1)
}
console.log('全部通过')
