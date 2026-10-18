<script setup>
import {computed, reactive, ref} from 'vue';
import {useAction} from '@/composables/useAction';
import {useCatalogStore} from '@/stores/catalog';
import {useDeviceStore} from '@/stores/device';
import StepEditor from '@/components/StepEditor.vue';
import {t} from '@/i18n';

const catalog = useCatalogStore();
const device = useDeviceStore();
const {busy, run} = useAction();

const selectedId = ref('');
const creating = ref(false);
const form = reactive({id: '', name: ''});
const steps = ref([]);

// ---------- 导入状态 ----------
const importData = ref(null);
const importError = ref('');
const importModalOpen = ref(false);
const missingKeyRemap = ref({});

function selectMacro(macro) {
  if (selectedId.value === macro.id) {
    selectedId.value = '';
    resetForm();
    return;
  }
  creating.value = false;
  selectedId.value = macro.id;
  Object.assign(form, {id: macro.id, name: macro.name});
  steps.value = [...(macro.steps || [])];
}

function resetForm() {
  selectedId.value = '';
  creating.value = false;
  Object.assign(form, {id: '', name: ''});
  steps.value = [];
}

function newMacro() {
  selectedId.value = '';
  creating.value = true;
  Object.assign(form, {id: '', name: ''});
  steps.value = [];
  requestAnimationFrame(() => {
    document.querySelector('.macro-editor input')?.focus();
  });
}

async function submit() {
  await run(async () => {
    if (!steps.value.length) throw new Error(t('macro.needsOneStep'));
    await catalog.save('macros', {id: form.id, name: form.name.trim(), steps: steps.value});
    resetForm();
  }, t('macro.saved_ok'));
}

async function removeMacro(macro) {
  if (!confirm(t('macro.confirmDelete', macro.name))) return;
  await run(() => catalog.remove('macros', macro.id), t('macro.deleted'));
  if (selectedId.value === macro.id) resetForm();
}

const testMacro = macro => run(() => catalog.test('macros', macro.id), t('macro.commandSent'));
const stopHid = () => run(() => catalog.stopHid(), t('macro.abortRequested'));

// ---------- 导入 ----------
function openImportModal() {
  importData.value = null;
  importError.value = '';
  missingKeyRemap.value = {};
  importModalOpen.value = true;
}

function closeImportModal() {
  importModalOpen.value = false;
  importData.value = null;
  importError.value = '';
  missingKeyRemap.value = {};
  const fileInput = document.querySelector('#macro-import-file');
  if (fileInput) fileInput.value = '';
}

function validateImport(data) {
  if (!data || typeof data !== 'object') return t('remote.invalidJson');
  if (data.format !== 'esp32-remote') return t('remote.invalidFormat');
  if (!Array.isArray(data.macros)) return t('macro.missingMacrosArray');
  return '';
}

function onImportFileChange(event) {
  importError.value = '';
  importData.value = null;
  missingKeyRemap.value = {};
  const file = event.target.files[0];
  if (!file) return;
  if (!file.name.endsWith('.json')) {
    importError.value = t('remote.selectJson');
    return;
  }
  const reader = new FileReader();
  reader.onload = e => {
    try {
      const data = JSON.parse(e.target.result);
      const err = validateImport(data);
      if (err) {
        importError.value = err;
        return;
      }
      importData.value = data;
      analyzeMissingKeys(data);
    } catch (parseErr) {
      importError.value = t('remote.invalidJson');
    }
  };
  reader.onerror = () => { importError.value = t('remote.fileRequired'); };
  reader.readAsText(file);
}

function analyzeMissingKeys(data) {
  const remap = {};
  const allKeys = new Set(catalog.keys.map(k => k.id));
  for (const macro of data.macros) {
    if (!Array.isArray(macro.steps)) continue;
    for (const step of macro.steps) {
      if (step.kind === 'ir' && step.keyId && !allKeys.has(step.keyId)) {
        if (!remap[step.keyId]) {
          remap[step.keyId] = {
            name: findOriginalKeyName(data, step.keyId) || step.keyId,
            selected: '',
          };
        }
      }
    }
  }
  missingKeyRemap.value = remap;
}

function findOriginalKeyName(data, keyId) {
  if (!Array.isArray(data.keys)) return null;
  const key = data.keys.find(k => k.id === keyId);
  return key?.name || null;
}

const hasMissingKeys = computed(() => Object.keys(missingKeyRemap.value).length > 0);
const allRemapped = computed(() => (
  hasMissingKeys.value
  && Object.values(missingKeyRemap.value).every(v => v.selected)
));

const remapOptions = computed(() => {
  const opts = [];
  for (const remote of catalog.remotes) {
    const keys = catalog.irKeysOfRemote(remote.id);
    for (const key of keys) {
      opts.push({value: key.id, label: `${remote.name} / ${key.name}`});
    }
  }
  return opts;
});

async function submitImport() {
  if (!importData.value) {
    importError.value = t('remote.fileRequired');
    return;
  }
  if (hasMissingKeys.value && !allRemapped.value) {
    importError.value = t('macro.remapAllKeys');
    return;
  }

  await run(async () => {
    const data = importData.value;
    const keyIdMap = {};
    for (const [oldId, info] of Object.entries(missingKeyRemap.value)) {
      if (info.selected) keyIdMap[oldId] = info.selected;
    }

    let imported = 0;
    for (const macro of data.macros) {
      const newSteps = macro.steps.map(step => {
        if (step.kind === 'ir' && step.keyId && keyIdMap[step.keyId]) {
          return {...step, keyId: keyIdMap[step.keyId]};
        }
        if (step.kind === 'ir' && step.keyId && !catalog.keyById(step.keyId) && !keyIdMap[step.keyId]) {
          return null;
        }
        return step;
      }).filter(Boolean);

      if (!newSteps.length) continue;

      const name = macro.name || 'unnamed';
      const existing = catalog.macros.find(m => m.name === name);
      await catalog.save('macros', {
        ...(existing ? {id: existing.id} : {}),
        name,
        steps: newSteps,
      });
      imported++;
    }

    device.setNotice(t('macro.importDone', imported));
    closeImportModal();
    await device.refresh();
  }, t('remote.importFailed'));
}
</script>

<template>
  <section>
    <!-- 已保存的动作 -->
    <article>
      <h2>{{ t('macro.saved') }}</h2>
      <div class="actions" style="margin-bottom: 12px;">
        <button @click="newMacro">{{ t('macro.new') }}</button>
        <button class="secondary" @click="openImportModal">{{ t('macro.import') }}</button>
      </div>
      <p v-if="!catalog.macros.length" class="hint">{{ t('macro.empty') }}</p>
      <div class="macro-grid" v-else>
        <div
          v-for="macro in catalog.macros"
          :key="macro.id"
          class="macro-card"
          :class="{active: macro.id === selectedId}"
          @click="selectMacro(macro)"
        >
          <div class="macro-card-header">
            <span class="macro-card-name">{{ macro.name }}</span>
          </div>
          <div class="macro-card-body">
            <span class="macro-card-meta">{{ t('macro.steps', macro.steps.length) }}</span>
            <span class="macro-card-hint">{{ catalog.stepSummary(macro.steps[0]) }}{{ macro.steps.length > 1 ? ' …' : '' }}</span>
          </div>
          <div class="macro-card-actions">
            <button class="secondary" @click.stop="device.exportData('macro', macro.id, macro.name)">{{ t('macro.export') }}</button>
            <button class="secondary" @click.stop="testMacro(macro)">{{ t('macro.test') }}</button>
            <button class="danger" @click.stop="removeMacro(macro)">{{ t('macro.delete') }}</button>
          </div>
        </div>
      </div>
      <div class="actions" style="margin-top: 16px;">
        <button class="danger" :disabled="busy" @click="stopHid">{{ t('macro.abort') }}</button>
      </div>
    </article>

    <!-- 编辑动作序列 -->
    <article class="macro-editor">
      <h2>{{ t('macro.edit') }}</h2>

      <form v-if="selectedId || creating" @submit.prevent="submit">
        <label>{{ t('macro.name') }}
          <input v-model="form.name" required maxlength="63" :placeholder="t('macro.namePlaceholder')">
        </label>

        <StepEditor
          :key="selectedId || 'new'"
          v-model="steps"
          @limit="device.setNotice($event, 'error')"
        />

        <div class="actions">
          <button :disabled="busy">{{ selectedId ? t('macro.update') : t('macro.save') }}</button>
          <button type="button" class="secondary" :disabled="busy" @click="newMacro">{{ t('macro.clear') }}</button>
        </div>
      </form>

      <p v-else class="hint">{{ t('macro.selectHint') }}</p>
    </article>

    <!-- 导入模态框 -->
    <Teleport to="body">
      <div v-if="importModalOpen" class="modal-backdrop" @click.self="closeImportModal">
        <div class="modal">
          <h3>{{ t('macro.importTitle') }}</h3>
          <form @submit.prevent="submitImport">
            <label>{{ t('remote.selectFile') }}
              <input id="macro-import-file" type="file" accept=".json" @change="onImportFileChange">
            </label>
            <p v-if="importData" class="hint">
              {{ t('macro.contains', importData.macros?.length || 0) }}
            </p>

            <!-- 缺失红外按键重映射 -->
            <div v-if="hasMissingKeys" class="remap-section">
              <h4>{{ t('macro.missingKeysTitle') }}</h4>
              <div v-for="(info, oldId) in missingKeyRemap" :key="oldId" class="remap-row">
                <span class="remap-original">{{ info.name }}</span>
                <select v-model="info.selected" class="remap-select">
                  <option value="">{{ t('macro.selectPlaceholder') }}</option>
                  <option v-for="opt in remapOptions" :key="opt.value" :value="opt.value">{{ opt.label }}</option>
                </select>
              </div>
              <p v-if="!remapOptions.length" class="error-text">
                {{ t('macro.noReplayableKeys') }}
              </p>
            </div>

            <p v-if="importError" class="error-text">{{ importError }}</p>
            <div class="modal-actions">
              <button
                type="submit"
                :disabled="busy || !importData || !!importError || (hasMissingKeys && (!allRemapped || !remapOptions.length))"
              >{{ t('macro.import') }}</button>
              <button class="secondary" type="button" @click="closeImportModal">{{ t('macro.cancel') }}</button>
            </div>
          </form>
        </div>
      </div>
    </Teleport>
  </section>
</template>

<style scoped>
/* 动作卡片网格 */
.macro-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:10px;margin-top:12px}
.macro-card{background:#182437;border:1px solid var(--line);border-radius:8px;padding:10px 12px;display:flex;flex-direction:column;gap:8px;cursor:pointer;transition:border-color .15s,background .15s;overflow-wrap:anywhere}
.macro-card:hover{border-color:var(--accent)}
.macro-card.active{border-color:var(--accent);background:#1a2a42}
.macro-card-header{display:flex;align-items:center}
.macro-card-name{font-weight:500;color:var(--text);font-size:14px}
.macro-card-body{flex:1;display:flex;flex-direction:column;gap:2px}
.macro-card-meta{font-size:12px;color:var(--accent)}
.macro-card-hint{font-size:12px;color:var(--muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.macro-card-actions{display:flex;gap:6px;justify-content:flex-end}
.macro-card-actions button{padding:5px 10px;font-size:12px}

/* 编辑面板 */
.macro-editor form{display:flex;flex-direction:column;gap:4px}

/* 导入：重映射区域 */
.remap-section{margin-top:12px;padding-top:12px;border-top:1px solid var(--line)}
.remap-section h4{margin:0 0 10px;font-size:13px;color:var(--muted);font-weight:500}
.remap-row{display:flex;align-items:center;gap:8px;margin-bottom:8px}
.remap-original{font-size:13px;color:var(--text);flex:0 0 auto;min-width:80px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.remap-select{flex:1;font-size:13px;padding:6px 8px}

.error-text{color:#ff6b6b;font-size:13px;margin:8px 0 0}

@media (max-width:600px){
  .macro-grid{grid-template-columns:repeat(auto-fill,minmax(140px,1fr))}
}
</style>
