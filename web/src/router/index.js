import {createRouter, createWebHistory} from 'vue-router';

// 视图一律静态导入：ESP32 的 WebServer 是阻塞式 streamFile，
// 每个额外 chunk 都要多一次会占住 loop() 的 HTTP 请求，滚动体积极不划算。
import RemotesView from '@/views/RemotesView.vue';
import MacrosView from '@/views/MacrosView.vue';
import MappingsView from '@/views/MappingsView.vue';
import SystemView from '@/views/SystemView.vue';

const routes = [
  {path: '/', redirect: '/remotes'},
  {path: '/remotes', name: 'remotes', component: RemotesView, meta: {label: '遥控器与学习'}},
  {path: '/macros', name: 'macros', component: MacrosView, meta: {label: '动作与序列'}},
  {path: '/mappings', name: 'mappings', component: MappingsView, meta: {label: '按键映射'}},
  {path: '/system', name: 'system', component: SystemView, meta: {label: '系统'}},
  // 固件对不含扩展名的路径回退 index.html，因此深链接刷新由前端兜住。
  {path: '/:pathMatch(.*)*', redirect: '/remotes'},
];

export default createRouter({
  // history 模式依赖固件的 SPA 回退：无扩展名路径 → /frontend/index.html
  history: createWebHistory(),
  routes,
});
