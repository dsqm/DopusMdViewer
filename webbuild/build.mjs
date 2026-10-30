/* ============================================================
   把 webbuild/src 打成 web\ 下的分发产物。

   产物：
     web\app.js        markdown-it + 6 插件 + DOMPurify + hljs core(19 语) + KaTeX + shell 逻辑
     web\mermaid.js    仅在出现 ```mermaid 时由 <script> 拉取
     web\katex.css     从 katex/dist 原样复制（相对 url(fonts/…) 保持不变）
     web\fonts\*.woff2 20 个字体

   手写资产（shell.html / base.css / skins\）不在这里生成，直接放在 web\。
   ============================================================ */

import { build } from 'esbuild'
import { cp, mkdir, readdir, stat, writeFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const here = path.dirname(fileURLToPath(import.meta.url))
const web = path.resolve(here, '..', 'web')
const nm = path.join(here, 'node_modules')

const TARGET = 'chrome110'

async function bundle(entry, outfile, extra = {}) {
  const result = await build({
    entryPoints: [entry],
    outfile,
    bundle: true,
    format: 'iife',
    target: TARGET,
    minify: true,
    legalComments: 'none',
    charset: 'utf8',
    logLevel: 'warning',
    ...extra,
  })
  if (result.warnings.length) {
    for (const w of result.warnings) console.warn('[warn]', w.text)
  }
  const kb = (await stat(outfile)).size / 1024
  console.log('  ' + path.relative(web, outfile).padEnd(16) + kb.toFixed(1) + ' KiB')
}

async function main() {
  await mkdir(web, { recursive: true })

  console.log('[1/3] app.js')
  await bundle(path.join(here, 'src', 'app.js'), path.join(web, 'app.js'))

  console.log('[2/3] mermaid.js')
  await bundle(path.join(here, 'src', 'mermaid.entry.mjs'), path.join(web, 'mermaid.js'), {
    globalName: 'mermaid',
  })

  console.log('[3/3] katex.css + fonts')
  const katexDist = path.join(nm, 'katex', 'dist')
  const css = await (await import('node:fs/promises')).readFile(
    path.join(katexDist, 'katex.min.css'), 'utf8')
  await writeFile(path.join(web, 'katex.css'), css, 'utf8')

  const fontsOut = path.join(web, 'fonts')
  await mkdir(fontsOut, { recursive: true })
  const fonts = (await readdir(path.join(katexDist, 'fonts'))).filter((f) => f.endsWith('.woff2'))
  for (const f of fonts) {
    await cp(path.join(katexDist, 'fonts', f), path.join(fontsOut, f))
  }
  console.log('  fonts/'.padEnd(18) + fonts.length + ' files')

  console.log('\n[OK] ' + web)
}

main().catch((e) => {
  console.error('[FAIL]', e.message)
  process.exitCode = 1
})
