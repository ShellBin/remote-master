<script setup>
import {reactive} from 'vue';

// 蓝牙主机编辑表单。只在创建时把 host 的值拷进本地草稿，
// 之后不跟随 store 更新覆盖——否则设备的 state 推送会打断用户正在输入的内容。
const props = defineProps({
  host: {type: Object, required: true},
  disabled: {type: Boolean, default: false},
  // 设备侧是否仍保存着该主机的配对密钥；false 时"尝试连接"只能重新配对。
  bonded: {type: Boolean, default: true},
});
const emit = defineEmits(['save', 'remove', 'connect']);

const draft = reactive({
  name: props.host.name,
});

let savedName = props.host.name;

function handleBlur() {
  if (draft.name !== savedName) {
    savedName = draft.name;
    emit('save', {name: draft.name});
  }
}
</script>

<template>
  <form class="host-card" @submit.prevent>
    <div class="host-card-header">
      <span class="host-card-address">{{ host.address }}</span>
      <span v-if="!bonded" class="host-card-warning">设备无配对密钥</span>
    </div>
    <div class="host-card-body">
      <label>主机名称<input v-model="draft.name" maxlength="63" required @blur="handleBlur"></label>
      <p v-if="!bonded" class="hint">
        “尝试连接”会断开当前连接并开放 2 分钟配对窗口。若电脑仍连不上，请在系统蓝牙设置里删除这台设备后重新配对。
      </p>
    </div>
    <div class="host-card-actions">
      <button type="button" class="secondary" :disabled="disabled" @click="$emit('connect')">尝试连接</button>
      <button type="button" class="danger" :disabled="disabled" @click="$emit('remove')">忘记主机</button>
    </div>
  </form>
</template>
