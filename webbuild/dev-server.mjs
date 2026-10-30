/* ============================================================
   开发用静态服务：只为在 Edge 里验收 web\，不随插件分发。

   与线上一致的地方只有 CSP —— 真实环境由 webres.cpp 以响应头下发，
   这里同样用响应头下发，保证在 Edge 里跑出来的违规数就是 WebView2 里会看到的。
   唯一差异是 connect-src：开发模式要 fetch 文档与皮肤，线上是 'none'。

   用法：
     node dev-server.mjs               → http://127.0.0.1:8731/web/shell.html
     ?doc=/samples/poc-test.md         → 直接渲染该文件（相对图片按所在目录解析）
     &skin=dark                        → 加载 web\skins\dark.css
   ============================================================ */

import { createServer } from 'node:http'
import { readFile } from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const root = path.resolve(here, '..') // 工程根：web\ 与 samples\ 都在它下面

const CSP = [
  "default-src 'none'",
  "script-src 'self'",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' https://mddoc.local data:",
  "font-src 'self' data:",
  "connect-src 'self'",
  "base-uri 'none'",
  "form-action 'none'",
  "frame-ancestors 'none'",
  "object-src 'none'",
].join('; ')

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.md': 'text/markdown; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
  '.woff2': 'font/woff2',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.webp': 'image/webp',
}

const port = Number(process.env.PORT || 8731)

const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1')
  const rel = decodeURIComponent(url.pathname === '/' ? '/web/shell.html' : url.pathname)
  const file = path.join(root, rel)

  if (!file.startsWith(root)) {
    res.writeHead(403).end('forbidden')
    return
  }

  try {
    const body = await readFile(file)
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
      'Content-Security-Policy': CSP,
      'Cache-Control': 'no-store',
    })
    res.end(body)
  } catch {
    res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' }).end('not found: ' + rel)
  }
})

server.listen(port, '127.0.0.1', () => {
  console.log('serve ' + root)
  console.log('open  http://127.0.0.1:' + port + '/web/shell.html?doc=/samples/poc-test.md')
})
