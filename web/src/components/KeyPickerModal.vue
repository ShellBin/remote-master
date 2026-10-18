<script setup>
import {computed, onBeforeUnmount, onMounted, ref} from 'vue';
import {HID_KEY_GROUPS, MODIFIERS, keyboardCodeToHid} from '@/lib/hid';
import {useCatalogStore} from '@/stores/catalog';
import {t} from '@/i18n';

// 按键选择模态框：支持 HID 键码和红外按键两种模式。
// mode='hid' 时显示分组键码 + 修饰键多选 + 按键录入；
// mode='ir' 时显示遥控器 → 按键两级列表。
const props = defineProps({
  mode: {type: String, required: true}, // 'hid' | 'ir'
  value: {type: [Number, String], default: ''},
  modifiers: {type: Number, default: 0}, // 修饰键位图
});
const emit = defineEmits(['select', 'close']);

const catalog = useCatalogStore();

function onKeyDown(event) {
  const el = event.target;
  // 跳过文本输入框；复选框、按钮等均正常捕获键盘输入
  if (el?.tagName === 'TEXTAREA') return;
  if (el?.tagName === 'INPUT' && el.type !== 'checkbox') return;
  if (event.code === 'Escape') return; // 由 onEsc 处理关闭
  const hid = keyboardCodeToHid(event.code);
  if (hid >= 0) {
    event.preventDefault();
    save(hid);
  }
}

// HID 模式：当前选中的键码
const selectedKey = ref(props.value);
const selectedMods = ref(props.modifiers);

function selectHidKey(key) {
  selectedKey.value = key;
}

function save(key) {
  if (key !== undefined) selectedKey.value = key;
  emit('select', {key: selectedKey.value, modifiers: selectedMods.value});
}

function toggleMod(modValue) {
  selectedMods.value ^= modValue;
}

// IR 模式分组
const irGroups = computed(() => catalog.remotes
  .map(remote => ({
    label: remote.name,
    children: catalog.irKeysOfRemote(remote.id).map(key => ({label: key.name, value: key.id})),
  }))
  .filter(group => group.children.length));

function selectIr(keyId) {
  emit('select', keyId);
}

function onBackdropClick(event) {
  if (event.target === event.currentTarget) emit('close');
}

onMounted(() => {
  document.addEventListener('keydown', onEsc, true);
  if (props.mode === 'hid') window.addEventListener('keydown', onKeyDown);
});
onBeforeUnmount(() => {
  document.removeEventListener('keydown', onEsc, true);
  window.removeEventListener('keydown', onKeyDown);
});

function onEsc(event) {
  if (event.key === 'Escape') emit('close');
}
</script>

<template>
  <Teleport to="body">
    <div class="modal-backdrop" @click="onBackdropClick">
      <div class="modal kp-modal">
        <h3>{{ mode === 'hid' ? t('picker.selectKey') : t('picker.selectIrKey') }}</h3>

        <!-- HID 键码模式 -->
        <template v-if="mode === 'hid'">
          <div class="kp-list">
            <div v-for="group in HID_KEY_GROUPS" :key="group.label" class="kp-group">
              <div class="kp-group-label">{{ group.label }}</div>
              <div class="kp-items">
                <div
                  v-for="item in group.children"
                  :key="item.value"
                  class="kp-item"
                  :class="{selected: item.value === selectedKey}"
                  @click="selectHidKey(item.value)"
                >{{ item.label }}</div>
              </div>
            </div>
          </div>

          <div class="kp-mods">
            <span class="kp-mods-label">{{ t('picker.modifiers') }}</span>
            <label v-for="mod in MODIFIERS" :key="mod.value" class="check">
              <input
                type="checkbox"
                :checked="(selectedMods & mod.value) !== 0"
                @change="toggleMod(mod.value)"
              >{{ mod.label }}
            </label>
          </div>
        </template>

        <!-- 红外按键模式 -->
        <template v-else>
          <p v-if="!irGroups.length" class="hint">{{ t('picker.noIrKeys') }}</p>
          <div v-else class="kp-list">
            <div v-for="group in irGroups" :key="group.label" class="kp-group">
              <div class="kp-group-label">{{ group.label }}</div>
              <div class="kp-items">
                <div
                  v-for="item in group.children"
                  :key="item.value"
                  class="kp-item"
                  :class="{selected: item.value === value}"
                  @click="selectIr(item.value)"
                >{{ item.label }}</div>
              </div>
            </div>
          </div>
        </template>

        <div class="modal-actions">
          <button v-if="mode === 'hid'" type="button" @click="save()">{{ t('picker.save') }}</button>
          <button class="secondary" type="button" @click="emit('close')">{{ t('picker.cancel') }}</button>
        </div>
      </div>
    </div>
  </Teleport>
</template>

<style scoped>
.kp-modal{max-width:520px;width:100%;max-height:80vh;display:flex;flex-direction:column}
.kp-list{flex:1;overflow-y:auto;display:flex;flex-direction:column;gap:4px;margin-bottom:12px}
.kp-group-label{font-size:12px;font-weight:600;color:var(--muted);padding:4px 0}
.kp-items{display:flex;flex-wrap:wrap;gap:4px}
.kp-item{padding:6px 12px;font-size:13px;color:var(--text);background:#101b2c;border:1px solid var(--line);border-radius:6px;cursor:pointer;transition:background .1s,border-color .1s}
.kp-item:hover{border-color:var(--accent);background:#1a2a42}
.kp-item.selected{background:var(--accent);border-color:var(--accent);color:#fff}
.kp-mods{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:12px}
.kp-mods-label{font-size:13px;color:var(--muted)}
</style>
