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
    </header>

    <!-- 状态指示器顺序：蓝牙、WebSocket、存储 -->
    <div class="chips status-bar" role="status" aria-label="设备状态">
      <StatusChips />
      <span class="chip connection">{{ device.connectionLabel }}</span>
    </div>

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
  </main>
  <!-- Toast 提示：fixed 定位显示在页面顶部 -->
  <NoticeBar />
</template>

<style>
/* 全局共享样式：主题变量 + 基础布局 + 通用组件 */

:root{color-scheme:dark;--bg:#101827;--card:#182437;--line:#293a52;--text:#edf4ff;--muted:#9aacc4;--accent:#69d3bd;--ok:#4caf7d;--warn:#e0a640;--danger:#af3949}

body{margin:0;background:var(--bg);color:var(--text);font:15px/1.6 system-ui,sans-serif}

main{max-width:1150px;margin:auto;padding:32px 20px}

header{display:flex;justify-content:space-between;align-items:center;gap:16px;flex-wrap:wrap}

h1{margin:2px 0 16px;font-size:30px}
small,.hint,footer{color:var(--muted)}

h2{font-size:18px;margin:0 0 18px}
h3{font-size:14px;margin:22px 0 6px;color:var(--muted);text-transform:uppercase;letter-spacing:.04em}

nav{display:flex;gap:8px;border-bottom:1px solid var(--line);padding:10px 0 18px;overflow:auto}
nav a{display:inline-block;background:#26344a;color:var(--text);padding:9px 15px;border-radius:8px;text-decoration:none;white-space:nowrap}
nav a.router-link-active{background:var(--accent);color:#06231c}

/* 表单元素 */
button,input,select{font:inherit}
button{border:0;background:var(--accent);color:#06231c;padding:9px 15px;border-radius:8px;cursor:pointer;white-space:nowrap}
button.secondary{background:#26344a;color:var(--text)}
.danger{background:var(--danger);color:#fff}
button:disabled{opacity:.45;cursor:not-allowed}

label{display:block;margin:12px 0}
label.check{display:flex;align-items:center;gap:8px;color:var(--muted)}
label.check input{margin:0}
input:not([type=checkbox]),select{display:block;width:100%;margin-top:6px;padding:10px;border:1px solid #405370;border-radius:7px;background:#101b2c;color:var(--text)}
input[readonly]{color:var(--muted)}
input[type=checkbox]{accent-color:var(--accent)}

/* 通用布局 */
article{padding:24px;background:var(--card);border:1px solid var(--line);border-radius:12px;margin-top:20px;min-width:0}
.columns{display:grid;grid-template-columns:1fr 1fr;gap:20px}
@media (max-width:880px){.columns{grid-template-columns:1fr}main{padding:20px 14px}}

.actions,.fields{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.fields>label{flex:1;min-width:90px}
.actions label{margin:8px 0}

/* 列表项 */
.item,.step{border-top:1px solid var(--line);padding:16px 0;overflow-wrap:anywhere}
.item strong{display:block}
.item p{margin:4px 0 12px;color:var(--muted)}

/* 步骤 */
.steps{margin:18px 0}
.step{display:grid;grid-template-columns:110px 1fr auto auto;gap:8px;align-items:center}
.step input,.step select{margin:0}
.step>div{display:flex;gap:8px;flex-wrap:wrap}
.step>div>*{flex:1;min-width:110px}

/* 分组 */
.group{margin-top:10px}
.group h3{margin:18px 0 0;color:var(--text);text-transform:none;letter-spacing:0;font-size:15px}
.group h3 small{margin-left:10px;font-weight:400}

/* Chips */
.chips{display:flex;gap:8px;flex-wrap:wrap}
.chip{border:1px solid var(--line);background:#141f31;padding:5px 12px;border-radius:18px;font-size:13px;color:var(--muted)}
.chip.ok{border-color:var(--ok);color:var(--ok)}
.chip.learn{border-color:var(--warn);color:var(--warn)}
.chip.danger{border-color:var(--danger);color:var(--danger)}
.chip.connection{border:1px solid var(--line)}

/* 状态栏 */
.status-bar{align-items:center}

/* Badge */
.badge{display:inline-block;border:1px solid var(--warn);color:var(--warn);border-radius:5px;font-size:11px;font-style:normal;padding:0 5px;margin-left:6px;vertical-align:1px;white-space:nowrap}

/* Segment */
.segment{display:flex;gap:16px;flex-wrap:wrap;margin:12px 0 16px}
.segment label{display:flex;align-items:center;gap:6px;margin:0;color:var(--text);font-weight:500}
.segment input{margin:0}

/* Storage */
.storage{color:var(--muted)}
.storage strong{color:var(--text)}
.bar{height:8px;background:#101b2c;border:1px solid var(--line);border-radius:6px;margin-top:10px;overflow:hidden}
.bar i{display:block;height:100%;background:var(--accent)}

/* Footer */
footer{margin:24px 0;text-align:center;font-size:12px}

.ble-status{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px;margin:8px 0}
.ble-status-item{display:flex;flex-direction:column;gap:2px;padding:8px 10px;background:#141f31;border:1px solid var(--line);border-radius:6px}
.ble-status-label{font-size:11px;color:var(--muted);text-transform:none}
.ble-status-value{font-size:13px;font-weight:500;color:var(--text)}
.ble-status-value.ok{color:var(--ok)}
.ble-status-value.warn{color:var(--muted)}

/* 蓝牙主机卡片 */
.host-card{background:#182437;border:1px solid var(--line);border-radius:10px;overflow:hidden;margin:12px 0}
.host-card-header{padding:12px 16px;background:#141f31;border-bottom:1px solid var(--line)}
.host-card-address{font-family:monospace;font-size:14px;font-weight:500;color:var(--text)}
.host-card-body{padding:12px 16px 0}
.host-card-body label{margin:0;font-size:13px;color:var(--muted)}
.host-card-body input{margin-top:4px}
.host-card-actions{display:flex;gap:8px;flex-wrap:wrap;padding:12px 16px}

/* 模态框 */
.modal-backdrop{position:fixed;inset:0;background:rgba(0,0,0,.6);display:flex;align-items:center;justify-content:center;z-index:1000;padding:20px}
.modal{background:#182437;border:1px solid var(--line);border-radius:12px;padding:24px;width:100%;max-width:420px;box-shadow:0 8px 40px rgba(0,0,0,.5)}
.modal h3{margin:0 0 16px;font-size:16px}
.modal-actions{display:flex;gap:8px;margin-top:20px}
.modal-actions button{flex:1}
</style>
