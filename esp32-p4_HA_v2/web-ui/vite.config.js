import { defineConfig } from 'vite'

// Дев: клиент на ПК, WS — на устройство. `VITE_WS_URL` переопределяет адрес,
// иначе берём хост из window.location (когда SPA отдаётся самим устройством).
export default defineConfig({
  server: {
    host: true,
    port: 5173,
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
})
