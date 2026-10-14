#!/usr/bin/env node
// 把本地目录镜像到设备文件系统，默认整目录覆盖前端。
//
//   node deploy.mjs                                   # 上传 <项目>/data/** 到设备 /**
//   node deploy.mjs http://192.168.4.1                # 指定设备地址
//   node deploy.mjs --dir ./dist --remote frontend    # 上传其它目录
//   node deploy.mjs --no-clean                        # 保留设备上已有的文件
//   node deploy.mjs --initialize                      # 设备未初始化时直接格式化（跳过询问）
//   node deploy.mjs --no-wait                         # 配置存储未就绪时不等待重启
//
// --remote 的前导斜杠可省略（脚本会补上）：在 Git Bash 里写 /frontend 会被 shell
// 转换成 C:/... 之类的 Windows 路径，脚本检测到这种情况会直接报错停下。
//
// 设备地址来源（优先级从高到低）：
//   命令行参数 → DEVICE_ADDR 环境变量 → web/.env.development 的 VITE_DEVICE → 192.168.4.1
// 后两者与前端 dev server 共用同一份配置，所以"设备在哪"只需要在一个地方改。
//
// 设备侧规则：/frontend 的目录结构与 URL 根路径 1:1 对应。
// 因此 data/frontend/index.html 会以 /frontend/index.html 落盘，并可通过
// GET /index.html 访问。

import {readdir, readFile, stat} from 'node:fs/promises';
import {dirname, join, resolve, sep} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createInterface} from 'node:readline/promises';

const HERE = dirname(fileURLToPath(import.meta.url));
const FS_ROOT = '/frontend';
const DEFAULT_ADDRESS = 'http://192.168.4.1';
// 配置存储只在固件 setup() 里加载，而固件没有重启接口——所以只能等用户复位。
const STORAGE_WAIT_MS = 60000;
const PROBE_ATTEMPTS = 3;

function usage() {
  console.log(`用法: node deploy.mjs [http://192.168.4.1] [选项]

选项:
  --dir <路径>       要上传的本地目录（默认: ${join(HERE, 'data')}）
  --remote <前缀>    设备侧目标前缀，前导斜杠可省略（默认: /）
  --no-clean         不预先清空目标目录，仅覆盖同名文件
  --initialize       设备未初始化时直接格式化，不询问（会清空设备数据）
  --no-wait          配置存储未就绪时立即继续，不等待设备重启
  --help             显示本帮助

示例:
  node deploy.mjs
  node deploy.mjs http://192.168.4.1 --dir ./dist --remote frontend`);
}

const args = process.argv.slice(2);
if (args.includes('--help') || args.includes('-h')) {
  usage();
  process.exit(0);
}

function flagValue(name) {
  const index = args.indexOf(name);
  return index >= 0 ? args[index + 1] : undefined;
}

// 取第一个非选项参数作为设备地址，同时跳过选项自身的取值。
const positional = [];
for (let i = 0; i < args.length; i++) {
  const arg = args[i];
  if (arg === '--dir' || arg === '--remote') { i++; continue; }
  if (!arg.startsWith('--')) positional.push(arg);
}

const localDir = resolve(flagValue('--dir') ?? join(HERE, 'data'));
const clean = !args.includes('--no-clean');
const assumeYes = args.includes('--initialize');
const waitForStorage = !args.includes('--no-wait');
const remotePrefix = normalizePrefix(flagValue('--remote') ?? '/');

// ---------------------------------------------------------------- 设备地址

// 与前端 dev server 复用同一份配置，避免"dev 连 A、部署传到 B"这种不一致。
async function sharedAddress() {
  const file = join(HERE, 'web', '.env.development');
  let text;
  try {
    text = await readFile(file, 'utf8');
  } catch {
    return null;
  }
  for (const line of text.split(/\r?\n/)) {
    const match = /^\s*VITE_DEVICE\s*=\s*(.+?)\s*$/.exec(line);
    if (match && match[1]) {
      return {value: match[1].replace(/^["']|["']$/g, ''), from: 'web/.env.development'};
    }
  }
  return null;
}

const shared = await sharedAddress();
const explicit = positional[0] || process.env.DEVICE_ADDR || shared?.value;
const addressFrom = positional[0]
  ? '命令行参数'
  : process.env.DEVICE_ADDR
    ? 'DEVICE_ADDR 环境变量'
    : shared
      ? 'web/.env.development'
      : '默认值';
const base = explicit || DEFAULT_ADDRESS;

const url = new URL(base.includes('://') ? base : `http://${base}`);
if (!['http:', 'https:'].includes(url.protocol)) throw new Error(`需要 http:// 或 https:// 地址，收到 ${base}`);

// ---------------------------------------------------------------- 基础工具

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const kb = bytes => {
  const value = Number(bytes) || 0;
  return value < 1024 ? `${value} B` : `${Math.round(value / 1024)} KB`;
};

// 进度行只在终端里有意义；重定向到文件时留着反而变成一堆 \r。
function progress(text) {
  if (process.stdout.isTTY) process.stdout.write(`\r${text.padEnd(56)}`);
}
function progressEnd() {
  if (process.stdout.isTTY) process.stdout.write(`\r${' '.repeat(56)}\r`);
}

function describeError(error) {
  if (error?.name === 'TimeoutError' || error?.name === 'AbortError') return '请求超时';
  return error?.cause?.code || error?.code || error?.cause?.message || error?.message || '未知错误';
}

function normalizePrefix(prefix) {
  let value = prefix.trim();
  if (!value.startsWith('/')) value = `/${value}`;
  while (value.length > 1 && value.endsWith('/')) value = value.slice(0, -1);
  return value;
}

// 目标路径 -> 设备侧绝对路径。反斜杠统一成正斜杠，避免 Windows 路径混入 URL。
function remotePathFor(relativePath) {
  const normalized = relativePath.split(sep).join('/');
  return remotePrefix === '/' ? `/${normalized}` : `${remotePrefix}/${normalized}`;
}

// 设备侧路径 -> 浏览器可访问的 URL 路径（/frontend 被映射到根）。
function publicPathFor(remotePath) {
  if (remotePath === FS_ROOT) return '/';
  if (remotePath.startsWith(`${FS_ROOT}/`)) return remotePath.slice(FS_ROOT.length);
  return remotePath;
}

async function request(path, options = {}) {
  return fetch(new URL(path, url), {...options, signal: AbortSignal.timeout(30000)});
}

async function requestOk(path, options = {}) {
  const response = await request(path, options);
  if (!response.ok) throw new Error(`${response.status}: ${(await response.text()).trim()}`);
  return response;
}

async function confirm(question) {
  const rl = createInterface({input: process.stdin, output: process.stdout});
  try {
    const answer = await rl.question(`${question} [y/N] `);
    return /^y(es)?$/i.test(answer.trim());
  } catch {
    return false;
  } finally {
    rl.close();
  }
}

// .DS_Store / __MACOSX 之类无用文件不应占用 LittleFS 空间
function skip(relativePath) {
  const parts = relativePath.split(sep);
  return parts.some(part => part.startsWith('.') || part === '__MACOSX');
}

async function walk(dir, prefix = '') {
  const entries = await readdir(dir, {withFileTypes: true});
  const found = [];
  for (const entry of entries) {
    const child = prefix ? join(prefix, entry.name) : entry.name;
    const full = join(dir, entry.name);
    if (skip(child)) continue;
    if (entry.isDirectory()) found.push(...(await walk(full, child)));
    else if (entry.isFile()) found.push({relative: child, full, size: (await stat(full)).size});
  }
  return found;
}

// ------------------------------------------------------- 设备初始化状态检测

async function fetchStatus(timeoutMs = 5000) {
  const response = await fetch(new URL('/api/status', url), {signal: AbortSignal.timeout(timeoutMs)});
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}

// 设备刚上电时 HTTP 要 2~3 秒才可用，重试几次比让用户重跑脚本友好。
async function probeStatus(attempts = PROBE_ATTEMPTS) {
  let lastError;
  for (let attempt = 1; attempt <= attempts; attempt++) {
    try {
      return await fetchStatus();
    } catch (error) {
      lastError = error;
      if (attempt < attempts) await sleep(1200);
    }
  }
  throw new Error(
    `无法连接设备 ${url.origin}（${describeError(lastError)}）\n` +
    '  排查顺序：\n' +
    '    1. 电脑是否已连上设备热点（名称可在前端「系统」页修改，出厂默认 IR-C3-Remote）；\n' +
    `    2. 地址是否正确 —— 设备作为热点时固定是 ${DEFAULT_ADDRESS}（当前用 ${addressFrom} 的 ${base}）；\n` +
    '    3. 设备是否已上电（刚复位时约需 2~3 秒）。\n' +
    `  可先单独验证连通性：curl ${url.origin}/api/status`,
  );
}

function describeStatus(status) {
  const fs = status.filesystemMounted
    ? `文件系统 已挂载 ${kb(status.fsUsed)}/${kb(status.fsTotal)}`
    : '文件系统 未初始化';
  const store = status.storageReady ? '配置存储 就绪' : '配置存储 未就绪';
  return `${fs} · ${store}`;
}

// 设备未初始化 = 从没挂载过 LittleFS（新烧固件的首次使用，或分区被擦过）。
// 固件只在"未挂载"时接受格式化，已挂载时会返回 409 拒绝，所以这里不会误擦数据。
async function initializeFilesystem() {
  console.log('检测到设备尚未初始化（LittleFS 未挂载）。');
  if (!assumeYes) {
    if (!process.stdin.isTTY) {
      throw new Error('设备未初始化。非交互环境请显式加 --initialize（会格式化设备文件系统）。');
    }
    const ok = await confirm('现在格式化设备文件系统？这会清空设备上已有的配置与学习记录。');
    if (!ok) throw new Error('已取消。设备未初始化，前端无法上传。');
  } else {
    console.log('--initialize：跳过询问，直接格式化。');
  }
  const result = await (await requestOk('/api/fs/initialize?confirm=INITIALIZE_EMPTY_FILESYSTEM', {method: 'POST'})).json();
  if (!result.ok) throw new Error(`初始化失败：${result.error || '未知原因'}`);
  console.log('文件系统已格式化并挂载。');
}

async function awaitStorageReady(status) {
  if (!waitForStorage) {
    console.log('配置存储未就绪，已按 --no-wait 跳过等待。');
    return status;
  }
  console.log('配置存储未就绪：固件在启动时才加载它，请按一下设备的复位键（或断电重上）。');
  const deadline = Date.now() + STORAGE_WAIT_MS;
  let waited = 0;
  while (Date.now() < deadline) {
    await sleep(2000);
    waited += 2;
    try {
      const fresh = await fetchStatus(3000);
      if (fresh.storageReady) {
        progressEnd();
        console.log(`配置存储已就绪（等待 ${waited} 秒）。`);
        return fresh;
      }
    } catch {
      // 重启过程中必然连不上，属于预期
    }
    progress(`等待设备重启… 已等待 ${waited}s / ${STORAGE_WAIT_MS / 1000}s`);
  }
  progressEnd();
  console.log(`等待超时（${STORAGE_WAIT_MS / 1000} 秒），配置存储仍未就绪。`);
  console.log('  · 设备若已重启仍是这个状态，多半是 remote-config.json 损坏 —— 重启不会自愈；');
  console.log('  · 前端资源与配置存储互相独立，先继续上传；配置类操作要等它就绪。');
  return status;
}

// 检测 + 自动修复，返回设备就绪后的状态。
async function ensureInitialized() {
  let status = await probeStatus();
  console.log(`设备状态：${describeStatus(status)}`);
  if (!status.filesystemMounted) {
    await initializeFilesystem();
    status = await probeStatus(); // 格式化后 filesystemMounted 立即为 true
  }
  if (!status.storageReady) status = await awaitStorageReady(status);
  return status;
}

// ------------------------------------------------------------------ 主流程

try {
  // Git Bash / MSYS 会把参数里的 /frontend 当路径转换成 C:/...，若放任下去
  // 会静默上传到错误目录。宁可在这里停下来说清楚。
  if (/^\/[A-Za-z]:\//.test(remotePrefix)) {
    throw new Error(
      `--remote 的值被 shell 转成了 Windows 路径：${remotePrefix}\n` +
      '  改用不带前导斜杠的写法（--remote frontend），或设 MSYS_NO_PATHCONV=1 后再执行。',
    );
  }

  const files = await walk(localDir);
  if (!files.length) throw new Error(`目录为空或全部被忽略: ${localDir}`);

  const totalBytes = files.reduce((sum, file) => sum + file.size, 0);
  console.log(`本地目录 ${localDir}`);
  console.log(`设备地址 ${url.origin}（来自${addressFrom}）`);
  console.log(`待上传 ${files.length} 个文件，共 ${kb(totalBytes)} → ${remotePrefix}`);

  const status = await ensureInitialized();

  // 只清理这次要写入的顶层目录，且必须落在 /frontend 之内。
  if (clean) {
    const roots = new Set();
    for (const file of files) {
      const top = remotePathFor(file.relative).split('/').filter(Boolean)[0];
      if (top) roots.add(`/${top}`);
    }
    for (const root of roots) {
      if (root !== FS_ROOT && !root.startsWith(`${FS_ROOT}/`)) continue;
      await requestOk(`/api/files?path=${encodeURIComponent(root)}`, {method: 'DELETE'});
      console.log(`已清空设备目录 ${root}`);
    }
  }

  let failed = 0;
  for (const file of files) {
    const remote = remotePathFor(file.relative);
    const bytes = await readFile(file.full);
    const form = new FormData();
    form.append('file', new Blob([bytes]), file.relative.split(sep).join('/'));
    try {
      await requestOk(`/api/files?path=${encodeURIComponent(remote)}`, {method: 'POST', body: form});
      const echoed = Buffer.from(await (await requestOk(`/api/files?path=${encodeURIComponent(remote)}`)).arrayBuffer());
      if (!echoed.equals(bytes)) throw new Error('回读校验不一致');
      console.log(`✓ ${remote} (${bytes.length} 字节)`);
    } catch (error) {
      failed++;
      console.error(`✗ ${remote} — ${error.message}`);
    }
  }

  if (failed) {
    console.error(`部署失败：${failed}/${files.length} 个文件未通过校验。`);
    process.exitCode = 1;
  } else {
    // 入口文件再走一次 HTTP，确认整条链路（文件系统 → 静态分发 → URL 根映射）通了。
    // 站点入口固定是 /frontend/index.html —— 固件把 /frontend 映射到 URL 根，与
    // --remote 写成 / 还是 /frontend 无关。
    // 这只是额外确认，失败不该推翻"文件都已回读校验通过"这个事实，所以不抛异常。
    const entryRemote = `${FS_ROOT}/index.html`;
    if (files.some(file => remotePathFor(file.relative) === entryRemote)) {
      const publicPath = publicPathFor(entryRemote);
      try {
        const response = await request(publicPath);
        const body = response.ok ? await response.arrayBuffer() : null;
        console.log(`入口自检：GET ${publicPath} → ${response.status}${body ? `，${body.byteLength} 字节` : ''}`);
        if (!response.ok) console.warn('入口不可访问——文件已上传，但静态分发未命中，请检查设备文件系统状态。');
      } catch (error) {
        console.warn(`入口自检失败（${describeError(error)}），文件已上传，可稍后手动访问 ${url.origin}${publicPath} 确认。`);
      }
    }
    console.log(`部署完成 → ${url.origin}/`);
    if (!status.storageReady) {
      console.warn('注意：配置存储未就绪，设备重启前页面上的保存类操作都会失败。');
    }
  }
} catch (error) {
  console.error(`部署中止：${error.message}`);
  process.exitCode = 1;
}
