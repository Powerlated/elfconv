const { test, expect } = require(process.env.ELFCONV_PLAYWRIGHT_PACKAGE);
const fs = require('fs');

const nativeStdout = fs.readFileSync(process.env.ELFCONV_PTHREAD_NATIVE_STDOUT, 'utf8');
const outputFile = process.env.ELFCONV_PTHREAD_OUTPUT_DIR + '/lifted.stdout';

test('glibc pthread fixture runs in isolated JSPI Chromium', async ({ page }) => {
  const pageErrors = [];
  page.on('pageerror', error => pageErrors.push(error.message));
  await page.goto('/');

  const capabilities = await page.evaluate(() => ({
    isolated: crossOriginIsolated,
    sharedArrayBuffer: typeof SharedArrayBuffer === 'function',
    jspi: typeof WebAssembly.Suspending === 'function' &&
      typeof WebAssembly.promising === 'function',
  }));
  expect(capabilities).toEqual({ isolated: true, sharedArrayBuffer: true, jspi: true });

  await page.locator('#start').click();
  await page.waitForFunction(() =>
    document.body.dataset.exitCode !== undefined || document.body.dataset.abort !== undefined,
    null, { timeout: 120000 });

  const result = await page.evaluate(() => ({
    exitCode: document.body.dataset.exitCode,
    abort: document.body.dataset.abort,
    stdout: document.querySelector('#output').textContent,
  }));
  expect(result.abort).toBeUndefined();
  expect(result.exitCode).toBe('0');
  expect(pageErrors).toEqual([]);
  fs.writeFileSync(outputFile, result.stdout);
  expect(result.stdout).toBe(nativeStdout);
});
