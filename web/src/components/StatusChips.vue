<script setup>
import {computed} from 'vue';
import {useDeviceStore} from '@/stores/device';
import {useCatalogStore} from '@/stores/catalog';

const device = useDeviceStore();
const catalog = useCatalogStore();

const ble = computed(() => device.status.ble || {});

const bleChip = computed(() => {
  const info = ble.value;
  if (info.connected) {
    const hostName = catalog.hostName(info.host) || info.host || '主机';
    return {text: `蓝牙已加密连接 · ${hostName}`, tone: 'ok'};
  }
  if (info.tryingHost) {
    const hostName = catalog.hostName(info.tryingHost) || tryingHost;
    return {text: `蓝牙正在尝试连接 · ${hostName}`, tone: ''};
  }
  if (info.pairingAlwaysOpen) return {text: '蓝牙配对开放（尚无主机）', tone: ''};
  if (info.pairing) return {text: '蓝牙新主机配对窗口开放中', tone: ''};
  if (info.advertising) return {text: '蓝牙广播中（等待上次连接的主机）', tone: ''};
  return {text: '蓝牙未广播', tone: ''};
});

const learningText = computed(() => (
  device.status.learningRemote ? `学习中 · ${catalog.remoteName(device.status.learningRemote)}` : ''
));

const storageChip = computed(() => {
  const total = device.status.fsTotal || 0;
  const used = device.status.fsUsed || 0;
  if (!total) return {text: '存储不可用', tone: 'danger'};
  return {text: `剩余存储 ${Math.round((total - used) / 1024)} KB`, tone: ''};
});
</script>

<template>
  <div class="chips-inline" role="status" aria-label="设备状态">
    <span class="chip" :class="bleChip.tone">{{ bleChip.text }}</span>
    <span v-if="learningText" class="chip learn">{{ learningText }}</span>
    <span class="chip" :class="storageChip.tone">{{ storageChip.text }}</span>
  </div>
</template>

<style scoped>
.chips-inline{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
</style>
