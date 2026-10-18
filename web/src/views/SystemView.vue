<script setup>
import {computed, reactive, ref, watch} from 'vue';
import {useAction} from '@/composables/useAction';
import {useCatalogStore} from '@/stores/catalog';
import {useDeviceStore} from '@/stores/device';
import HostCard from '@/components/HostCard.vue';
import StorageBar from '@/components/StorageBar.vue';
import {t} from '@/i18n';

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

watch(() => catalog.settings, settings => {
  if (formEl.value?.contains(document.activeElement)) return;
  syncFromSettings(settings);
}, {immediate: true, deep: true});

// Disconnect reason labels (HCI error codes)
const disconnectReasons = {
  0x00: 'Normal',
  0x10: 'Remote user terminated',
  0x11: 'Remote device terminated (resources)',
  0x12: 'Remote device terminated (power off)',
  0x13: 'Local host terminated',
  0x22: 'Encryption mode unacceptable',
  0x29: 'Host terminated (user request)',
};

const disconnectReasonsZh = {
  0x00: '正常断开',
  0x10: '远端用户终止连接',
  0x11: '远端设备终止连接（资源不足）',
  0x12: '远端设备终止连接（电源关闭）',
  0x13: '本地主机终止连接',
  0x22: '加密模式不可接受',
  0x29: '主机终止（用户请求）',
};

const bleStatus = computed(() => {
  const ble = device.status.ble || {};
  const subscriptions = ble.reportSubscriptions || 0;
  const reports = [
    subscriptions & 1 ? 'Keyboard' : '',
    subscriptions & 2 ? 'Mouse' : '',
    subscriptions & 4 ? 'Media' : '',
  ].filter(Boolean);

  const locale = localStorage.getItem('locale') || 'zh';
  const reasons = locale === 'zh' ? disconnectReasonsZh : disconnectReasons;
  const disconnectCode = ble.lastDisconnectReason || 0;
  const disconnectHex = `0x${disconnectCode.toString(16).toUpperCase().padStart(2, '0')}`;

  return [
    {label: t('system.advertising'), value: ble.advertising ? t('system.advertising') : t('system.advertisingStopped'), ok: ble.advertising},
    {label: t('system.connected'), value: ble.linkConnected ? t('system.connected') : t('system.disconnected'), ok: ble.linkConnected},
    {label: t('system.encryptedDone'), value: ble.connected ? t('system.encryptedDone') : t('system.encryptedPending'), ok: ble.connected},
    {label: t('system.reportSubscribed'), value: reports.length ? reports.join(', ') : '—', ok: reports.length > 0},
    {label: t('system.bondCount', '0'), value: ble.bondCount ?? 0, ok: (ble.bondCount ?? 0) > 0},
    {label: t('system.disconnectReasonPrefix'), value: reasons[disconnectCode] || `${disconnectHex}`, ok: disconnectCode === 0},
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
    if (apChanged && !confirm(t('system.apChangeConfirm'))) {
      throw new Error(t('system.apChangeCancelled'));
    }
    await catalog.saveSettings(payload);
  }, t('system.wifiSaved'));
}

async function submitBleSettings() {
  await run(async () => {
    const payload = {
      bleName: form.bleName.trim(),
    };
    await catalog.saveSettings(payload);
  }, t('system.bleNameSaved'));
}

async function submitIrSettings() {
  await run(async () => {
    const payload = {
      minPulses: Number(form.minPulses),
      irTxMode: form.irTxMode,
    };
    await catalog.saveSettings(payload);
  }, t('system.irTxSaved'));
}

function pairHost() {
  if (device.status.ble?.linkConnected && !confirm(t('system.pairingDisconnect'))) return;
  run(() => catalog.pairHost(), t('system.pairingOpened'));
}

const saveHost = (host, payload) => run(() => catalog.save('hosts', {id: host.id, ...payload}), t('system.saved'));

async function forgetHost(host) {
  if (!confirm(t('system.forgetHost') + '?')) return;
  await run(() => catalog.remove('hosts', host.id), t('system.forgotten'));
}

const bondedDevices = computed(() => (
  Array.isArray(device.status.ble?.bonds) ? device.status.ble.bonds : null
));

const isHostBonded = host => !bondedDevices.value || bondedDevices.value.some(
  addr => String(addr).toLowerCase() === String(host.address).toLowerCase(),
);

const connectHost = host => run(
  () => catalog.reconnectToHost(host.id),
  isHostBonded(host)
    ? `${t('system.tryConnect')} ${host.address}`
    : `设备没有 ${host.name || host.address} 的配对密钥，已开放 2 分钟配对窗口；请在电脑蓝牙设置中重新连接该设备`,
);

async function resetSystem() {
  if (!confirm(t('system.resetConfirm'))) return;
  await run(async () => {
    await catalog.systemReset();
  }, t('system.resetStarted'));
}
</script>

<template>
  <section ref="formEl">
    <!-- Wi-Fi -->
    <article>
      <h2>{{ t('system.wifi') }}</h2>
      <form @submit.prevent="submitWifiSettings">
        <h3>{{ t('system.bleHotspot') }}</h3>
        <div class="fields">
          <label>{{ t('system.ssid') }}<input v-model="form.apSsid" maxlength="31" required></label>
          <label>{{ t('system.password') }} ({{ t('system.openNetwork') }} / {{ t('system.passwordHint') }})<input v-model="form.apPassword" maxlength="63" autocomplete="off"></label>
        </div>
        <div class="actions">
          <button :disabled="busy">{{ t('system.saveWifi') }}</button>
        </div>
      </form>
    </article>

    <!-- Bluetooth -->
    <article>
      <h2>{{ t('system.bluetooth') }}</h2>
      <form @submit.prevent="submitBleSettings">
        <h3>{{ t('system.bleSettings') }}</h3>
        <label>{{ t('system.bleName') }}<input v-model="form.bleName" maxlength="29" required></label>
        <div class="actions">
          <button :disabled="busy">{{ t('system.saveBleName') }}</button>
        </div>
      </form>

      <h3>{{ t('system.bleStatus') }}</h3>
      <div class="ble-status">
        <div v-for="item in bleStatus" :key="item.label" class="ble-status-item">
          <span class="ble-status-label">{{ item.label }}</span>
          <span class="ble-status-value" :class="{ok: item.ok, warn: !item.ok && item.ok !== false}">{{ item.value }}</span>
        </div>
      </div>

      <h3>{{ t('system.bleHost') }}</h3>
      <div class="actions">
        <button :disabled="busy" @click="pairHost">{{ t('system.addNewHost') }}</button>
      </div>

      <HostCard
        v-for="host in catalog.hosts"
        :key="host.id"
        :host="host"
        :bonded="isHostBonded(host)"
        :disabled="busy"
        @save="payload => saveHost(host, payload)"
        @remove="forgetHost(host)"
        @connect="connectHost(host)"
      />
      <p v-if="!catalog.hosts.length" class="hint">{{ t('system.noHosts') }}</p>
    </article>

    <!-- IR -->
    <article>
      <h2>{{ t('system.irTxTitle') }}</h2>
      <form @submit.prevent="submitIrSettings">
        <h3>{{ t('system.irTx') }}</h3>
        <label>{{ t('system.driveMode') }}
          <select v-model="form.irTxMode">
            <option value="off">{{ t('system.offHigh-Z') }}</option>
            <option value="push-pull">{{ t('system.pushPull') }}</option>
            <option value="sink">{{ t('system.sink') }}</option>
          </select>
        </label>
        <p class="hint">{{ t('system.irTxHint') }}</p>
        <p class="hint">{{ t('system.irTxHint2') }}</p>
        <h3>{{ t('system.irRxFilter') }}</h3>
        <label>{{ t('system.minPulses') }}
          <input v-model.number="form.minPulses" type="number" min="0" max="750" required>
        </label>
        <div class="actions">
          <button :disabled="busy">{{ t('system.saveIrSettings') }}</button>
        </div>
      </form>
    </article>

    <!-- Storage -->
    <article>
      <h2>{{ t('system.storage') }}</h2>
      <StorageBar />

      <h3>{{ t('system.factoryResetTitle') }}</h3>
      <div class="actions">
        <button class="danger" :disabled="busy" @click="resetSystem">{{ t('system.factoryReset') }}</button>
      </div>
      <p class="hint">{{ t('system.resetHint') }}</p>
    </article>
  </section>
</template>
