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
const form = reactive({apSsid: '', apPassword: '', bleName: '', minPulses: 8});

function syncFromSettings(settings) {
  form.apSsid = settings.apSsid || '';
  form.apPassword = settings.apPassword || '';
  form.bleName = settings.bleName || '';
  form.minPulses = settings.minPulses ?? 8;
}

// 状态推送会频繁到来，但用户正在输入时不回填，否则光标会被抢走。
watch(() => catalog.settings, settings => {
  if (formEl.value?.contains(document.activeElement)) return;
  syncFromSettings(settings);
}, {immediate: true, deep: true});

const bleDiagnostics = computed(() => {
  const ble = device.status.ble || {};
  const subscriptions = ble.reportSubscriptions || 0;
  const reports = [
    subscriptions & 1 ? '键盘' : '',
    subscriptions & 2 ? '鼠标' : '',
    subscriptions & 4 ? '媒体' : '',
  ].filter(Boolean).join('、') || '无';
  return `广播：${ble.advertising ? '进行中' : '停止'}`
    + ` · 链路：${ble.linkConnected ? '已连接' : '未连接'}`
    + ` · 加密：${ble.connected ? '完成' : '未完成'}`
    + ` · 报告订阅：${reports}`
    + ` · 配对主机数：${ble.bondCount ?? 0}`
    + ` · 上次断开原因：${ble.lastDisconnectReason || 0}`
    + ` · 空闲堆：${device.status.heap ?? '?'} B`;
});

async function submitSettings() {
  await run(async () => {
    const payload = {
      apSsid: form.apSsid.trim(),
      apPassword: form.apPassword,
      bleName: form.bleName.trim(),
      minPulses: Number(form.minPulses),
    };
    const current = catalog.settings;
    const apChanged = payload.apSsid !== current.apSsid || payload.apPassword !== current.apPassword;
    if (apChanged && !confirm(
      '热点名称或密码将变更，设备会重启热点并断开当前连接，需要重新连接 WiFi。是否继续？',
    )) {
      throw new Error('已取消，设置未保存');
    }
    await catalog.saveSettings(payload);
  }, '设置已保存');
}

function pairHost() {
  if (device.status.ble?.linkConnected && !confirm('添加新主机会断开当前蓝牙连接，是否继续？')) return;
  run(() => catalog.pairHost(), '已开放配对窗口');
}

const reconnectHosts = () => run(() => catalog.reconnectHosts(), '已重启优先级广播');
const saveHost = (host, payload) => run(() => catalog.save('hosts', {id: host.id, ...payload}), '主机已保存');

async function forgetHost(host) {
  if (!confirm('忘记主机并删除配对密钥？')) return;
  await run(() => catalog.remove('hosts', host.id), '已忘记主机');
}

const exportData = scope => run(() => device.exportData(scope), '数据已导出');
</script>

<template>
  <section>
    <article>
      <h2>设备设置</h2>
      <form ref="formEl" @submit.prevent="submitSettings">
        <h3>无线热点</h3>
        <div class="fields">
          <label>热点名称<input v-model="form.apSsid" maxlength="31" required></label>
          <label>热点密码（8-63 位）<input v-model="form.apPassword" maxlength="63" required autocomplete="off"></label>
        </div>

        <h3>蓝牙</h3>
        <label>蓝牙设备名称（1-29 字节）<input v-model="form.bleName" maxlength="29" required></label>

        <h3>信号过滤</h3>
        <label>忽略脉冲数少于 n 的红外信号（0 为不过滤）
          <input v-model.number="form.minPulses" type="number" min="0" max="750" required>
        </label>
        <p class="hint">
          过滤同时作用于学习与执行阶段的非重复帧。一帧 NEC 约 67 段脉冲，残余干扰通常只有几段，
          默认 8 即可滤除；重复帧不受影响，长按仍然有效。
        </p>

        <div class="actions">
          <button :disabled="busy">保存设置</button>
        </div>
        <p class="hint">
          修改热点名称或密码后热点会立即重启，需要重新连接 WiFi。
          部分主机会缓存蓝牙设备名，可能要重新配对后才显示新名称。
        </p>
      </form>
    </article>

    <article>
      <h2>存储空间</h2>
      <StorageBar />
      <p class="hint">
        配置、学习到的原始脉冲与前端页面共用这一空间；剩余空间不足时无法保存新的学习数据，
        可删除旧按键释放空间。
      </p>
    </article>

    <article>
      <h2>蓝牙状态</h2>
      <p class="hint">{{ bleDiagnostics }}</p>
    </article>

    <article>
      <h2>蓝牙主机</h2>
      <p class="hint">
        没有已配对主机时持续开放配对。同时只连接一个主机；添加新主机会断开当前连接并开放配对 2 分钟，
        旧主机在窗口内不能抢先重连。新主机配对完成后窗口自动关闭，原有主机记录保留。
        已配对主机按优先级轮流获得 10 秒连接窗口。
      </p>
      <div class="actions">
        <button :disabled="busy" @click="pairHost">添加新主机（断开当前连接）</button>
        <button class="secondary" :disabled="busy" @click="reconnectHosts">重启优先级广播</button>
      </div>

      <HostCard
        v-for="host in catalog.hosts"
        :key="host.id"
        :host="host"
        :disabled="busy"
        @save="payload => saveHost(host, payload)"
        @remove="forgetHost(host)"
      />
      <p v-if="!catalog.hosts.length" class="hint">尚无配对主机，请开放配对并在电脑上连接。</p>

      <p class="hint">
        修改固件后仍无法连接时，先在电脑中移除旧设备，再在此处忘记主机并重新配对。
        忘记主机会删除设备端的配对密钥。
      </p>
      <p class="hint">
        Mac 不显示连接按钮时，可在项目目录执行 swift mac-pair.swift。
        该工具通过 CoreBluetooth 发起连接，不发送按键，不使用串口。
      </p>
    </article>

    <article>
      <h2>数据导出</h2>
      <div class="actions">
        <button class="secondary" :disabled="busy" @click="exportData('remotes')">导出遥控器与原始红外数据</button>
        <button class="secondary" :disabled="busy" @click="exportData('macros')">导出动作序列</button>
        <button class="secondary" :disabled="busy" @click="exportData('all')">导出全部业务数据</button>
      </div>
    </article>
  </section>
</template>
