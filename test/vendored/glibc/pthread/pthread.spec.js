const { test, expect } = require(process.env.ELFCONV_PLAYWRIGHT_PACKAGE);
const fs = require('fs');

const nativeStdout = fs.readFileSync(process.env.ELFCONV_PTHREAD_NATIVE_STDOUT, 'utf8');
const nativeStderr = fs.readFileSync(process.env.ELFCONV_PTHREAD_NATIVE_STDERR, 'utf8');
const outputDir = process.env.ELFCONV_PTHREAD_OUTPUT_DIR;

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
  await page.evaluate(() => {
    document.body.dataset.stdout = '';
    document.body.dataset.stderr = '';
    const print = Module.print;
    const printErr = Module.printErr;
    Module.print = line => {
      document.body.dataset.stdout += line + String.fromCharCode(10);
      print(line);
    };
    Module.printErr = line => {
      document.body.dataset.stderr += line + String.fromCharCode(10);
      printErr(line);
    };
  });
  expect(capabilities).toEqual({ isolated: true, sharedArrayBuffer: true, jspi: true });

  await page.locator('#start').click();
  await page.waitForFunction(() =>
    document.body.dataset.exitCode !== undefined || document.body.dataset.abort !== undefined,
    null, { timeout: 120000 });

  const result = await page.evaluate(() => ({
    exitCode: document.body.dataset.exitCode,
    abort: document.body.dataset.abort,
    stdout: document.body.dataset.stdout,
    stderr: document.body.dataset.stderr,
  }));
  expect(result.abort).toBeUndefined();
  expect(result.exitCode).toBe('0');
  expect(pageErrors).toEqual([]);
  fs.writeFileSync(outputDir + '/lifted.stdout', result.stdout);
  fs.writeFileSync(outputDir + '/lifted.stderr', result.stderr);
  expect(result.stdout).toBe(nativeStdout);
  expect(result.stderr).toBe(nativeStderr);
});
