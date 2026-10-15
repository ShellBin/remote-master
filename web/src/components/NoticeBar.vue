<script setup>
import {computed, ref, watch} from 'vue';
import {useDeviceStore} from '@/stores/device';

const device = useDeviceStore();

const visible = ref(false);
let timer = 0;

// 监听 notice 变化，显示 Toast 并自动消失
watch(() => device.notice.text, text => {
  clearTimeout(timer);
  if (text) {
    visible.value = true;
    timer = setTimeout(() => {
      visible.value = false;
      device.setNotice('');
    }, 3000);
  } else {
    visible.value = false;
  }
});

const toastClass = computed(() => ({
  'toast-enter': visible.value && device.notice.level === 'info',
  'toast-enter-error': visible.value && device.notice.level === 'error',
}));
</script>

<template>
  <Transition name="toast">
    <div v-if="visible" class="toast" :class="toastClass" role="status" aria-live="polite">
      {{ device.notice.text }}
    </div>
  </Transition>
</template>
