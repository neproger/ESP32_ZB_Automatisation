import { defineConfig } from 'vite'

// Сборка кладётся прямо в компонент прошивки (web/ui), откуда её встраивает
// EMBED_FILES. Имена файлов фиксированы (без хэша), чтобы CMake знал символы.
export default defineConfig({
  server: {
    host: true,
    port: 5173,
  },
  build: {
    outDir: '../web/ui',
    emptyOutDir: true,
    rollupOptions: {
      output: {
        entryFileNames: 'app.js',
        assetFileNames: 'app.[ext]',
        chunkFileNames: 'chunk-[name].js',
      },
    },
  },
})
