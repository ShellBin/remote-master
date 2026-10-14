#!/usr/bin/env node
// 为构建产物生成 .gz 副本。
//
// 固件的静态服务在客户端声明 Accept-Encoding: gzip 时会优先下发同名 .gz，
// 而 WebServer::streamFile 是阻塞式循环——下发时间越短，loop()（IR 采集、
// BLE 状态机）被占住的时间就越短。Vue 产物通常能压到三分之一左右。
//
// .gz 与原文件都会保留在设备上：不支持 gzip 的客户端仍能拿到原文件。
// 压缩收益不明显（小于 5%）时不留副本，避免白占 LittleFS 空间。

import {readdir, readFile, writeFile} from 'node:fs/promises';
import {extname, join, relative, sep} from 'node:path';
import {fileURLToPath} from 'node:url';
import {gzipSync} from 'node:zlib';

const OUT_DIR = fileURLToPath(new URL('../../data/frontend', import.meta.url));
const COMPRESSIBLE = new Set(['.html', '.js', '.css', '.svg', '.json']);
const MIN_BYTES = 1024;
const MIN_GAIN = 0.05;

async function walk(dir) {
  const entries = await readdir(dir, {withFileTypes: true});
  const files = [];
  for (const entry of entries) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) files.push(...await walk(full));
    else if (entry.isFile()) files.push(full);
  }
  return files;
}

let files = [];
try {
  files = await walk(OUT_DIR);
} catch (error) {
  console.error(`找不到构建产物目录 ${OUT_DIR}，请先执行 vite build。`);
  process.exit(1);
}

let created = 0;
let saved = 0;

for (const file of files) {
  if (file.endsWith('.gz') || !COMPRESSIBLE.has(extname(file))) continue;
  const source = await readFile(file);
  if (source.length < MIN_BYTES) continue;

  const compressed = gzipSync(source, {level: 9});
  if (compressed.length >= source.length * (1 - MIN_GAIN)) continue;

  await writeFile(`${file}.gz`, compressed);
  created += 1;
  saved += source.length - compressed.length;
  const name = relative(OUT_DIR, file).split(sep).join('/');
  console.log(`  ${name}  ${source.length} → ${compressed.length} 字节`);
}

if (created) {
  console.log(`已生成 ${created} 个 .gz 副本，可节省约 ${Math.round(saved / 1024)} KB 传输量。`);
} else {
  console.log('没有需要预压缩的文件。');
}
