<script setup>
import {computed, reactive, ref, watch} from 'vue';
import {useAction} from '@/composables/useAction';
import {hidKeyLabel, MEDIA_CODES, MEDIA_CODE_GROUPS, MODIFIERS, MOUSE_BUTTONS} from '@/lib/hid';
import {ACTION_TYPES, IR_ACTION_TYPES, TARGETS} from '@/lib/mapping';
import {useCatalogStore} from '@/stores/catalog';
import {useDeviceStore} from '@/stores/device';
import KeyPickerModal from '@/components/KeyPickerModal.vue';
import {t} from '@/i18n';

const catalog = useCatalogStore();
const device = useDeviceStore();
const {busy, run} = useAction();

const form = reactive({
  id: '',
  keyId: '',
  target: 'ble-hid',
  actionType: 0,
  irType: 4,
  hidKey: 4,
  mouseButton: 0,
  mouseX: 0,
  mouseY: 0,
  mouseWheel: 0,
  mediaCode: MEDIA_CODES[0].value,
  macroId: '',
  outKeyId: '',
  irMacroId: '',
});

const modifiers = ref([]);
const creating = ref(false);

function onTargetChange() {
  if (form.target === 'ble-hid') {
    form.actionType = 0;
  } else {
    form.irType = 4;
  }
}

watch(() => device.lastInput, input => {
  if (!input) return;
  const key = catalog.keys.find(k => k.proto === input.proto && k.code === input.code);
  if (key) form.keyId = key.id;
}, {flush: 'sync'});

const picker = ref(null);

function openInputPicker() {
  picker.value = {mode: 'input'};
}
function openHidPicker() {
  picker.value = {mode: 'hid'};
}
function openIrOutPicker() {
  picker.value = {mode: 'irOut'};
}

function onPickerSelect(result) {
  if (!picker.value) return;
  if (picker.value.mode === 'input') {
    form.keyId = result;
  } else if (picker.value.mode === 'hid') {
    form.hidKey = result.key;
    modifiers.value = MODIFIERS.filter(m => result.modifiers & m.value).map(m => m.value);
  } else if (picker.value.mode === 'irOut') {
    form.outKeyId = result;
  }
  picker.value = null;
}

function closePicker() {
  picker.value = null;
}

const inputKeyText = computed(() => {
  if (!form.keyId) return '';
  const key = catalog.keyById(form.keyId);
  if (!key) return t('mapping.missingKey');
  return `${catalog.remoteName(key.remoteId)} / ${key.name}`;
});

const irOutKeyText = computed(() => {
  if (!form.outKeyId) return '';
  const key = catalog.keyById(form.outKeyId);
  if (!key) return t('mapping.missingKey');
  return `${catalog.remoteName(key.remoteId)} / ${key.name}`;
});

function resetForm() {
  Object.assign(form, {
    id: '', keyId: '', target: 'ble-hid', actionType: 0, irType: 4,
    hidKey: 4, mouseButton: 0, mouseX: 0, mouseY: 0, mouseWheel: 0,
    mediaCode: MEDIA_CODES[0].value, macroId: '', outKeyId: '', irMacroId: '',
  });
  modifiers.value = [];
  creating.value = false;
}

async function submit() {
  await run(async () => {
    if (!form.keyId) throw new Error(t('mapping.selectInputKey'));
    const payload = {id: form.id, keyId: form.keyId, target: form.target};

    if (form.target === 'ble-hid') {
      payload.type = form.actionType;
      if (form.actionType === 0) {
        payload.key = form.hidKey;
        payload.modifiers = modifiers.value.reduce((sum, value) => sum | value, 0);
      } else if (form.actionType === 1) {
        payload.mouseButton = form.mouseButton;
        payload.mouseX = form.mouseX;
        payload.mouseY = form.mouseY;
        payload.mouseWheel = form.mouseWheel;
      } else if (form.actionType === 2) {
        payload.mediaCode = form.mediaCode;
      } else {
        if (!form.macroId) throw new Error(t('mapping.selectMacroRequired'));
        payload.macroId = form.macroId;
      }
    } else {
      payload.type = form.irType;
      if (form.irType === 4) {
        if (!form.outKeyId) throw new Error(t('mapping.selectIrToReplay'));
        payload.outKeyId = form.outKeyId;
      } else {
        if (!form.irMacroId) throw new Error(t('mapping.selectIrMacro'));
        payload.macroId = form.irMacroId;
      }
    }

    await catalog.save('mappings', payload);
    form.id = '';
    form.keyId = '';
  }, t('mapping.saved'));
}

function editMapping(mapping) {
  resetForm();
  creating.value = false;
  Object.assign(form, {
    id: mapping.id,
    keyId: mapping.keyId,
    target: mapping.target || 'ble-hid',
  });

  if (form.target === 'ble-hid') {
    form.actionType = mapping.type;
    if (mapping.type === 0) {
      form.hidKey = mapping.key;
      modifiers.value = MODIFIERS.filter(modifier => mapping.modifiers & modifier.value)
        .map(modifier => modifier.value);
    } else if (mapping.type === 1) {
      form.mouseButton = mapping.mouseButton || 0;
      form.mouseX = mapping.mouseX || 0;
      form.mouseY = mapping.mouseY || 0;
      form.mouseWheel = mapping.mouseWheel || 0;
    } else if (mapping.type === 2) {
      form.mediaCode = mapping.mediaCode;
    } else {
      form.macroId = mapping.macroId || '';
    }
  } else {
    form.irType = mapping.type;
    if (mapping.type === 4) {
      form.outKeyId = mapping.outKeyId;
    } else {
      form.irMacroId = mapping.macroId || '';
    }
  }
}

const mappingsOfRemote = remoteId => catalog.mappings.filter(mapping => {
  const key = catalog.keyById(mapping.keyId);
  return key && key.remoteId === remoteId;
});

function unmappedLabel(remoteId) {
  const unmapped = catalog.keysOfRemote(remoteId).filter(key => !catalog.mappingOf(key.id)).length;
  return unmapped ? t('mapping.unmappedKeys', unmapped) : t('mapping.allMapped');
}

const testMapping = mapping => run(() => catalog.test('mappings', mapping.id), t('mapping.commandSent'));

async function removeMapping(mapping) {
  if (!confirm(t('mapping.confirmDelete'))) return;
  await run(() => catalog.remove('mappings', mapping.id), t('mapping.deleted'));
}

const collapsed = ref(new Set());

function toggleCollapse(remoteId) {
  const next = new Set(collapsed.value);
  if (next.has(remoteId)) {
    next.delete(remoteId);
  } else {
    next.add(remoteId);
  }
  collapsed.value = next;
}

function isCollapsed(remoteId) {
  return collapsed.value.has(remoteId);
}
</script>

<template>
  <section>
    <!-- 上栏：编辑区 -->
    <article class="mapping-editor">
      <h2>{{ form.id ? t('mapping.edit') : t('mapping.new') }}</h2>

      <form @submit.prevent="submit">
        <!-- 左右结构：左边输入按键，右边目标动作 -->
        <div class="mapping-top">
          <!-- 左栏：输入按键 -->
          <div class="mapping-top-left">
            <span class="mapping-row-label">{{ t('mapping.inputKey') }}</span>
            <button type="button" class="secondary mapping-input-btn" @click="openInputPicker">
              {{ inputKeyText || t('mapping.inputKeyPlaceholder') }}
            </button>
          </div>

          <!-- 右栏：目标动作 -->
          <div class="mapping-top-right">
            <span class="mapping-row-label">{{ t('mapping.targetAction') }}</span>

            <!-- 第一级：目标类型 -->
            <div class="mapping-target-row">
              <label v-for="t in TARGETS" :key="t.value" class="mapping-target-opt"
                     :class="{active: form.target === t.value}">
                <input v-model="form.target" type="radio" :value="t.value" @change="onTargetChange">{{ t.label }}
              </label>
            </div>

            <!-- 第二级：动作类型 -->
            <select v-if="form.target === 'ble-hid'" v-model.number="form.actionType" class="mapping-action-select">
              <option v-for="a in ACTION_TYPES" :key="a.value" :value="a.value">{{ a.label }}</option>
            </select>
            <select v-else v-model.number="form.irType" class="mapping-action-select">
              <option v-for="a in IR_ACTION_TYPES" :key="a.value" :value="a.value">{{ a.label }}</option>
            </select>

            <!-- 第三级：动作参数 -->
            <template v-if="form.target === 'ble-hid' && form.actionType === 0">
              <div class="param-inline">
                <button type="button" class="secondary mapping-param-btn" @click="openHidPicker">
                  {{ hidKeyLabel(form.hidKey) }}{{ modifiers.length ? ' + ' + modifiers.map(m => MODIFIERS.find(mod => mod.value === m)?.label?.replace(' / Win', '')).join('+') : '' }}
                </button>
                <div class="param-mods">
                  <label v-for="modifier in MODIFIERS" :key="modifier.value" class="check">
                    <input v-model="modifiers" type="checkbox" :value="modifier.value">{{ modifier.label }}
                  </label>
                </div>
              </div>
            </template>
            <template v-else-if="form.target === 'ir-send' && form.irType === 4">
              <button type="button" class="secondary mapping-param-btn" @click="openIrOutPicker">
                {{ irOutKeyText || t('mapping.selectIrKey') }}
              </button>
            </template>
            <template v-else-if="form.target === 'ir-send'">
              <select v-model="form.irMacroId" class="mapping-param-select">
                <option value="">{{ t('mapping.selectMacro') }}</option>
                <option v-for="macro in catalog.irOnlyMacros" :key="macro.id" :value="macro.id">
                  {{ macro.name }}
                </option>
              </select>
            </template>
          </div>
        </div>

        <!-- 鼠标参数 -->
        <div v-if="form.target === 'ble-hid' && form.actionType === 1" class="mapping-sub-row">
          <label>{{ t('mapping.mouseButton') }}
            <select v-model.number="form.mouseButton">
              <option v-for="button in MOUSE_BUTTONS" :key="button.value" :value="button.value">{{ button.label }}</option>
            </select>
          </label>
          <label>X<input v-model.number="form.mouseX" type="number" min="-127" max="127"></label>
          <label>Y<input v-model.number="form.mouseY" type="number" min="-127" max="127"></label>
          <label>{{ t('mapping.wheel') }}<input v-model.number="form.mouseWheel" type="number" min="-127" max="127"></label>
        </div>

        <!-- 媒体参数 -->
        <div v-if="form.target === 'ble-hid' && form.actionType === 2" class="mapping-sub-row">
          <label>{{ t('mapping.mediaAction') }}
            <select v-model.number="form.mediaCode">
              <optgroup v-for="group in MEDIA_CODE_GROUPS" :key="group.label" :label="group.label">
                <option v-for="media in group.children" :key="media.value" :value="media.value">{{ media.label }}</option>
              </optgroup>
            </select>
          </label>
        </div>

        <!-- HID 宏选择 -->
        <div v-if="form.target === 'ble-hid' && form.actionType === 3" class="mapping-sub-row">
          <label>{{ t('mapping.selectMacroShort') }}
            <select v-model="form.macroId" required>
              <option value="">{{ t('mapping.selectMacro') }}</option>
              <option v-for="macro in catalog.macros" :key="macro.id" :value="macro.id">{{ macro.name }}</option>
            </select>
          </label>
        </div>

        <!-- 红外提示 -->
        <p v-if="form.target === 'ir-send'" class="hint">
          {{ t('mapping.irHint') }}
        </p>

        <div class="actions">
          <button :disabled="busy">{{ form.id ? t('mapping.update') : t('mapping.save') }}</button>
          <button type="button" class="secondary" :disabled="busy" @click="resetForm">{{ t('mapping.clear') }}</button>
        </div>
      </form>
    </article>

    <!-- 下栏：已配置映射 -->
    <article>
      <h2>{{ t('mapping.configured') }}</h2>
      <p v-if="!catalog.remotes.length" class="hint">{{ t('mapping.noRemotes') }}</p>
      <div v-for="remote in catalog.remotes" :key="remote.id" class="mapping-group">
        <button
          type="button"
          class="collapse-header"
          :class="{collapsed: isCollapsed(remote.id)}"
          @click="toggleCollapse(remote.id)"
        >
          <span class="collapse-arrow">&#9662;</span>
          <span class="collapse-title">{{ remote.name }}</span>
          <span class="collapse-meta">{{ t('mapping.count', mappingsOfRemote(remote.id).length, unmappedLabel(remote.id)) }}</span>
        </button>
        <div v-show="!isCollapsed(remote.id)" class="collapse-body">
          <p v-if="!mappingsOfRemote(remote.id).length" class="hint">{{ t('mapping.noMappings') }}</p>
          <div v-else class="mapping-grid">
            <div
              v-for="mapping in mappingsOfRemote(remote.id)"
              :key="mapping.id"
              class="mapping-card"
            >
              <div class="mapping-card-header">
                <button type="button" class="mapping-key-name" @click="form.keyId = mapping.keyId">{{ catalog.keyById(mapping.keyId)?.name || t('mapping.missingKey') }}</button>
              </div>
              <div class="mapping-card-body">
                <span class="mapping-desc">{{ catalog.describeMapping(mapping) }}</span>
              </div>
              <div class="mapping-card-actions">
                <button class="secondary" :disabled="busy" @click="editMapping(mapping)">{{ t('mapping.editBtn') }}</button>
                <button class="secondary" :disabled="busy" @click="testMapping(mapping)">{{ t('mapping.testBtn') }}</button>
                <button class="danger" :disabled="busy" @click="removeMapping(mapping)">{{ t('mapping.deleteBtn') }}</button>
              </div>
            </div>
          </div>
        </div>
      </div>
    </article>
  </section>

  <KeyPickerModal
    v-if="picker"
    :mode="picker.mode === 'hid' ? 'hid' : 'ir'"
    :value="picker.mode === 'hid' ? form.hidKey : (picker.mode === 'input' ? form.keyId : form.outKeyId)"
    :modifiers="picker.mode === 'hid' ? modifiers.reduce((sum, v) => sum | v, 0) : 0"
    @select="onPickerSelect"
    @close="closePicker"
  />
</template>

<style scoped>
.mapping-editor form{display:flex;flex-direction:column;gap:10px}

.mapping-top{display:flex;flex-direction:column;gap:16px}
.mapping-top-left{flex:1;min-width:0;display:flex;flex-direction:column;gap:6px}
.mapping-top-right{flex:1;min-width:0;display:flex;flex-direction:column;gap:6px}
.mapping-input-btn{width:100%;text-align:left;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.mapping-row-label{display:block;font-size:12px;color:var(--muted);margin-bottom:2px}

.mapping-target-row{display:flex;gap:0}
.mapping-target-opt{display:flex;align-items:center;gap:4px;padding:6px 14px;font-size:13px;border:1px solid var(--line);cursor:pointer;white-space:nowrap;transition:background .15s,color .15s}
.mapping-target-opt:first-child{border-radius:6px 0 0 6px}
.mapping-target-opt:last-child{border-radius:0 6px 6px 0}
.mapping-target-opt:not(:first-child){margin-left:-1px}
.mapping-target-opt.active{background:var(--primary);color:#fff;border-color:var(--primary);z-index:1}
.mapping-target-opt input{display:none}

.mapping-action-select{width:100%}

.mapping-param-btn{text-align:left;width:100%;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.mapping-param-select{width:100%}
.param-inline{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.param-mods{display:flex;gap:6px;flex-wrap:wrap;font-size:12px;align-items:center}

.mapping-sub-row{display:flex;gap:12px;align-items:flex-end;flex-wrap:wrap}
.mapping-sub-row > label{flex:0 0 auto;white-space:nowrap}

.mapping-group{margin-top:12px;border:1px solid var(--line);border-radius:10px;overflow:hidden;background:#141f31}
.collapse-header{display:flex;align-items:center;gap:10px;width:100%;padding:12px 16px;background:transparent;border:none;color:var(--text);font:inherit;font-size:14px;text-align:left;cursor:pointer;transition:background .15s}
.collapse-header:hover{background:#1a2a42}
.collapse-arrow{font-size:10px;transition:transform .15s;color:var(--muted)}
.collapse-header.collapsed .collapse-arrow{transform:rotate(-90deg)}
.collapse-title{font-weight:500}
.collapse-meta{margin-left:auto;font-size:12px;color:var(--muted)}

.collapse-body{padding:0 16px 16px}

.mapping-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:10px}
.mapping-card{background:#182437;border:1px solid var(--line);border-radius:8px;padding:10px 12px;display:flex;flex-direction:column;gap:8px;overflow-wrap:anywhere}
.mapping-card-header{display:flex;align-items:center;gap:6px}
.mapping-key-name{font-weight:500;color:var(--text);font-size:14px;background:none;border:none;padding:0;cursor:pointer;text-align:left;font:inherit}
.mapping-key-name:hover{color:var(--primary);text-decoration:underline}
.mapping-card-body{flex:1}
.mapping-desc{font-size:12px;color:var(--muted);word-break:break-word}
.mapping-card-actions{display:flex;gap:6px;justify-content:flex-end}
.mapping-card-actions button{padding:5px 10px;font-size:12px}

@media (max-width:600px){
  .mapping-grid{grid-template-columns:repeat(auto-fill,minmax(140px,1fr))}
}
</style>
