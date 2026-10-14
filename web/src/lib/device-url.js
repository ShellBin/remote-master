// 设备地址解析。
//
// 设备有两个服务：HTTP（80）与事件通道 WebSocket（81，固件写死）。
// 页面有三种运行形态，对应两种寻址方式：
//
//   形态             HTTP                     事件通道
//   ───────────────  ───────────────────────  ──────────────────────────
//   真机（生产）      同源                     同源 host + :81
//   本机 dev         同源（dev server 代理）   同源 host + /ws（代理 → :81）
//   dev + 直连模式    VITE_DEVICE              VITE_DEVICE host + :81
//
// dev 下默认走代理（见 vite.config.js 的 server.proxy）：浏览器只与 dev server
// 通信，因此不产生跨域，设备侧也不需要任何 CORS 头。
//
// 代理模式下客户端不再需要知道设备 IP；它只存在于 .env.development 里，
// 由 dev server 与 deploy.mjs 读取。

const configured = (import.meta.env.VITE_DEVICE || '').trim();
const proxied = import.meta.env.DEV && import.meta.env.VITE_DEVICE_DIRECT !== 'true';

function baseUrl() {
  if (configured) {
    return new URL(configured.includes('://') ? configured : `http://${configured}`);
  }
  return new URL(location.origin);
}

/**
 * HTTP 接口（/api/status、/api/export 等）的基地址。
 * 代理模式下返回空串，请求落在当前源（相对地址）。
 */
export function httpBase() {
  if (proxied) return '';
  return baseUrl().origin;
}

/** WebSocket 地址。 */
export function websocketUrl() {
  if (proxied) {
    const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
    return `${scheme}://${location.host}/ws`;
  }
  const url = baseUrl();
  const scheme = url.protocol === 'https:' ? 'wss' : 'ws';
  return `${scheme}://${url.hostname}:81/`;
}
