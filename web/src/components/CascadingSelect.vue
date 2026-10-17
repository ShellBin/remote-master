<script setup>
import {computed, onBeforeUnmount, onMounted, ref} from 'vue';

// 多级分组下拉：点击触发按钮 → 弹出面板 → 分类作为行，点击分类展开子项 → 点击子项选中。
// groups: [{label, children: [{label, value}]}]
const props = defineProps({
  modelValue: {type: [Number, String], default: ''},
  groups: {type: Array, required: true},
  placeholder: {type: String, default: '请选择'},
});
const emit = defineEmits(['update:modelValue']);

const open = ref(false);
const expandedGroup = ref(null);
const rootEl = ref(null);

const currentLabel = computed(() => {
  for (const group of props.groups) {
    const item = group.children.find(child => child.value === props.modelValue);
    if (item) return `${group.label} · ${item.label}`;
  }
  return '';
});

function toggle() {
  open.value = !open.value;
  if (!open.value) expandedGroup.value = null;
}

function selectGroup(group) {
  // 若该组只有一个子项，直接选中
  if (group.children.length === 1) {
    emit('update:modelValue', group.children[0].value);
    open.value = false;
    expandedGroup.value = null;
    return;
  }
  expandedGroup.value = expandedGroup.value === group.label ? null : group.label;
}

function selectItem(value) {
  emit('update:modelValue', value);
  open.value = false;
  expandedGroup.value = null;
}

function onClickOutside(event) {
  if (rootEl.value && !rootEl.value.contains(event.target)) {
    open.value = false;
    expandedGroup.value = null;
  }
}

onMounted(() => document.addEventListener('mousedown', onClickOutside));
onBeforeUnmount(() => document.removeEventListener('mousedown', onClickOutside));
</script>

<template>
  <div ref="rootEl" class="cs">
    <button type="button" class="cs-trigger secondary" @click="toggle">
      {{ currentLabel || placeholder }}
      <span class="cs-arrow" :class="{open}">&#9662;</span>
    </button>
    <div v-if="open" class="cs-panel">
      <div v-for="group in groups" :key="group.label" class="cs-group">
        <div
          class="cs-group-label"
          :class="{expanded: expandedGroup === group.label}"
          @click="selectGroup(group)"
        >
          <span>{{ group.label }}</span>
          <span v-if="group.children.length > 1" class="cs-chevron">&#9662;</span>
        </div>
        <div v-if="expandedGroup === group.label || group.children.length === 1" class="cs-items">
          <div
            v-for="item in group.children"
            :key="item.value"
            class="cs-item"
            :class="{selected: item.value === modelValue}"
            @click="selectItem(item.value)"
          >{{ item.label }}</div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.cs{position:relative;display:inline-flex;min-width:0;flex:1}
.cs-trigger{display:flex;align-items:center;justify-content:space-between;width:100%;padding:10px;border:1px solid #405370;border-radius:7px;background:#101b2c;color:var(--text);font:inherit;font-size:14px;text-align:left;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.cs-trigger:hover{border-color:var(--accent)}
.cs-arrow{margin-left:8px;font-size:10px;transition:transform .15s;flex-shrink:0}
.cs-arrow.open{transform:rotate(180deg)}
.cs-panel{position:absolute;top:100%;left:0;right:0;z-index:50;margin-top:4px;background:#182437;border:1px solid var(--line);border-radius:8px;box-shadow:0 8px 24px rgba(0,0,0,.4);max-height:320px;overflow-y:auto}
.cs-group-label{display:flex;align-items:center;justify-content:space-between;padding:8px 12px;font-size:13px;font-weight:600;color:var(--muted);cursor:pointer;user-select:none;border-bottom:1px solid var(--line)}
.cs-group-label:hover{color:var(--text);background:#1a2a42}
.cs-group-label.expanded{color:var(--accent)}
.cs-chevron{font-size:10px;transition:transform .15s}
.cs-group-label.expanded .cs-chevron{transform:rotate(180deg)}
.cs-items{padding:4px 0}
.cs-item{padding:7px 12px 7px 24px;font-size:14px;color:var(--text);cursor:pointer;transition:background .1s}
.cs-item:hover{background:#1a2a42}
.cs-item.selected{color:var(--accent);font-weight:500}
</style>