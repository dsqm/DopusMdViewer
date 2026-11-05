/* 把 src/preview.mjs 打成 web/preview.js（单文件 IIFE）。
 *
 * 依赖装在 webbuild/node_modules 里，入口却属于 tools/skinedit，esbuild 默认
 * 从入口向上找 node_modules 找不到；nodePaths 把它指过去，源码就能继续留在
 * 编辑器自己的目录里，不必塞进 webbuild/src。
 */

import { createRequire } from 'node:module'
import path from 'node:path'
import { copyFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const root = path.resolve(here, '..', '..')
const nm = path.join(root, 'webbuild', 'node_modules')
const require = createRequire(path.join(root, 'webbuild', 'package.json'))
const { build } = require('esbuild')

const result = await build({
  entryPoints: [path.join(here, 'src', 'preview.mjs')],
  outfile: path.join(here, 'web', 'preview.js'),
  bundle: true,
  format: 'iife',
  target: 'chrome110',
  minify: true,
  legalComments: 'none',
  charset: 'utf8',
  nodePaths: [nm],
  logLevel: 'warning',
})
if (result.warnings.length) {
  for (const w of result.warnings) console.warn('[warn]', w.text)
  process.exitCode = 1
}

/* editor.js 用相对路径 import skinfile.mjs。库源码在 src/，但 WebView2 映射的是
   assets/（由 web/ 同步过去），所以这里复制一份到 web/。每次构建都重拷，
   不会出现「改了库、页面还在用旧副本」。 */
copyFileSync(path.join(here, 'src', 'skinfile.mjs'),
             path.join(here, 'web', 'skinfile.mjs'))
