<script setup>
import {computed, reactive, ref, watch} from 'vue';
import {useAction} from '@/composables/useAction';
import {useCatalogStore} from '@/stores/catalog';
import {useDeviceStore} from '@/stores/device';
import HostCard from '@/components/HostCard.vue';
import StorageBar from '@/components/StorageBar.vue';

const catalog = useCatalogStore();
const device = useDeviceStore();
const {busy, run} = useAction();

const formEl = ref(null);
const form = reactive({apSsid: '', apPassword: '', bleName: '', minPulses: 8, irTxMode: 'off'});

function syncFromSettings(settings) {
  form.apSsid = settings.apSsid || '';
  form.apPassword = settings.apPassword || '';
  form.bleName = settings.bleName || '';
  form.minPulses = settings.minPulses ?? 8;
  form.irTxMode = settings.irTxMode || 'off';
}

// 状态推送会频繁到来，但用户正在输入时不回填，否则光标会被抢走。
watch(() => catalog.settings, settings => {
  if (formEl.value?.contains(document.activeElement)) return;
  syncFromSettings(settings);
}, {immediate: true, deep: true});

// WiFi 已连接设备端点 - 从 WebSocket 推送的状态中获取
const wifiStations = computed(() => device.status.wifiStations || []);
const stationCount = computed(() => wifiStations.value.length);

// 蓝牙断开原因映射（HCI 错误码 → 中文）
const disconnectReasons = {
  0x00: '正常断开',
  0x01: '未知 HCI 命令',
  0x02: '未知连接标识',
  0x03: '硬件故障',
  0x04: '页面超时',
  0x05: '认证失败',
  0x06: 'PIN 或密钥缺失',
  0x07: '连接超时',
  0x08: '连接数量已达上限',
  0x09: ' SCO 连接数量已达上限',
  0x0A: 'ACL 连接已存在',
  0x0B: '命令不允许',
  0x0C: '连接被拒绝（资源不足）',
  0x0D: '连接被拒绝（安全原因）',
  0x0E: '连接被拒绝（不可接受）',
  0x0F: '不支持的参数',
  0x10: '远端用户终止连接',
  0x11: '远端设备终止连接（资源不足）',
  0x12: '远端设备终止连接（电源关闭）',
  0x13: '本地主机终止连接',
  0x14: '重复认证',
  0x15: '不允许配对',
  0x16: '未知 LMP PDU',
  0x17: '不支持的远端特性',
  0x18: 'SCO 偏移被拒绝',
  0x19: 'SCO 间隔被拒绝',
  0x1A: 'SCO 模式被拒绝',
  0x1B: 'LMP 参数无效',
  0x1C: '未指定错误',
  0x1D: '不支持的 LMP 参数',
  0x1E: '角色变更不允许',
  0x1F: 'LMP 响应超时',
  0x20: 'LMP 碰撞',
  0x21: 'LMP PDU 不允许',
  0x22: '加密模式不可接受',
  0x23: '链路密钥不可更改',
  0x24: '请求的 QoS 不支持',
  0x25: '即时已过',
  0x26: '配对与密钥交换不支持',
  0x27: 'LMP 事务碰撞',
  0x28: '保留槽不允许',
  0x29: '主机终止（用户请求）',
  0x2A: '主机终止（资源不足）',
  0x2B: '主机终止（超时）',
  0x2C: '主机终止（连接超时）',
  0x2D: '主机终止（连接失败）',
  0x2E: '简单配对不支持',
  0x2F: '主机处于简单配对模式',
  0x30: '主机终止（认证失败）',
  0x31: '主机终止（加密失败）',
};

const bleStatus = computed(() => {
  const ble = device.status.ble || {};
  const subscriptions = ble.reportSubscriptions || 0;
  const reports = [
    subscriptions & 1 ? '键盘' : '',
    subscriptions & 2 ? '鼠标' : '',
    subscriptions & 4 ? '媒体' : '',
  ].filter(Boolean);

  const disconnectCode = ble.lastDisconnectReason || 0;
  const disconnectHex = `0x${disconnectCode.toString(16).toUpperCase().padStart(2, '0')}`;

  return [
    {label: '广播状态', value: ble.advertising ? '进行中' : '已停止', ok: ble.advertising},
    {label: '链路连接', value: ble.linkConnected ? '已连接' : '未连接', ok: ble.linkConnected},
    {label: '加密状态', value: ble.connected ? '已完成' : '未完成', ok: ble.connected},
    {label: '报告订阅', value: reports.length ? reports.join('、') : '无', ok: reports.length > 0},
    {label: '配对主机数', value: ble.bondCount ?? 0, ok: (ble.bondCount ?? 0) > 0},
    {label: '断开原因', value: disconnectReasons[disconnectCode] || `${disconnectHex}（未知）`, ok: disconnectCode === 0},
  ];
});

async function submitWifiSettings() {
  await run(async () => {
    const payload = {
      apSsid: form.apSsid.trim(),
      apPassword: form.apPassword,
    };
    const current = catalog.settings;
    const apChanged = payload.apSsid !== current.apSsid || payload.apPassword !== current.apPassword;
    if (apChanged && !confirm(
      '热点名称或密码将变更，设备会重启热点并断开当前连接，需要重新连接 WiFi。是否继续？',
    )) {
      throw new Error('已取消，设置未保存');
    }
    await catalog.saveSettings(payload);
  }, '热点设置已保存');
}

async function submitBleSettings() {
  await run(async () => {
    const payload = {
      bleName: form.bleName.trim(),
    };
    await catalog.saveSettings(payload);
  }, '蓝牙设置已保存');
}

async function submitIrSettings() {
  await run(async () => {
    const payload = {
      minPulses: Number(form.minPulses),
      irTxMode: form.irTxMode,
    };
    await catalog.saveSettings(payload);
  }, '红外设置已保存');
}

function pairHost() {
  if (device.status.ble?.linkConnected && !confirm('添加新主机会断开当前蓝牙连接，是否继续？')) return;
  run(() => catalog.pairHost(), '已开放配对窗口');
}

const saveHost = (host, payload) => run(() => catalog.save('hosts', {id: host.id, ...payload}), '主机名称已保存');

async function forgetHost(host) {
  if (!confirm('忘记主机并删除配对密钥？')) return;
  await run(() => catalog.remove('hosts', host.id), '已忘记主机');
}

const connectHost = host => run(() => catalog.reconnectToHost(host.id), `正在尝试连接 ${host.address}`);

const exportData = scope => run(() => device.exportData(scope), '数据已导出');

async function resetSystem() {
  if (!confirm('恢复初始状态将格式化整个文件系统（含前端页面），并清除所有已配对蓝牙设备，不可撤销。是否继续？')) return;
  await run(async () => {
    await catalog.systemReset();
  }, '已恢复初始状态');
}
</script>

<template>
  <section ref="formEl">
    <!-- 无线网 -->
    <article>
      <h2>Wi-Fi</h2>
      <form @submit.prevent="submitWifiSettings">
        <h3>热点设置</h3>
        <div class="fields">
          <label>热点名称<input v-model="form.apSsid" maxlength="31" required></label>
          <label>热点密码（留空为开放热点，或 8-63 位）<input v-model="form.apPassword" maxlength="63" autocomplete="off"></label>
        </div>
        <div class="actions">
          <button :disabled="busy">保存热点设置</button>
        </div>
      </form>

      <h3>已连接设备<span class="station-count">{{ stationCount }}</span></h3>
      <div v-if="wifiStations.length" class="stations">
        <div v-for="sta in wifiStations" :key="sta.mac" class="station-item">
          <span class="station-mac">{{ sta.mac }}</span>
          <span v-if="sta.rssi" class="station-rssi">{{ sta.rssi }} dBm</span>
        </div>
      </div>
      <p v-else class="hint">暂无设备连接到此热点。</p>
    </article>

    <!-- 蓝牙 -->
    <article>
      <h2>蓝牙</h2>
      <form @submit.prevent="submitBleSettings">
        <h3>蓝牙设置</h3>
        <label>蓝牙设备名称（1-29 字节）<input v-model="form.bleName" maxlength="29" required></label>
        <div class="actions">
          <button :disabled="busy">保存蓝牙设置</button>
        </div>
      </form>

      <h3>蓝牙状态</h3>
      <div class="ble-status">
        <div v-for="item in bleStatus" :key="item.label" class="ble-status-item">
          <span class="ble-status-label">{{ item.label }}</span>
          <span class="ble-status-value" :class="{ok: item.ok, warn: !item.ok && item.ok !== false}">{{ item.value }}</span>
        </div>
      </div>

      <h3>蓝牙主机</h3>
      <div class="actions">
        <button :disabled="busy" @click="pairHost">添加新主机（断开当前连接）</button>
      </div>

      <HostCard
        v-for="host in catalog.hosts"
        :key="host.id"
        :host="host"
        :disabled="busy"
        @save="payload => saveHost(host, payload)"
        @remove="forgetHost(host)"
        @connect="connectHost(host)"
      />
      <p v-if="!catalog.hosts.length" class="hint">尚无配对主机，请开放配对并在电脑上连接。</p>
    </article>

    <!-- 红外 -->
    <article>
      <h2>红外</h2>
      <form @submit.prevent="submitIrSettings">
        <h3>红外发射（GPIO4）</h3>
        <label>发射管驱动模式
          <select v-model="form.irTxMode">
            <option value="off">关闭（高阻）</option>
            <option value="push-pull">推挽（高电平有效）</option>
            <option value="sink">灌电流（低电平有效）</option>
          </select>
        </label>
        <p class="hint">默认关闭。推挽模式空闲为低电平；灌电流模式使用开漏输出，空闲释放引脚。请按接线选择模式，发射管必须限流，勿将 GPIO4 接到高于 3.3V 的电压。</p>
        <p class="hint">保存后立即生效并中止当前红外动作。采样未记录载波时使用协议常用频率，未知协议默认 38 kHz。</p>
        <h3>红外接收信号过滤</h3>
        <label>忽略脉冲数少于 n 的红外信号（0 为不过滤）
          <input v-model.number="form.minPulses" type="number" min="0" max="750" required>
        </label>
        <div class="actions">
          <button :disabled="busy">保存红外设置</button>
        </div>
      </form>
    </article>

    <!-- 存储 -->
    <article>
      <h2>存储</h2>
      <StorageBar />

      <h3>数据导出</h3>
      <div class="actions">
        <button class="secondary" :disabled="busy" @click="exportData('macros')">导出动作序列</button>
        <button class="secondary" :disabled="busy" @click="exportData('all')">导出全部业务数据</button>
      </div>
      <p class="hint">单个遥控器的导出请在「遥控器管理」页面操作</p>

      <h3>初始化</h3>
      <div class="actions">
        <button class="danger" :disabled="busy" @click="resetSystem">清除配置和数据</button>
      </div>
    </article>
  </section>
</template>
