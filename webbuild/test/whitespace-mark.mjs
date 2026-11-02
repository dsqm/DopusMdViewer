// 空白符标记的回归测试（构建期用，不进包）。
//
// 空白标记错了，表现很隐蔽：缩进点画偏、行尾空白漏标、或者最要命的——
// 标记混进了文本节点，复制出来的代码里凭空多出「·」和「→」，查找也对不齐源码。
// 所以这里逐条比对：
//   ① 边界档的判定：行首缩进、行尾空白、行内连续两个以上的空格要标；
//      词与词之间的单个空格不标。
//   ② 全部档：所有空格与制表符都标。
//   ③ 剥掉标记后，文本必须与原文逐字符相同（这是「复制不带标记」的根据）。
//   ④ 标签混在里面时（高亮输出）判定不受影响。
//   ⑤ 逐字符与逐段两种粒度标的位置完全一致。
//   ⑥ 仅选中档：全部空白都包上，但一个都不常显（点亮交给选区）。
//
// 直接读 src/app.js 抠真实实现，不复制一份逻辑来测，否则测的是另一份代码。
// 跑：node test/whitespace-mark.mjs

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const WEB = path.resolve(HERE, '..')
const APP = path.join(WEB, 'src', 'app.js')

const src = fs.readFileSync(APP, 'utf8')

// 按花括号配对抠出一个顶层函数源码
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
  throw new Error(name + '的花括号不配对')
}

// 档位常量从 app.js 里读，不在测试里写死：编号一改（比如按常显多少重排），
// 测试跟着走，而不是变成两份互相打架的真相。
function grabConst(name) {
  const m = src.match(new RegExp('const ' + name + ' = (\\d+)'))
  if (!m) throw new Error('app.js 里找不到常量 ' + name)
  return parseInt(m[1], 10)
}
const WS_SEL = grabConst('WS_SEL')
const WS_BOUNDARY = grabConst('WS_BOUNDARY')
const WS_ALL = grabConst('WS_ALL')
const wrapWsChunk = new Function('return ' + grab('wrapWsChunk'))()
const wrapWsRun = new Function('wrapWsChunk', 'return ' + grab('wrapWsRun'))(wrapWsChunk)
// 抠出来的函数体里会用到档位常量，得逐个作为参数喂进去，漏一个就是 ReferenceError
const markWhitespace =
  new Function('wrapWsRun', 'WS_ALL', 'WS_BOUNDARY', 'return ' + grab('markWhitespace'))(
    wrapWsRun, WS_ALL, WS_BOUNDARY)

// 剥掉所有标签：只剩文本节点的内容
function plainText(html) {
  return html.replace(/<[^>]*>/g, '')
}

// 把每个空白字符按「是否常显」标出来：常显的是 ·（制表符是 →），不常显的是 _。
// 换行不在标记里，原样留着；其余标签（高亮的 span）剥掉，只看文本。
function markPattern(html) {
  const marked = html.replace(/<span class="(mdv-w[^"]*)">([ \t]*)<\/span>/g, (m, cls, ws) => {
    const on = cls.indexOf('mdv-on') >= 0 || cls.indexOf('mdv-sel') >= 0
    const tab = cls.indexOf('wt') >= 0
    return ws.replace(/[ \t]/g, (c) => (on ? (tab ? '→' : '·') : '_'))
  })
  return marked.replace(/<[^>]*>/g, '')
}

let failed = 0

function check(name, cond, detail) {
  if (cond) {
    console.log('  v ' + name)
  } else {
    failed++
    console.log('  x ' + name + (detail ? '  → ' + detail : ''))
  }
}

function same(a, b) {
  return a === b
}

console.log('空白符标记')

// ---- ① 边界档的判定 ----
{
  const code = 'if (x) {\n    return "a b";\n}\n'
  const html = markWhitespace(code, WS_BOUNDARY, true)
  const want = 'if_(x)_{' + '\n' + '····return_"a_b";' + '\n' + '}' + '\n'
  check('边界档只标行首缩进', same(markPattern(html), want),
    JSON.stringify(markPattern(html)))
  check('剥掉标记后与原文一致', plainText(html) === code, JSON.stringify(plainText(html)))
}

{
  const code = 'a  \nb'
  const html = markWhitespace(code, WS_BOUNDARY, true)
  check('行尾空白标上', same(markPattern(html), 'a··' + '\n' + 'b'),
    JSON.stringify(markPattern(html)))
}

{
  const code = 'a  b'
  const html = markWhitespace(code, WS_BOUNDARY, true)
  check('行内连续两个空格标上', same(markPattern(html), 'a··b'),
    JSON.stringify(markPattern(html)))
}

{
  const code = 'a b'
  const html = markWhitespace(code, WS_BOUNDARY, true)
  check('词间单个空格不标', same(markPattern(html), 'a_b'),
    JSON.stringify(markPattern(html)))
}

{
  const code = '\ta\tb\t'
  const html = markWhitespace(code, WS_BOUNDARY, true)
  // 行首制表符与行尾制表符算边界，中间的单个制表符不算
  check('制表符的边界判定与空格一致',
    same(markPattern(html), '→a_b→'), JSON.stringify(markPattern(html)))
  check('制表符剥掉后与原文一致', plainText(html) === code)
  check('制表符用的是箭头类名', html.includes('mdv-wt'))
}

{
  const code = 'a\n   \nb'
  const html = markWhitespace(code, WS_BOUNDARY, true)
  check('纯空白行的空白标上（既贴行首也贴行尾）',
    same(markPattern(html), 'a' + '\n' + '···' + '\n' + 'b'),
    JSON.stringify(markPattern(html)))
}

// ---- ② 全部档 ----
{
  const code = 'if (x) {\n    return "a b";\n}\n'
  const html = markWhitespace(code, WS_ALL, true)
  const want = 'if·(x)·{' + '\n' + '····return·"a·b";' + '\n' + '}' + '\n'
  check('全部档标所有空白', same(markPattern(html), want), JSON.stringify(markPattern(html)))
  check('全部档剥掉标记后与原文一致', plainText(html) === code)
}

// ---- ③ 标签混在里面（高亮输出） ----
{
  const code = '<span class="hljs-keyword">if</span> (x) {\n' +
    '    <span class="hljs-string">"a b"</span>\n}\n'
  const html = markWhitespace(code, WS_BOUNDARY, true)
  check('带标签时边界判定不受影响',
    same(markPattern(html), 'if_(x)_{' + '\n' + '····"a_b"' + '\n' + '}' + '\n'),
    JSON.stringify(markPattern(html)))
  check('带标签时文本内容不变', plainText(html) === plainText(code),
    JSON.stringify(plainText(html)))
}

// ---- ④ 两种粒度标的位置一致 ----
{
  const code = 'a b\n    c\n'
  const perChar = markWhitespace(code, WS_BOUNDARY, true)
  const perRun = markWhitespace(code, WS_BOUNDARY, false)
  check('两种粒度标的位置完全一致', markPattern(perChar) === markPattern(perRun),
    JSON.stringify(markPattern(perChar)) + ' vs ' + JSON.stringify(markPattern(perRun)))
  check('逐段用独立的类名', perRun.includes('mdv-wrs'), perRun.slice(0, 120))
  check('逐段剥掉标记后与原文一致', plainText(perRun) === code)
}

// ---- ⑥ 仅选中档：全包、全不常显 ----
{
  const code = 'if (x) {\n    return "a b";\n}\n'
  const html = markWhitespace(code, WS_SEL, true)
  // 与全部档同位置同数量，区别只在没有一个 mdv-on
  const want = 'if_(x)_{' + '\n' + '____return_"a_b";' + '\n' + '}' + '\n'
  check('仅选中档包住所有空白但不常显', same(markPattern(html), want),
    JSON.stringify(markPattern(html)))
  check('仅选中档没有一个 mdv-on', html.indexOf('mdv-on') < 0, html.slice(0, 120))
  check('仅选中档剥掉标记后与原文一致', plainText(html) === code)

  // 与全部档逐字对比：只差 mdv-on 这一个类名，说明包的位置完全一致
  const all = markWhitespace(code, WS_ALL, true)
  check('仅选中档与全部档只差 mdv-on', html === all.replace(/ mdv-on/g, ''),
    html.slice(0, 120))

  const perRun = markWhitespace(code, WS_SEL, false)
  check('仅选中档逐段也不常显', perRun.indexOf('mdv-on') < 0 && perRun.includes('mdv-wrs'),
    perRun.slice(0, 120))
  check('仅选中档逐段剥掉标记后与原文一致', plainText(perRun) === code)
}

// ---- ⑦ 关闭时原样返回 ----
{
  const code = 'a b\n'
  check('档位为 0 时不加工', markWhitespace(code, 0, true) === code)
}

if (failed) {
  console.log('\n失败 ' + failed + ' 项')
  process.exit(1)
}
console.log('\n全部通过')
