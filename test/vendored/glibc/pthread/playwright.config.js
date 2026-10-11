const { defineConfig } = require(process.env.ELFCONV_PLAYWRIGHT_PACKAGE);
const path = require('path');

const root = path.resolve(__dirname, '../../../..');

module.exports = defineConfig({
  testDir: __dirname,
  testMatch: 'pthread.spec.js',
  timeout: 150000,
  workers: 1,
  use: {
    browserName: 'chromium',
    baseURL: 'http://localhost:3002',
  },
  webServer: {
    command: `${process.env.ELFCONV_PTHREAD_NODE} ${path.join(root, 'test/browser/test-server.js')}`,
    cwd: root,
    port: 3002,
    reuseExistingServer: false,
    env: {
      TEST_PORT: '3002',
      SERVE_DIR: process.env.ELFCONV_PTHREAD_SERVE_DIR,
      TEST_HTML: process.env.ELFCONV_PTHREAD_TEST_HTML,
    },
  },
});
