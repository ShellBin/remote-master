<script setup>
import {reactive} from 'vue';
import {t} from '@/i18n';

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
      <span v-if="!bonded" class="host-card-warning">{{ t('host.noBond') }}</span>
    </div>
    <div class="host-card-body">
      <label>{{ t('host.name') }}<input v-model="draft.name" maxlength="63" required @blur="handleBlur"></label>
      <p v-if="!bonded" class="hint">
        {{ t('host.connectHint') }}
      </p>
    </div>
    <div class="host-card-actions">
      <button type="button" class="secondary" :disabled="disabled" @click="$emit('connect')">{{ t('host.tryConnect') }}</button>
      <button type="button" class="danger" :disabled="disabled" @click="$emit('remove')">{{ t('host.forget') }}</button>
    </div>
  </form>
</template>
