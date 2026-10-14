<script setup>
import {computed} from 'vue';
import {HID_KEYS, MODIFIERS} from '@/lib/hid';
import {MACRO_LIMITS, STEP_KINDS} from '@/lib/mapping';
import {useCatalogStore} from '@/stores/catalog';

// 动作序列的步骤编辑器。单向数据流：所有修改都生成新数组后 emit，
// 父组件用 v-model 接收，避免直接改写 props。
const props = defineProps({
  modelValue: {type: Array, required: true},
});
const emit = defineEmits(['update:modelValue', 'limit']);

const catalog = useCatalogStore();

/** 按遥控器分组的红外按键，用于"红外重放"步骤的下拉（只有带原始脉冲的按键可用）。 */
const irGroups = computed(() => catalog.remotes
  .map(remote => ({remote, keys: catalog.irKeysOfRemote(remote.id)}))
  .filter(group => group.keys.length));

function defaultStep(kind) {
  if (kind === 'text') return {kind, text: ''};
  if (kind === 'wait') return {kind, ms: 200};
  if (kind === 'ir') return {kind, keyId: catalog.irCapableKeys[0]?.id || ''};
  return {kind: 'key', key: 4, modifiers: 0};
}

function patch(index, changes) {
  emit('update:modelValue', props.modelValue.map((step, i) => (
    i === index ? {...step, ...changes} : step
  )));
}

function changeKind(index, kind) {
  const next = props.modelValue.slice();
  next[index] = defaultStep(kind);
  emit('update:modelValue', next);
}

function addStep() {
  if (props.modelValue.length >= MACRO_LIMITS.maxSteps) {
    emit('limit', `最多 ${MACRO_LIMITS.maxSteps} 个步骤`);
    return;
  }
  emit('update:modelValue', [...props.modelValue, defaultStep('key')]);
}

function removeStep(index) {
  emit('update:modelValue', props.modelValue.filter((_, i) => i !== index));
}
</script>

<template>
  <div class="steps">
    <div v-for="(step, index) in modelValue" :key="index" class="step">
      <select
        :value="step.kind"
        aria-label="步骤类型"
        @change="changeKind(index, $event.target.value)"
      >
        <option v-for="kind in STEP_KINDS" :key="kind.value" :value="kind.value">{{ kind.label }}</option>
      </select>

      <div>
        <template v-if="step.kind === 'key'">
          <select
            :value="step.key"
            aria-label="HID 键码"
            @change="patch(index, {key: Number($event.target.value)})"
          >
            <option v-for="item in HID_KEYS" :key="item.value" :value="item.value">{{ item.label }}</option>
          </select>
          <select
            :value="step.modifiers"
            aria-label="修饰键位图"
            @change="patch(index, {modifiers: Number($event.target.value)})"
          >
            <option :value="0">无修饰键</option>
            <option v-for="modifier in MODIFIERS" :key="modifier.value" :value="modifier.value">
              {{ modifier.label }}
            </option>
          </select>
        </template>

        <input
          v-else-if="step.kind === 'text'"
          :value="step.text"
          aria-label="ASCII 文本"
          maxlength="512"
          placeholder="要输入的文本"
          @input="patch(index, {text: $event.target.value})"
        >

        <input
          v-else-if="step.kind === 'wait'"
          :value="step.ms"
          type="number"
          min="0"
          max="30000"
          aria-label="等待毫秒"
          @input="patch(index, {ms: Number($event.target.value)})"
        >

        <select
          v-else
          :value="step.keyId"
          aria-label="重放的红外按键"
          @change="patch(index, {keyId: $event.target.value})"
        >
          <option value="">请选择红外按键</option>
          <optgroup v-for="group in irGroups" :key="group.remote.id" :label="group.remote.name">
            <option v-for="key in group.keys" :key="key.id" :value="key.id">{{ key.name }}</option>
          </optgroup>
        </select>
      </div>

      <button type="button" class="danger" aria-label="删除步骤" @click="removeStep(index)">删除</button>
    </div>

    <p v-if="!modelValue.length" class="hint">尚未添加步骤，动作序列至少需要一步才能保存。</p>
    <button type="button" class="secondary" @click="addStep">增加步骤</button>
  </div>
</template>
