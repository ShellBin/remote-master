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
 *   - 事件：{event: 'state' | 'input' | 'capture.error' | 'ir.error'}，无 id 主动推送
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

  /**
   * 带超时的命令下发：用于导入等可能耗时较长的操作。
   * 注意：超时后不自动重试，因为写入操作重试可能导致重复或不一致。
   */
  async function commandWithTimeout(op, args = {}, timeoutMs = 15000) {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      return Promise.reject(new Error('设备未连接，请等待 WebSocket 重连'));
    }
    const id = ++sequence;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        pending.delete(id);
        reject(new Error('设备响应超时，请检查设备状态后重试'));
      }, timeoutMs);
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

    if (data.event === 'config.changed') {
      scheduleFetch();
      return;
    }
    if (data.event === 'state') {
      // 新固件只推 status；保留 data.config 分支兼容旧固件。
      if (data.config) config.value = data.config;
      if (data.status) status.value = {...status.value, ...data.status};
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
    if (data.event === 'ir.error') {
      setNotice(`红外发射失败：${data.error}`, 'error');
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

  /** 页面加载时通过 HTTP 获取全量状态（config + status） */
  async function fetchJson(path) {
    const response = await fetch(`${httpBase()}${path}`, {signal: AbortSignal.timeout(10000)});
    if (!response.ok) throw new Error(`${path} → HTTP ${response.status}`);
    return response.json();
  }

  let fetching = null;
  let refetchQueued = false;
  let configRefreshTimer = 0;

  /**
   * 分段拉取配置。按键/映射集合可能很大，整份 config 一次性下发会给设备
   * 带来明显的堆压力；这里按集合顺序请求，任一时刻只保留一个响应。
   */
  async function fetchState() {
    if (fetching) {
      refetchQueued = true;
      return fetching;
    }
    fetching = (async () => {
      try {
        const meta = await fetchJson('/api/config/meta');
        const remotes = await fetchJson('/api/config/remotes');
        const keys = await fetchJson('/api/config/keys');
        const macros = await fetchJson('/api/config/macros');
        const mappings = await fetchJson('/api/config/mappings');
        const statusData = await fetchJson('/api/status');
        config.value = {
          settings: meta.settings || {},
          hosts: meta.hosts || [],
          nextId: meta.nextId,
          lastHostId: meta.lastHostId || '',
          remotes: remotes.remotes || [],
          keys: keys.keys || [],
          macros: macros.macros || [],
          mappings: mappings.mappings || [],
        };
        status.value = {...status.value, ...statusData};
      } catch (error) {
        setNotice(`状态获取失败：${error.message}`, 'error');
      }
    })();
    try {
      await fetching;
    } finally {
      fetching = null;
      if (refetchQueued) {
        refetchQueued = false;
        void fetchState();
      }
    }
  }

  /** 合并短时间内的多次 config.changed，避免连续保存触发风暴式重拉。 */
  function scheduleFetch() {
    clearTimeout(configRefreshTimer);
    configRefreshTimer = setTimeout(() => { void fetchState(); }, 150);
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
      fetchState();  // 通过 HTTP 获取全量状态，WebSocket 差分推送可能遗漏字段
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

  /** 重新拉取一次配置与状态（HTTP 分段接口）。 */
  const refresh = () => fetchState();

  /** 通过 HTTP 导出业务数据并触发浏览器下载。scope=remote/macro 时需提供 id 与 name 用于命名。 */
  async function exportData(scope, id = '', name = '') {
    let url = `${httpBase()}/api/export?scope=${encodeURIComponent(scope)}`;
    if ((scope === 'remote' || scope === 'macro') && id) url += `&id=${encodeURIComponent(id)}`;
    const response = await fetch(url, {signal: AbortSignal.timeout(30000)});
    if (!response.ok) throw new Error(`导出失败：${await response.text()}`);
    const blob = await response.blob();
    const url2 = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url2;
    link.download = exportFilename(scope, name);
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url2), 1000);
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
    commandWithTimeout,
    connect,
    refresh,
    fetchState,
    exportData,
  };
});
