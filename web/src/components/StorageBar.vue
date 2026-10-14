<script setup>
import {computed} from 'vue';
import {useDeviceStore} from '@/stores/device';

const device = useDeviceStore();

const total = computed(() => device.status.fsTotal || 0);
const used = computed(() => device.status.fsUsed || 0);
const percent = computed(() => (total.value ? Math.min(100, used.value / total.value * 100) : 0));
const kb = value => Math.round(value / 1024);
</script>

<template>
  <div class="storage">
    <template v-if="total">
      共 {{ kb(total) }} KB · 已用 {{ kb(used) }} KB · 剩余 <strong>{{ kb(total - used) }} KB</strong>
      <div class="bar"><i :style="{width: `${percent.toFixed(1)}%`}" /></div>
    </template>
    <template v-else>文件系统未挂载，无法统计存储。</template>
  </div>
</template>
