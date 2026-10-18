<script setup>
import {computed} from 'vue';
import {useDeviceStore} from '@/stores/device';
import {useCatalogStore} from '@/stores/catalog';
import {t} from '@/i18n';

const device = useDeviceStore();
const catalog = useCatalogStore();

const ble = computed(() => device.status.ble || {});

const bleChip = computed(() => {
  const info = ble.value;
  if (info.connected) {
    const hostName = catalog.hostName(info.host) || info.host || '主机';
    return {text: t('status.bleEncrypted', hostName), tone: 'ok'};
  }
  if (info.tryingHost) {
    const hostName = catalog.hostName(info.tryingHost) || tryingHost;
    return {text: t('status.bleConnecting', hostName), tone: ''};
  }
  if (info.pairingAlwaysOpen) return {text: t('status.blePairingOpen'), tone: ''};
  if (info.pairing) return {text: t('status.bleNewHost'), tone: ''};
  if (info.advertising) return {text: t('status.bleAdvertising'), tone: ''};
  return {text: t('status.bleNotAdvertising'), tone: ''};
});

const learningText = computed(() => (
  device.status.learningRemote ? t('status.learning', catalog.remoteName(device.status.learningRemote)) : ''
));

const storageChip = computed(() => {
  const total = device.status.fsTotal || 0;
  const used = device.status.fsUsed || 0;
  if (!total) return {text: t('status.storageUnavailable'), tone: 'danger'};
  return {text: t('status.storageFree', Math.round((total - used) / 1024)), tone: ''};
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
