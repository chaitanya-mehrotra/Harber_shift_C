import { defineConfig } from './frontend/node_modules/@playwright/test/index.mjs';
import original from './frontend/playwright.config.js';

export default defineConfig({
  ...original,
  testDir: './frontend/tests',
  outputDir: './build/test-results',
  webServer: {
    command: 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./run.ps1 -Port 5173',
    url: 'http://127.0.0.1:5173/health',
    reuseExistingServer: false,
  },
});
