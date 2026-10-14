import {computed, ref} from 'vue';
import {defineStore} from 'pinia';
import {httpBase, websocketUrl} from '@/lib/device-url';
import {exportFilename} from '@/lib/mapping';

const emptyConfig = () => ({
  remotes: [],
  keys: [],
  macros: [],
  mappings: [],
  hosts: [],
  settings: {},
});

const COMMAND_TIMEOUT_MS = 6000;
const RECONNECT_DELAY_MS = 2000;

/**
 * 设备连接层：唯一持有 WebSocket 的 store。
 *
 * 固件侧是"命令 + 事件"混合模型：
 *   - 命令：{id, op, args} → {id, ok, ...} 或 {id, ok: false, error}，按 id 配对
 *   - 事件：{event: 'state' | 'input' | 'capture.error'}，无 id 主动推送
 * 不轮询设备，所有状态变化都由 state 事件驱动。
 */
export const useDeviceStore = defineStore('device', () => {
  const connection = ref('connecting'); // connecting | open | closed
  const notice = ref({text: '', level: 'info'});
  const status = ref({});
  const config = ref(emptyConfig());

  /** 最近一次学习到的红外信号，RemotesView 监听它来填充按键表单。 */
  const lastInput = ref(null);
  /** 缓冲溢出计数，自增即表示发生了一次溢出（用于触发提示）。 */
  const captureErrors = ref(0);

  // 连接相关的内部状态刻意不做响应式：它们只在浏览器事件回调里使用。
  let socket = null;
  let retryTimer = 0;
  let sequence = 0;
  const pending = new Map();

  const ready = computed(() => connection.value === 'open');

  const connectionLabel = computed(() => ({
    connecting: '正在连接',
    open: 'WebSocket 已连接',
    closed: '连接中断，正在重连',
  }[connection.value] || '未知状态'));

  function setNotice(text, level = 'info') {
    notice.value = {text: text || '', level};
  }

  /**
   * 下发一条设备命令，返回 Promise。
   * 超时与断线都会 reject——这两种情况下设备是否已执行无从得知，
   * 所以提示文案必须让用户去核对结果，而不是暗示失败。
   */
  function command(op, args = {}) {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      return Promise.reject(new Error('设备未连接，请等待 WebSocket 重连'));
    }
    const id = ++sequence;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        pending.delete(id);
        reject(new Error('设备响应超时，操作结果未知，请重连后检查'));
      }, COMMAND_TIMEOUT_MS);
      pending.set(id, {resolve, reject, timer});
      socket.send(JSON.stringify({id, op, args}));
    });
  }

  function failPending(message) {
    for (const item of pending.values()) {
      clearTimeout(item.timer);
      item.reject(new Error(message));
    }
    pending.clear();
  }

  function handleMessage(event) {
    let data;
    try {
      data = JSON.parse(event.data);
    } catch (error) {
      setNotice(`响应解析失败：${error.message}`, 'error');
      return;
    }

    if (data.event === 'state') {
      config.value = data.config || emptyConfig();
      status.value = data.status || {};
      return;
    }
    if (data.event === 'input') {
      lastInput.value = data;
      return;
    }
    if (data.event === 'capture.error') {
      lastInput.value = null;
      captureErrors.value += 1;
      setNotice('红外采样缓冲区溢出，未保存完整数据，请重新学习', 'error');
      return;
    }

    // 命令响应
    const item = pending.get(data.id);
    if (!item) return;
    clearTimeout(item.timer);
    pending.delete(data.id);
    if (data.ok) item.resolve(data);
    else item.reject(new Error(data.error));
  }

  function connect() {
    if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) {
      return;
    }
    clearTimeout(retryTimer);
    const ws = new WebSocket(websocketUrl());
    socket = ws;

    ws.onopen = () => {
      if (socket !== ws) return;
      connection.value = 'open';
      setNotice('');
    };
    ws.onmessage = event => {
      if (socket === ws) handleMessage(event);
    };
    ws.onclose = () => {
      if (socket !== ws) return;
      socket = null;
      connection.value = 'closed';
      failPending('连接中断，操作结果未知，请检查保存结果');
      retryTimer = setTimeout(connect, RECONNECT_DELAY_MS);
    };
    ws.onerror = () => ws.close();
  }

  /** 请求设备立刻推送一次完整状态（正常无需调用，连上即自动推送）。 */
  const refresh = () => command('state.get');

  /** 通过 HTTP 导出业务数据并触发浏览器下载。 */
  async function exportData(scope) {
    const response = await fetch(`${httpBase()}/api/export?scope=${encodeURIComponent(scope)}`, {
      signal: AbortSignal.timeout(30000),
    });
    if (!response.ok) throw new Error(`导出失败：${await response.text()}`);
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = exportFilename(scope);
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  return {
    connection,
    connectionLabel,
    notice,
    status,
    config,
    lastInput,
    captureErrors,
    ready,
    setNotice,
    command,
    connect,
    refresh,
    exportData,
  };
});
