<script setup>
import {reactive} from 'vue';

// 蓝牙主机编辑表单。只在创建时把 host 的值拷进本地草稿，
// 之后不跟随 store 更新覆盖——否则设备的 state 推送会打断用户正在输入的内容。
const props = defineProps({
  host: {type: Object, required: true},
  disabled: {type: Boolean, default: false},
});
defineEmits(['save', 'remove']);

const draft = reactive({
  name: props.host.name,
  priority: props.host.priority,
  enabled: Boolean(props.host.enabled),
});
</script>

<template>
  <form class="item host-form" @submit.prevent="$emit('save', {
    name: draft.name,
    priority: Number(draft.priority),
    enabled: draft.enabled,
  })">
    <strong>{{ host.address }}</strong>
    <label>主机名称<input v-model="draft.name" maxlength="63" required></label>
    <label>优先级（越小越优先）<input v-model="draft.priority" type="number" min="0" max="99" required></label>
    <label class="check"><input v-model="draft.enabled" type="checkbox">允许自动重连</label>
    <div class="actions">
      <button :disabled="disabled">保存主机</button>
      <button type="button" class="danger" :disabled="disabled" @click="$emit('remove')">忘记主机</button>
    </div>
  </form>
</template>
