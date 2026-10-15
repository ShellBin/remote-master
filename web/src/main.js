import {createApp} from 'vue';
import {createPinia} from 'pinia';
import App from './App.vue';
import router from './router';
import './assets/styles/main.css';

const app = createApp(App);

// 自动聚焦指令：用于点击切换为输入框时立即聚焦
app.directive('focus', {
  mounted(el) {
    el.focus();
    el.select();
  },
});

app.use(createPinia()).use(router).mount('#app');
