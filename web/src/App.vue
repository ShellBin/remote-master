<script setup>
import {onMounted} from 'vue';
import {useRouter} from 'vue-router';
import {useDeviceStore} from '@/stores/device';
import StatusChips from '@/components/StatusChips.vue';
import NoticeBar from '@/components/NoticeBar.vue';

const device = useDeviceStore();
const router = useRouter();

// 导航项从路由表推导：新增带 meta.label 的路由会自动出现在这里。
const tabs = router.options.routes
  .filter(route => route.meta?.label)
  .map(route => ({name: route.name, label: route.meta.label}));

onMounted(() => device.connect());
</script>

<template>
  <main>
    <header>
      <div>
        <small>ESP32-C3 BLE/IR 多功能遥控平台</small>
        <h1>Remote Master</h1>
      </div>
      <span class="connection" role="status">{{ device.connectionLabel }}</span>
    </header>

    <StatusChips />
    <NoticeBar />

    <nav aria-label="功能">
      <RouterLink v-for="tab in tabs" :key="tab.name" :to="{name: tab.name}">
        {{ tab.label }}
      </RouterLink>
    </nav>

    <!-- KeepAlive：切走再回来时未提交的表单内容还在，与原实现（tab 只是 hidden）一致 -->
    <RouterView v-slot="{Component}">
      <KeepAlive>
        <component :is="Component" />
      </KeepAlive>
    </RouterView>

    <footer>配置保存到 LittleFS · WebSocket 事件通信 · 不轮询设备</footer>
  </main>
</template>
