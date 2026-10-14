<script setup>
import {useDeviceStore} from '@/stores/device';

const device = useDeviceStore();
</script>

<template>
  <p class="notice" :class="device.notice.level" role="status" aria-live="polite">
    {{ device.notice.text }}
  </p>
</template>
