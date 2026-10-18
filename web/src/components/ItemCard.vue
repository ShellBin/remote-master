<script setup>
// 列表项的统一外壳：标题 + 描述 + 操作按钮组。
// 相比原实现的 innerHTML 拼接 + data 属性事件委托，这里由父组件直接处理事件。
import {t} from '@/i18n';

defineProps({
  title: {type: String, required: true},
  description: {type: String, default: ''},
  /** 徽章文案，例如"未映射"；空字符串则不显示 */
  badge: {type: String, default: ''},
  editable: {type: Boolean, default: true},
  testable: {type: Boolean, default: false},
  removable: {type: Boolean, default: true},
  disabled: {type: Boolean, default: false},
});

defineEmits(['edit', 'test', 'remove']);
</script>

<template>
  <div class="item">
    <strong>{{ title }}</strong>
    <p>
      <slot>{{ description }}</slot>
      <em v-if="badge" class="badge">{{ badge }}</em>
    </p>
    <div class="actions">
      <button v-if="editable" class="secondary" :disabled="disabled" @click="$emit('edit')">{{ t('card.edit') }}</button>
      <button v-if="testable" class="secondary" :disabled="disabled" @click="$emit('test')">{{ t('card.test') }}</button>
      <button v-if="removable" class="danger" :disabled="disabled" @click="$emit('remove')">{{ t('card.delete') }}</button>
    </div>
  </div>
</template>
