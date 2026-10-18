<script setup>
import {computed} from 'vue';
import {useDeviceStore} from '@/stores/device';
import {t} from '@/i18n';

const device = useDeviceStore();

const total = computed(() => device.status.fsTotal || 0);
const used = computed(() => device.status.fsUsed || 0);
const percent = computed(() => (total.value ? Math.min(100, used.value / total.value * 100) : 0));
const kb = value => Math.round(value / 1024);
</script>

<template>
  <div class="storage">
    <template v-if="total">
      {{ t('storage.total', kb(total), kb(used), kb(total - used)) }}
      <div class="bar"><i :style="{width: `${percent.toFixed(1)}%`}" /></div>
    </template>
    <template v-else>{{ t('storage.unmounted') }}</template>
  </div>
</template>
