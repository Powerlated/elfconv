#!/usr/bin/env node
// Node 24+: node scripts/serve_game_files.ts ROOT [PORT]
import { createServer } from 'node:http';
import { open, readdir, realpath, stat } from 'node:fs/promises';
import { resolve, relative, isAbsolute, sep } from 'node:path';
import { pipeline } from 'node:stream/promises';

type AssetMetadata = {
  version: 1;
  files: { path: string; size: number }[];
  directories: string[];
};

async function getAssetMetadata(): Promise<AssetMetadata> {
  const files: AssetMetadata['files'] = [];
  const directories: string[] = [];

  async function walk(directory: string, assetPath: string, ancestors: Set<string>): Promise<void> {
    const canonicalDirectory = await realpath(directory);
    if (!contained(canonicalDirectory) || ancestors.has(canonicalDirectory)) return;
    const nextAncestors = new Set(ancestors);
    nextAncestors.add(canonicalDirectory);
    for (const entry of await readdir(directory, { withFileTypes: true })) {
      const entryPath = resolve(directory, entry.name);
      const entryAssetPath = `${assetPath}/${entry.name}`;
      let canonical: string;
      try {
        canonical = await realpath(entryPath);
        if (!contained(canonical)) continue;
        const info = await stat(canonical);
        if (info.isDirectory()) {
          directories.push(entryAssetPath);
          await walk(canonical, entryAssetPath, nextAncestors);
        } else if (info.isFile()) {
          files.push({ path: entryAssetPath, size: info.size });
        }
      } catch (error) {
        const code = (error as NodeJS.ErrnoException).code;
        if (['ENOENT', 'ENOTDIR', 'EACCES', 'ELOOP'].includes(code ?? '')) continue;
        throw error;
      }
    }
  }

  await walk(root, '', new Set());
  files.sort((a, b) => a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
  directories.sort();
  return { version: 1, files, directories };
}

const rootArg = process.argv[2];
const port = Number(process.argv[3] ?? 8081);
if (!rootArg || !Number.isInteger(port) || port < 1 || port > 65535) {
  console.error('Usage: node scripts/serve_game_files.ts ROOT [PORT]');
  process.exit(1);
}
const root = await realpath(resolve(rootArg));
function contained(path: string): boolean {
  const suffix = relative(root, path);
  return suffix !== '..' && !suffix.startsWith(`..${sep}`) && !isAbsolute(suffix);
}

const server = createServer(async (req, res) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, HEAD, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Range');
  res.setHeader('Access-Control-Expose-Headers', 'Accept-Ranges, Content-Range, Content-Length');
  res.setHeader('Cross-Origin-Resource-Policy', 'cross-origin');
  res.setHeader('X-Content-Type-Options', 'nosniff');
  if (req.method === 'OPTIONS') { res.writeHead(204); res.end(); return; }
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    res.setHeader('Allow', 'GET, HEAD, OPTIONS');
    res.writeHead(405); res.end(); return;
  }
  let file;
  try {
    // Decode the raw path rather than URL-normalizing traversal segments away.
    let path: string;
    try { path = decodeURIComponent((req.url ?? '/').split('?')[0]); }
    catch { res.writeHead(400); res.end(); return; }
    if (path.includes('\0') || path.includes('\\')) {
      res.writeHead(400); res.end(); return;
    }
    if (path === '/__elfconv_assets.json') {
      const body = Buffer.from(JSON.stringify(await getAssetMetadata()));
      res.setHeader('Content-Type', 'application/json; charset=utf-8');
      res.setHeader('Cache-Control', 'no-cache');
      res.setHeader('Content-Length', body.length);
      res.writeHead(200);
      res.end(req.method === 'HEAD' ? undefined : body);
      return;
    }
    const candidate = resolve(root, `.${path}`);
    if (!contained(candidate)) { res.writeHead(403); res.end(); return; }
    const canonical = await realpath(candidate);
    if (!contained(canonical)) { res.writeHead(403); res.end(); return; }
    file = await open(canonical, 'r');
    const stat = await file.stat();
    if (!stat.isFile()) { res.writeHead(404); res.end(); return; }
    res.setHeader('Accept-Ranges', 'bytes');
    res.setHeader('Content-Type', 'application/octet-stream');
    res.setHeader('Cache-Control', 'no-cache');
    let start = 0, end = stat.size - 1, status = 200;
    if (req.headers.range && req.method === 'GET') {
      const match = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range);
      let valid = !!match && !!(match[1] || match[2]) && stat.size > 0;
      if (valid && match) {
        if (match[1]) {
          start = Number(match[1]);
          end = match[2] ? Number(match[2]) : end;
          valid = Number.isSafeInteger(start) && Number.isSafeInteger(end)
            && start < stat.size && end >= start;
          end = Math.min(end, stat.size - 1);
        } else {
          const length = Number(match[2]);
          valid = Number.isSafeInteger(length) && length > 0;
          start = Math.max(0, stat.size - length);
        }
      }
      if (!valid) {
        res.setHeader('Content-Range', `bytes */${stat.size}`);
        res.writeHead(416); res.end(); return;
      }
      status = 206;
      res.setHeader('Content-Range', `bytes ${start}-${end}/${stat.size}`);
    }
    res.setHeader('Content-Length', stat.size === 0 ? 0 : end - start + 1);
    res.writeHead(status);
    if (req.method === 'HEAD' || stat.size === 0) { res.end(); return; }
    await pipeline(file.createReadStream({ start, end, autoClose: false }), res);
  } catch (error) {
    const code = (error as NodeJS.ErrnoException).code;
    if (!res.headersSent) {
      res.writeHead(code === 'ENOENT' || code === 'ENOTDIR' ? 404 : code === 'EACCES' ? 403 : 500);
      res.end();
    } else {
      res.destroy();
    }
    if (!['ENOENT', 'ENOTDIR', 'EACCES', 'ERR_STREAM_PREMATURE_CLOSE', 'ECONNRESET'].includes(code ?? '')) {
      console.error(error);
    }
  } finally {
    await file?.close();
  }
});
server.on('error', error => { console.error(error.message); process.exitCode = 1; });
server.listen(port, '127.0.0.1', () => {
  console.log(`Serving ${root} at http://127.0.0.1:${port}/`);
});
