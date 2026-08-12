import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { viteSingleFile } from 'vite-plugin-singlefile'

/*
 * The build emits one file. design.md §10 puts the bundle on the device's
 * LittleFS and firmware/components/slate_editor seeds it there out of the
 * image, so a single document is what makes that seed one atomic write and
 * §10's "under 400 KB gzipped" one number the firmware build measures rather
 * than a sum somebody keeps up to date. There is no CDN and no second origin
 * to fetch a chunk from (ADR-3), so splitting would buy nothing here either.
 *
 * `npm run dev` serves the editor from this machine and proxies the API to a
 * real panel, because ADR-5 makes the device the only renderer: there is
 * nothing to develop against but a device. Point it somewhere else with
 * SLATE_DEVICE=192.168.1.42 npm run dev.
 */
/* This file is the one thing here that runs in Node rather than in a browser,
 * and one environment variable is not worth pulling @types/node into a
 * configuration whose `types` the application shares. */
declare const process: { env: Record<string, string | undefined> }

/* A panel answers to `slate-<mac>.local` (§4.3) and this file cannot know the
 * MAC, so the fallback below is a placeholder: `npm run dev` normally wants an
 * address or the panel's real name in SLATE_DEVICE. */
const device = process.env['SLATE_DEVICE'] ?? 'slate.local'

export default defineConfig({
  plugins: [react(), viteSingleFile()],
  build: {
    target: 'es2022',
    outDir: 'dist',
    emptyOutDir: true,
    cssCodeSplit: false,
    // The device is on a LAN and the gzipped size is what §10 budgets, so the
    // uncompressed inline warning is noise here.
    chunkSizeWarningLimit: 4096,
  },
  server: {
    proxy: {
      '/api': { target: `http://${device}`, changeOrigin: true, ws: true },
    },
  },
})
