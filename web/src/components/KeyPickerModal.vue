<script setup>
import {computed, onBeforeUnmount, onMounted, onUnmounted, ref} from 'vue';
import {HID_KEY_GROUPS, MODIFIERS, keyboardCodeToHid} from '@/lib/hid';
import {useCatalogStore} from '@/stores/catalog';

// 按键选择模态框：支持 HID 键码和红外按键两种模式。
// mode='hid' 时显示分组键码 + 修饰键多选 + 按键录入；
// mode='ir' 时显示遥控器 → 按键两级列表。
const props = defineProps({
  mode: {type: String, required: true}, // 'hid' | 'ir'
  value: {type: [Number, String], default: ''},
});
const emit = defineEmits(['select', 'close']);

const catalog = useCatalogStore();

const listening = ref(false);

function onKeyDown(event) {
  event.preventDefault();
  event.stopPropagation();
  const hid = keyboardCodeToHid(event.code);
  if (hid >= 0) {
    emit('select', {key: hid, modifiers: selectedMods.value});
    listening.value = false;
    window.removeEventListener('keydown', onKeyDown, true);
  }
}

function startListen() {
  listening.value = true;
  window.addEventListener('keydown', onKeyDown, true);
}

function stopListen() {
  listening.value = false;
  window.removeEventListener('keydown', onKeyDown, true);
}

onUnmounted(stopListen);

// HID 模式：当前选中的键码
const selectedKey = ref(props.value);
const selectedMods = ref(0);

function selectHidKey(key) {
  selectedKey.value = key;
  emit('select', {key, modifiers: selectedMods.value});
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

onMounted(() => document.addEventListener('keydown', onEsc, true));
onBeforeUnmount(() => document.removeEventListener('keydown', onEsc, true));

function onEsc(event) {
  if (event.key === 'Escape') emit('close');
}
</script>

<template>
  <Teleport to="body">
    <div class="modal-backdrop" @click="onBackdropClick">
      <div class="modal kp-modal">
        <h3>{{ mode === 'hid' ? '选择按键' : '选择红外按键' }}</h3>

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
                  :class="{selected: item.value === value}"
                  @click="selectHidKey(item.value)"
                >{{ item.label }}</div>
              </div>
            </div>
          </div>

          <div class="kp-mods">
            <span class="kp-mods-label">修饰键：</span>
            <label v-for="mod in MODIFIERS" :key="mod.value" class="check">
              <input
                type="checkbox"
                :checked="(selectedMods & mod.value) !== 0"
                @change="toggleMod(mod.value)"
              >{{ mod.label }}
            </label>
          </div>

          <button
            type="button"
            class="secondary kp-listen"
            :class="{active: listening}"
            @click="listening ? stopListen() : startListen()"
          >{{ listening ? '请按键盘…' : '按键录入' }}</button>
        </template>

        <!-- 红外按键模式 -->
        <template v-else>
          <p v-if="!irGroups.length" class="hint">还没有可重放的红外按键，请先在遥控器下学习。</p>
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
          <button class="secondary" type="button" @click="emit('close')">取消</button>
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
.kp-listen{flex:0 0 auto;font-size:13px;padding:6px 14px}
.kp-listen.active{border-color:var(--warn);color:var(--warn);animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
</style>