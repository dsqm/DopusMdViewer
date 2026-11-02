// 高亮切分的回归测试（构建期用，不进包）。
//
// 巨型代码块走的是「整块 hljs + 按行切分缓存」，窗口取用时直接拼字符串。切分算法一旦
// 写错，表现是续行丢色、缩进丢失、甚至行首多出裸标签文字——而这些在单行注释/单行字符串
// 的样本上完全看不出来。所以这里对插件注册的每种语言逐行比对：
//   ① 行数一致
//   ② 每行可见文本与源码逐字符相同（含前导空格、Tab、尾随空格、CJK、emoji、HTML 转义字符）
//   ③ 每行标签自闭合
//   ④ 跨行结构（多行注释 / 多行字符串 / 原始字符串 / 文本块）的续行仍带同一高亮类
//   ⑤ 逐行拼接回去与整块文本完全一致
//
// 直接读 src/app.js 抠真实实现，不复制一份逻辑来测，否则测的是另一份代码。
// 跑：node test/highlight-split.mjs

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { createRequire } from 'node:module'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const WEB = path.resolve(HERE, '..')
const APP = path.join(WEB, 'src', 'app.js')
const require = createRequire(path.join(WEB, 'package.json'))
const hljs = require('highlight.js')

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
  throw new Error(name + ' 的花括号不配对')
}

const closeTags = new Function('return ' + grab('closeTags'))()
const splitHighlightLines =
  new Function('closeTags', 'return ' + grab('splitHighlightLines'))(closeTags)
const displayLines = new Function('return ' + grab('displayLines'))()

// 与 app.js 的 LANGUAGES 保持同一组：漏一个就等于那门语言没测
const LANGUAGES = ['javascript', 'typescript', 'python', 'c', 'cpp', 'csharp', 'java', 'go',
  'rust', 'json', 'bash', 'xml', 'css', 'sql', 'yaml', 'ini', 'markdown', 'diff', 'powershell']

// 每种语言的样本都刻意带上：空格缩进、Tab 缩进、空行、只有空白的行、跨行结构、
// 引号与转义、CJK、emoji、行尾空格
const SAMPLES = {
  javascript: [
    'function outer() {',
    '  \tconst tpl = `第一行 ${name}',
    '  第二行 含 "引号" 与 \\ 反斜杠',
    '\t第三行 Tab 开头`',
    '',
    '  // 单行注释',
    '  /* 多行注释',
    '     第二行仍属注释 */',
    '  const re = /a\\/b[c]+/g',
    '  return tpl + re + 中文 + "😀"',
    '}',
    '  ',
  ],
  typescript: [
    'interface Box<T extends object> {',
    '\tvalue: T',
    '\tmap<U>(f: (x: T) => U): Box<U>',
    '}',
    '/* 多行',
    '   注释 */',
    'const s: string = `模板',
    '  跨行 ${v} 结尾`',
  ],
  python: [
    'def f(a, b):',
    '    """文档字符串',
    '    第二行，含 # 不是注释',
    '\tTab 开头的一行',
    '    """',
    '    s = "普通字符串"',
    "    t = '''三引号",
    "    仍属字符串'''",
    '    return a + b  # 尾注释',
    '',
    '  ',
  ],
  c: [
    '/* 多行注释开始',
    '   中间行 "引号" \'单引号\'',
    '   结束 */',
    'int main(void) {',
    '\tprintf("tab 缩进\\n");',
    '    char c = \'"\'',
    '    // 尾注释',
    '    return 0;',
    '}',
  ],
  cpp: [
    '#include <vector>',
    '/* 跨行注释',
    '   第二行 */',
    'const char* raw = R"tag(',
    '  原始字符串第一行',
    '  第二行含 "引号" 与 \\n',
    ')tag";',
    'int main() {',
    '\tstd::vector<int> v{1, 2};',
    '    return v.size();',
    '}',
  ],
  csharp: [
    '/* 注释',
    '   跨行 */',
    'var v = @"逐字字符串',
    '    第二行 \\ 不是转义";',
    'class A {',
    '\tvoid M() { }',
    '}',
  ],
  java: [
    '/* 注释跨行',
    '   第二行 */',
    'String b = """',
    '    文本块第一行',
    '    "引号" 第二行',
    '    """;',
    'class A {',
    '\tvoid m() {}',
    '}',
  ],
  go: [
    '/* 注释',
    '   第二行 */',
    'var s = `原始字符串',
    '  第二行 "引号"',
    '`',
    'func main() {',
    '\tprintln(s)',
    '}',
  ],
  rust: [
    '/* 外层注释',
    '   /* 嵌套注释 */',
    '   仍在注释里 */',
    'fn main() {',
    '\tlet s = r#"原始',
    '  第二行"#;',
    '    println!("{}", s);',
    '}',
  ],
  json: [
    '{',
    '  "a": 1,',
    '  "b": "含 \\"转义\\" 与 / 斜杠",',
    '  "c": [true, null, -1.5e10],',
    '  "d": "中文 😀"',
    '}',
  ],
  bash: [
    '#!/bin/bash',
    '# 注释',
    'cat <<EOF',
    '  第一行 $VAR',
    '\t第二行 Tab',
    'EOF',
    'echo "含 \\" 引号" \\',
    '  && echo 续行',
  ],
  xml: [
    '<!-- 注释',
    '     跨行 -->',
    '<root attr="含 &quot; 的值">',
    '  <child>文本 😀</child>',
    '\t<indented />',
    '</root>',
  ],
  css: [
    '/* 注释',
    '   跨行 */',
    '@media (min-width: 10em) {',
    '  .a::before {',
    '    content: "含 \\" 引号";',
    '\t\tbackground: url("a b.png");',
    '  }',
    '}',
  ],
  sql: [
    '-- 行注释',
    '/* 多行',
    '   注释 */',
    "SELECT '含 '' 单引号' AS s,",
    '       COUNT(*) AS n',
    'FROM t',
    'WHERE a = 1;',
  ],
  yaml: [
    'key: value',
    'block: |',
    '  第一行',
    '\t第二行 Tab',
    '  第三行',
    'list:',
    '  - "含 \\" 引号"',
    '  - 中文',
  ],
  ini: [
    '; 注释',
    '[section]',
    'a = 1',
    'b = 含空格的值',
    '  c = 缩进行',
    '# 井号注释',
  ],
  markdown: [
    '# 标题',
    '',
    '    四空格缩进代码块',
    '\tTab 缩进的行',
    '- 列表',
    '  续行',
    '> 引用',
    '```js',
    'const x = 1',
    '```',
  ],
  diff: [
    'diff --git a/a b/a',
    '--- a/a',
    '+++ b/a',
    '@@ -1,3 +1,4 @@',
    ' 上下文行',
    '-删除行',
    '+新增行',
    '\tTab 行',
  ],
  powershell: [
    '# 注释',
    '<# 块注释',
    '   跨行 #>',
    '$here = @"',
    '  第一行 $var',
    '  第二行 "引号"',
    '"@',
    'Write-Output $here',
  ],
}

// 续行探针：这句话在源码里是某个跨行结构的第 2 行及以后，必须仍带该高亮类
const CONTINUATION = {
  javascript: { needle: '第二行 含', cls: 'hljs-string' },
  typescript: { needle: '跨行 ${v}', cls: 'hljs-string' },
  python: { needle: '仍属字符串', cls: 'hljs-string' },
  c: { needle: '中间行', cls: 'hljs-comment' },
  cpp: { needle: '原始字符串第一行', cls: 'hljs-string' },
  csharp: { needle: '第二行 \\ 不是转义', cls: 'hljs-string' },
  java: { needle: '文本块第一行', cls: 'hljs-string' },
  go: { needle: '第二行 "引号"', cls: 'hljs-string' },
  rust: { needle: '仍在注释里', cls: 'hljs-comment' },
  bash: { needle: '第二行 Tab', cls: 'hljs-string' },
  xml: { needle: '跨行', cls: 'hljs-comment' },
  css: { needle: '跨行 */', cls: 'hljs-comment' },
  sql: { needle: '注释 */', cls: 'hljs-comment' },
  yaml: { needle: '第二行 Tab', cls: 'hljs-string' },
  powershell: { needle: '第二行 "引号"', cls: 'hljs-string' },
}

// hljs 的转义：& < > " 与单引号（新版输出 &#x27;）
const unescape = (s) => s.replace(/&#x27;/g, "'").replace(/&#39;/g, "'")
  .replace(/&quot;/g, '"').replace(/&gt;/g, '>').replace(/&lt;/g, '<').replace(/&amp;/g, '&')
const stripTags = (html) => html.replace(/<[^>]*>/g, '')

let failures = 0
function fail(lang, msg) {
  failures++
  console.log('  x ' + lang + '：' + msg)
}

for (const lang of LANGUAGES) {
  if (!hljs.getLanguage(lang)) {
    fail(lang, 'highlight.js 里没有这门语言，样本等于没测')
    continue
  }
  const raw = SAMPLES[lang]
  if (!raw) {
    fail(lang, '缺少样本')
    continue
  }
  const code = raw.join('\n')
  const html = hljs.highlight(code, { language: lang, ignoreIllegals: true }).value
  const lines = splitHighlightLines(html)
  const bad = failures

  if (lines.length !== raw.length) fail(lang, `行数 ${lines.length} != ${raw.length}`)

  for (let i = 0; i < Math.min(lines.length, raw.length); i++) {
    const got = unescape(stripTags(lines[i]))
    if (got !== raw[i]) {
      fail(lang, `第 ${i + 1} 行文本不同：got=${JSON.stringify(got)} want=${JSON.stringify(raw[i])}`)
      break
    }
    const open = (lines[i].match(/<span[\s>]/g) || []).length
    const close = (lines[i].match(/<\/span>/g) || []).length
    if (open !== close) {
      fail(lang, `第 ${i + 1} 行标签未闭合 ${open}/${close}`)
      break
    }
  }

  // 缩进必须原样出现在行首（重开标签时最容易把空白挤丢）
  for (let i = 0; i < Math.min(lines.length, raw.length); i++) {
    const lead = raw[i].match(/^[ \t]*/)[0]
    if (!lead) continue
    if (!unescape(stripTags(lines[i])).startsWith(lead)) {
      fail(lang, `第 ${i + 1} 行缩进丢失：${JSON.stringify(stripTags(lines[i]).slice(0, 12))}`)
      break
    }
  }

  const probe = CONTINUATION[lang]
  if (probe) {
    const idx = raw.findIndex((l) => l.includes(probe.needle))
    if (idx < 0) fail(lang, '样本里找不到续行探针 ' + probe.needle)
    else if (!lines[idx].includes(probe.cls)) {
      fail(lang, '续行缺高亮类 ' + probe.cls + '：' + lines[idx].slice(0, 90))
    } else if (lines[idx].indexOf(probe.cls) > 40) {
      fail(lang, '续行高亮类出现太靠后，疑似断色：' + lines[idx].slice(0, 90))
    }
  }

  if (displayLines(code) !== raw.length) {
    fail(lang, `displayLines ${displayLines(code)} != ${raw.length}`)
  }

  const rejoined = unescape(lines.map((l) => stripTags(l)).join('\n'))
  if (rejoined !== code) {
    const a = code.split('\n')
    const b = rejoined.split('\n')
    let at = 0
    while (at < Math.max(a.length, b.length) && a[at] === b[at]) at++
    fail(lang, `整体还原不一致，首个差异第 ${at + 1} 行：` +
      `got=${JSON.stringify(b[at])} want=${JSON.stringify(a[at])}`)
  }

  console.log('  ' + (failures === bad ? 'v' : 'x') + ' ' + lang + '  ' + raw.length + ' 行')
}

// 退化输入：空块、只有换行、纯空白行
for (const [name, code] of [['空块', ''], ['单行', 'x'], ['只有换行', '\n\n\n'],
  ['纯空白行', '  \n\t\n  ']]) {
  const lines = splitHighlightLines(
    hljs.highlight(code, { language: 'cpp', ignoreIllegals: true }).value)
  const want = code.split('\n').length
  if (lines.length !== want) {
    fail(name, `${lines.length} 行 != ${want} 行`)
  } else {
    console.log('  v 退化：' + name + '  ' + lines.length + ' 行')
  }
}

console.log(failures ? '\n失败 ' + failures + ' 项' : '\n全部通过')
process.exit(failures ? 1 : 0)
