// 皮肤读写的回归测试（开发期用，不进分发）。
//
// 主断言是往返等式：emitSkin(parseSkin(x)) 与 x 的 ":root" 起部分逐字相同。
// 其次是默认值表与 web/base.css 不漂、缺项补齐、重复取最后一条、值与写法原样往返、
// color-scheme 的读与推断、未识别声明不丢、坏文件拒绝、两种编码都能解码。
//
// 跑：node tools/skinedit/test/skinfile.test.mjs

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { GROUPS, ORDER, DEFAULTS, parseSkin, emitSkin, luminanceOf, decodeSkinBytes }
  from '../src/skinfile.mjs'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const ROOT = path.resolve(HERE, '..', '..', '..')
const SKINS = path.join(ROOT, 'web', 'skins')

let failed = 0
let passed = 0

function check(ok, label, detail) {
  if (ok) { passed++; return }
  failed++
  console.log('  FAIL ' + label + (detail ? '\n       ' + detail : ''))
}

function eq(actual, expect, label) {
  check(actual === expect, label, actual === expect ? '' :
    'expect ' + JSON.stringify(expect).slice(0, 200) + '\n       actual ' + JSON.stringify(actual).slice(0, 200))
}

/* ---- 1. 分组与默认值表 ---- */
console.log('[1] 分组与默认值')
eq(ORDER.length, 113, '共 113 个变量')
eq(Object.keys(DEFAULTS).length, 113, 'DEFAULTS 有 113 项')
eq(new Set(ORDER).size, 113, 'ORDER 无重复')
const missingDefault = ORDER.filter((n) => !(n in DEFAULTS))
eq(missingDefault.length, 0, '每个变量都有默认值', missingDefault.join(','))
const extraDefault = Object.keys(DEFAULTS).filter((n) => !ORDER.includes(n))
eq(extraDefault.length, 0, 'DEFAULTS 没有多余项', extraDefault.join(','))

/* 与 base.css 的 :root 对齐（改 base 忘改表，这里会先炸） */
const base = fs.readFileSync(path.join(ROOT, 'web', 'base.css'), 'utf8')
const baseRoot = /:root\s*\{([\s\S]*?)\n\}/.exec(base)
check(!!baseRoot, 'base.css 里能找到 :root')
const baseVars = {}
for (const m of (baseRoot ? baseRoot[1] : '').matchAll(/--md-([a-z0-9-]+)\s*:\s*([^;]+);/g)) {
  baseVars[m[1]] = m[2].replace(/\s+/g, ' ').trim()
}
eq(Object.keys(baseVars).length, 113, 'base.css :root 也是 113 项')
const drift = ORDER.filter((n) => baseVars[n] !== DEFAULTS[n])
eq(drift.length, 0, '默认值表与 base.css 一致',
  drift.map((n) => n + ': base=' + baseVars[n] + ' table=' + DEFAULTS[n]).join('; '))

/* ---- 2. 往返等式：四套内置皮肤 ---- */
console.log('[2] 往返等式')
for (const file of fs.readdirSync(SKINS).filter((f) => f.endsWith('.css')).sort()) {
  const text = fs.readFileSync(path.join(SKINS, file), 'utf8')
  const id = file.replace(/\.css$/, '')
  const parsed = parseSkin(text)
  const out = emitSkin({ ...parsed, id })
  const tail = text.slice(text.indexOf(':root'))
  eq(out.slice(out.indexOf(':root')), tail, file + ' 往返逐字一致（头注释除外）')
  // 头注释本来就只有 name/id 的皮肤，整篇也该逐字一致（light.css 带说明文字，天然不满足）
  if (text.slice(0, text.indexOf(':root')) === '/* name: ' + parsed.name + '\n   id: ' + id + ' */\n\n') {
    eq(out, text, file + ' 整篇逐字一致')
  }
  eq(parsed.filled.length, 0, file + ' 没有缺项')
  eq(parsed.duplicates.length, 0, file + ' 没有重复变量')
  eq(parsed.unknown.length, 0, file + ' 没有未识别声明')
  check(parsed.name.length > 0, file + ' 取到了显示名')
}

/* ---- 3. 缺项补齐 ---- */
console.log('[3] 缺项与重复')
const minimal = ':root {\n  --md-bg: #101010;\n}\n'
const p3 = parseSkin(minimal)
eq(p3.filled.length, 112, '缺 112 项被补齐')
eq(p3.vars.bg, '#101010', '写了的项保持原值')
eq(p3.vars.fg, DEFAULTS.fg, '缺的项落回默认值')
const out3 = emitSkin({ ...p3, name: 'mini', id: 'mini' })
eq((out3.match(/^  --md-/gm) || []).length, 113, '写出 113 行变量')

const dup = ':root {\n  --md-bg: #111111;\n  --md-bg: #222222;\n}\n'
const p4 = parseSkin(dup)
eq(p4.vars.bg, '#222222', '重复变量取最后一条')
eq(p4.duplicates.join(','), 'bg', '重复被记录下来')

/* ---- 4. 值原样往返 ---- */
console.log('[4] 值写法')
const weird = ':root {\n  --md-bg: rgba(10, 20, 30, 0.5);\n  --md-fg: #FFF !important;\n' +
  '  --md-link: var(--md-fg);\n  --md-hr: hsl(210 90% 40%);\n}\n'
const p5 = parseSkin(weird)
eq(p5.vars.bg, 'rgba(10, 20, 30, 0.5)', 'rgba 原样')
eq(p5.vars.fg, '#FFF !important', '!important 原样')
eq(p5.vars.link, 'var(--md-fg)', 'var() 原样')
eq(p5.vars.hr, 'hsl(210 90% 40%)', 'hsl() 原样')
const out5 = emitSkin({ ...p5, name: 'w', id: 'w' })
check(out5.includes('--md-fg: #FFF !important;'), '写出时保留 !important')
check(out5.includes('--md-hr: hsl(210 90% 40%);'), '写出时保留 hsl()')

/* ---- 5. color-scheme ---- */
console.log('[5] color-scheme')
eq(parseSkin(':root {\n  --md-bg: #282c34;\n}\n').scheme, 'dark', '深底推断 dark')
eq(parseSkin(':root {\n  --md-bg: #ffffff;\n}\n').scheme, 'light', '浅底推断 light')
eq(parseSkin(':root {\n  --md-bg: transparent;\n}\n').scheme, 'light', '推不出时取 light')
eq(parseSkin(':root {\n  color-scheme: dark;\n  --md-bg: #ffffff;\n}\n').scheme, 'dark',
  '显式声明优先于亮度推断')
eq(parseSkin(':root {\n  color-scheme: light;\n  --md-bg: #000000;\n}\n').scheme, 'light',
  '显式 light 不被深底推翻')
eq(parseSkin(':root {\n  --md-bg: #ffffff;\n}\n').schemeExplicit, false, '没写时 schemeExplicit=false')
eq(luminanceOf('rgb(0,0,0)'), null, '非 hex 推不出亮度')

/* ---- 6. 未识别声明 ---- */
console.log('[6] 未识别声明')
const odd = ':root {\n  --md-bg: #ffffff;\n  -webkit-font-smoothing: antialiased;\n  --md-custom-x: 1px;\n}\n'
const p7 = parseSkin(odd)
eq(p7.unknown.length, 2, '两条未识别声明都被留下')
const out7 = emitSkin({ ...p7, name: 'o', id: 'o' })
check(out7.includes('-webkit-font-smoothing: antialiased;'), '普通属性透传')
check(out7.includes('--md-custom-x: 1px;'), '未知 --md- 变量透传')
eq(parseSkin(out7).unknown.length, 2, '再读一次仍然只有这两条')

/* ---- 7. extra 与坏文件 ---- */
console.log('[7] extra 与坏文件')
const withExtra = ':root {\n  --md-bg: #ffffff;\n}\n\n/* 尾巴 */\n.measure { padding-top: 2em; }\n'
const p8 = parseSkin(withExtra)
eq(p8.extra, '\n\n/* 尾巴 */\n.measure { padding-top: 2em; }\n', 'extra 逐字保留')
const out8 = emitSkin({ ...p8, name: 'e', id: 'e' })
check(out8.endsWith(p8.extra), 'extra 原样接在 :root 之后')
eq(parseSkin(out8).extra, p8.extra, '再读一次 extra 不变')

let threw = false
try { parseSkin('body { color: red; }') } catch { threw = true }
check(threw, '没有 :root 时抛错')
threw = false
try { parseSkin(':root { --md-bg: #fff;') } catch { threw = true }
check(threw, ':root 括号不配对时抛错')

/* ---- 8. 编码 ---- */
console.log('[8] 编码')
const utf8 = new TextEncoder().encode(':root {\n  --md-bg: #ffffff; /* 中文注释 */\n}\n')
eq(decodeSkinBytes(utf8).includes('中文注释'), true, 'UTF-8 解码')
const gb = Uint8Array.from([0x3a, 0x72, 0x6f, 0x6f, 0x74, 0x7b, 0x2f, 0x2a, 0xd6, 0xd0, 0xce, 0xc4, 0x2a, 0x2f, 0x7d])
eq(decodeSkinBytes(gb).includes('中文'), true, 'GB18030 兜底解码')

/* ---- 汇总 ---- */
console.log('------------------------------------------------------------')
console.log(`${passed} 项通过，${failed} 项失败`)
process.exitCode = failed ? 1 : 0
