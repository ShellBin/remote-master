<script setup>
import {ref} from 'vue';
import {hidKeyLabel, MODIFIERS} from '@/lib/hid';
import {MACRO_LIMITS, STEP_KINDS} from '@/lib/mapping';
import {useCatalogStore} from '@/stores/catalog';
import KeyPickerModal from '@/components/KeyPickerModal.vue';

const props = defineProps({
  modelValue: {type: Array, required: true},
});
const emit = defineEmits(['update:modelValue', 'limit']);

const catalog = useCatalogStore();

// 模态框状态：{ stepIndex, mode } | null
const picker = ref(null);

// 拖拽状态
const dragIndex = ref(-1);
const dragOverIndex = ref(-1);

function openKeyPicker(index) {
  picker.value = {stepIndex: index, mode: 'hid'};
}

function openIrPicker(index) {
  picker.value = {stepIndex: index, mode: 'ir'};
}

function onPickerSelect(result) {
  if (!picker.value) return;
  const {stepIndex, mode} = picker.value;
  if (mode === 'hid') {
    patch(stepIndex, {key: result.key, modifiers: result.modifiers});
  } else {
    patch(stepIndex, {keyId: result});
  }
  picker.value = null;
}

function closePicker() {
  picker.value = null;
}

// 修饰键：位图 ↔ 数组互转
function modToArray(mod) {
  return MODIFIERS.filter(m => mod & m.value).map(m => m.value);
}
function arrayToMod(arr) {
  return arr.reduce((sum, v) => sum | v, 0);
}

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

function toggleMod(index, step, modValue) {
  const arr = modToArray(step.modifiers);
  const next = arr.includes(modValue) ? arr.filter(v => v !== modValue) : [...arr, modValue];
  patch(index, {modifiers: arrayToMod(next)});
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

// 红外按键显示文本
function irKeyText(keyId) {
  const key = catalog.keyById(keyId);
  if (!key) return '（缺失按键）';
  return `${catalog.remoteName(key.remoteId)} / ${key.name}`;
}

// 当前选中 HID 键的修饰键文本
function modText(mod) {
  if (!mod) return '';
  const labels = MODIFIERS.filter(m => mod & m.value).map(m => m.label.replace(' / Win', ''));
  return labels.length ? ' + ' + labels.join('+') : '';
}

// ---------- 拖拽排序 ----------
function onDragStart(event, index) {
  dragIndex.value = index;
  event.dataTransfer.effectAllowed = 'move';
  event.dataTransfer.setData('text/plain', String(index));
}

function onDragOver(event, index) {
  event.preventDefault();
  dragOverIndex.value = index;
}

function onDragLeave() {
  dragOverIndex.value = -1;
}

function onDrop(event, index) {
  event.preventDefault();
  const from = dragIndex.value;
  const to = index;
  dragIndex.value = -1;
  dragOverIndex.value = -1;
  if (from === -1 || from === to) return;
  const next = props.modelValue.slice();
  const [moved] = next.splice(from, 1);
  next.splice(to, 0, moved);
  emit('update:modelValue', next);
}

function onDragEnd() {
  dragIndex.value = -1;
  dragOverIndex.value = -1;
}
</script>

<template>
  <div class="steps">
    <div
      v-for="(step, index) in modelValue"
      :key="index"
      class="step"
      :class="{'drag-over': dragOverIndex === index, 'dragging': dragIndex === index}"
      draggable="true"
      @dragstart="onDragStart($event, index)"
      @dragover="onDragOver($event, index)"
      @dragleave="onDragLeave"
      @drop="onDrop($event, index)"
      @dragend="onDragEnd"
    >
      <div class="step-drag" aria-label="拖拽排序">
        <span class="step-drag-grip">⋮⋮</span>
        <select
          :value="step.kind"
          aria-label="步骤类型"
          @change="changeKind(index, $event.target.value)"
          @click.stop
        >
          <option v-for="kind in STEP_KINDS" :key="kind.value" :value="kind.value">{{ kind.label }}</option>
        </select>
      </div>

      <div class="step-body">
        <template v-if="step.kind === 'key'">
          <button type="button" class="secondary step-key-btn" @click="openKeyPicker(index)">
            {{ hidKeyLabel(step.key) || '选择按键' }}{{ modText(step.modifiers) }}
          </button>
          <div class="mod-row">
            <label v-for="mod in MODIFIERS" :key="mod.value" class="check">
              <input
                type="checkbox"
                :checked="(step.modifiers & mod.value) !== 0"
                @change="toggleMod(index, step, mod.value)"
              >{{ mod.label }}
            </label>
          </div>
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

        <button v-else type="button" class="secondary step-key-btn" @click="openIrPicker(index)">
          {{ irKeyText(step.keyId) }}
        </button>
      </div>

      <button type="button" class="danger" aria-label="删除步骤" @click="removeStep(index)">删除</button>
    </div>

    <p v-if="!modelValue.length" class="hint">尚未添加步骤，动作序列至少需要一步才能保存。</p>
    <button type="button" class="secondary" @click="addStep">增加步骤</button>
  </div>

  <KeyPickerModal
    v-if="picker"
    :mode="picker.mode"
    :value="picker.mode === 'hid' ? modelValue[picker.stepIndex]?.key : modelValue[picker.stepIndex]?.keyId"
    @select="onPickerSelect"
    @close="closePicker"
  />
</template>

<style scoped>
.step-drag{display:flex;align-items:center;gap:8px}
.step-drag-grip{color:var(--muted);font-size:12px;cursor:grab;user-select:none;letter-spacing:1px}
.step.dragging{opacity:.4}
.step.drag-over{border-color:var(--accent)}
.step-key-btn{flex:1;text-align:left;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.mod-row{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
</style>