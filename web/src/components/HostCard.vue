<script setup>
import {reactive} from 'vue';

// 蓝牙主机编辑表单。只在创建时把 host 的值拷进本地草稿，
// 之后不跟随 store 更新覆盖——否则设备的 state 推送会打断用户正在输入的内容。
const props = defineProps({
  host: {type: Object, required: true},
  disabled: {type: Boolean, default: false},
});
defineEmits(['save', 'remove', 'connect']);

const draft = reactive({
  name: props.host.name,
});
</script>

<template>
  <form class="item host-form" @submit.prevent="$emit('save', {name: draft.name})">
    <strong>{{ host.address }}</strong>
    <label>主机名称<input v-model="draft.name" maxlength="63" required></label>
    <div class="actions">
      <button :disabled="disabled">保存名称</button>
      <button type="button" class="secondary" :disabled="disabled" @click="$emit('connect')">尝试连接</button>
      <button type="button" class="danger" :disabled="disabled" @click="$emit('remove')">忘记主机</button>
    </div>
  </form>
</template>
