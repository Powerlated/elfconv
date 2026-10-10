const { defineConfig } = require('@playwright/test');
const path = require('path');

const browserBuildDir = path.resolve(
  process.env.ELFCONV_BROWSER_BUILD_DIR || path.join(__dirname, '../../build/test/browser'),
);

module.exports = defineConfig({
  testDir: '.',
  testMatch: '*.spec.js',
  timeout: 120000,
  use: {
    baseURL: 'http://localhost:3000',
  },
  webServer: [
    {
      command: 'node test-server.js',
      port: 3000,
      reuseExistingServer: false,
      env: {
        TEST_PORT: '3000',
        SERVE_DIR: path.join(browserBuildDir, 'wasm-out'),
        TEST_HTML: 'test-main.html',
      },
    },
    {
      command: 'node test-server.js',
      port: 3001,
      reuseExistingServer: false,
      env: {
        TEST_PORT: '3001',
        SERVE_DIR: path.join(browserBuildDir, 'wasm-out-bash'),
        TEST_HTML: 'test-bash.html',
      },
    },
  ],
  projects: [
    {
      name: 'hello',
      testMatch: 'hello.spec.js',
      use: {
        browserName: 'chromium',
        baseURL: 'http://localhost:3000',
      },
    },
    {
      name: 'bash',
      testMatch: 'bash-busybox.spec.js',
      use: {
        browserName: 'chromium',
        baseURL: 'http://localhost:3001',
      },
    },
  ],
});
