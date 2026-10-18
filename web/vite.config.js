import {fileURLToPath, URL} from 'node:url';
import {defineConfig, loadEnv} from 'vite';
import vue from '@vitejs/plugin-vue';

// 构建产物直接落到 <项目>/data/frontend，与设备侧 /frontend 保持同构：
//   web/dist 的概念被 ../data/frontend 取代，data/ 即是"设备文件系统镜像"。
// 这样 deploy.mjs（默认上传 data/**）和 Arduino IDE 的 "Upload LittleFS data"
// 两种部署方式结果完全一致。
const outDir = fileURLToPath(new URL('../data/frontend', import.meta.url));
const root = fileURLToPath(new URL('.', import.meta.url));

// 设备地址的唯一来源：web/.env.development 的 VITE_DEVICE。
// deploy.mjs 读的是同一个文件，所以"设备在哪"只需要在一个地方改。
function deviceTarget(env) {
  const raw = (env.VITE_DEVICE || '').trim();
  if (!raw) return 'http://192.168.4.1';
  return new URL(raw.includes('://') ? raw : `http://${raw}`).origin;
}

// 设备不可达时把原因说清楚——否则只丢一个 ECONNREFUSED，很难联想到"忘连热点了"。
// 前端断线后每 2 秒重连一次，所以同一处错误 15 秒内只提示一次，避免刷屏。
function deviceErrorReporter(target) {
  let seen = '';
  let seenAt = 0;
  return proxy => {
    proxy.on('error', (error, request) => {
      const where = request?.url || '(upgrade)';
      const reason = error.code || error.message;
      const key = `${where}|${reason}`;
      const now = Date.now();
      if (key === seen && now - seenAt < 15000) return;
      seen = key;
      seenAt = now;
      console.error(`\n[dev-proxy] ${where} → ${target} 不可达：${reason}`);
      console.error(`[dev-proxy] 确认电脑已连上设备热点，且 ${target}/api/status 可直接访问\n`);
    });
  };
}

export default defineConfig(({mode}) => {
  const target = deviceTarget(loadEnv(mode, root));
  const device = new URL(target);
  // 固件把事件通道挂在 /ws 路径上，与 HTTP 共用 80 端口。

  return {
    // 必须是绝对根路径：固件把 /frontend 目录 1:1 映射到 URL 根路径。
    // 用 './' 会让 /system 这类深链接把 assets 解析成 /system/assets/... 而 404。
    base: '/',

    plugins: [vue()],

    resolve: {
      alias: {'@': fileURLToPath(new URL('./src', import.meta.url))},
    },

    build: {
      outDir,
      emptyOutDir: true,
      // ESP32-C3 上跑的是现代浏览器，且目标设备内存有限，不向下兼容 ES5。
      target: 'es2020',
      assetsDir: 'assets',
      // 不拆分 CSS，减少设备侧 HTTP 请求数：WebServer::streamFile 是阻塞实现，
      // 每次请求都会占住 loop()，请求数比单文件体积更值得控制。
      cssCodeSplit: false,
      // 同理，保持单 JS chunk（不做 manualChunks 拆分）。
      reportCompressedSize: true,
      chunkSizeWarningLimit: 800,
    },

    server: {
      port: 5173,
      // 项目位于 /Volumes/Macintosh HD/... 下，fsevents 在该挂载路径下
      // 不能可靠触发文件变更事件，导致 HMR 失效。启用轮询作为兜底。
      watch: {
        usePolling: true,
        interval: 100,
      },
      // 开发时前端跑在本机，接口与事件通道都由 dev server 转给真机：
      //   localhost:5173/api/*  →  192.168.4.1/api/*      （HTTP）
      //   localhost:5173/ws     →  192.168.4.1/ws         （WebSocket）
      // 浏览器只跟 dev server 说话，全程同源，因此不存在跨域——设备侧也就不需要
      // 任何 CORS 头（固件确实没发）。改设备地址只需改 .env.development。
      proxy: {
        '/api': {
          target,
          changeOrigin: true,
          configure: deviceErrorReporter(target),
        },
        '/ws': {
          target,
          ws: true,
          changeOrigin: true,
          configure: deviceErrorReporter(target),
        },
      },
      // 想在手机上调试可加 --host；此时手机访问本机 IP，代理仍然在本机完成。
    },
  };
});
