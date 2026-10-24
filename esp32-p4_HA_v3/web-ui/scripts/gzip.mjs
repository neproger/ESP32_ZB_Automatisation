import { readFileSync, writeFileSync, existsSync } from 'node:fs'
import { gzipSync } from 'node:zlib'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'

// Сжимаем собранные ассеты в *.gz — прошивка отдаёт их с Content-Encoding: gzip.
const root = join(dirname(fileURLToPath(import.meta.url)), '..')
const ui = join(root, '..', 'web', 'ui')

for (const name of ['index.html', 'app.js', 'app.css']) {
  const src = join(ui, name)
  if (!existsSync(src)) continue
  const buf = readFileSync(src)
  const gz = gzipSync(buf, { level: 9 })
  writeFileSync(src + '.gz', gz)
  console.log(`gz ${name}: ${buf.length} -> ${gz.length}`)
}
