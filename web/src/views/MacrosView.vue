<script setup>
import {computed, reactive, ref} from 'vue';
import {useAction} from '@/composables/useAction';
import {useCatalogStore} from '@/stores/catalog';
import {useDeviceStore} from '@/stores/device';
import StepEditor from '@/components/StepEditor.vue';

const catalog = useCatalogStore();
const device = useDeviceStore();
const {busy, run} = useAction();

const selectedId = ref('');
const creating = ref(false);
const form = reactive({id: '', name: ''});
const steps = ref([]);

// ---------- 导入状态 ----------
const importData = ref(null); // 解析后的导入数据
const importError = ref('');
const importModalOpen = ref(false);
// 缺失的红外按键映射：{原keyId: {name, options, selected}}
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
    if (!steps.value.length) throw new Error('动作序列至少需要一个步骤');
    await catalog.save('macros', {id: form.id, name: form.name.trim(), steps: steps.value});
    resetForm();
  }, '动作序列已保存');
}

async function removeMacro(macro) {
  if (!confirm(`确定删除动作「${macro.name}」？引用它的映射需要先删除。`)) return;
  await run(() => catalog.remove('macros', macro.id), '动作已删除');
  if (selectedId.value === macro.id) resetForm();
}

const testMacro = macro => run(() => catalog.test('macros', macro.id), '已下发执行指令');
const stopHid = () => run(() => catalog.stopHid(), '已请求中止当前动作');

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
  if (!data || typeof data !== 'object') return '文件内容不是有效的 JSON 对象';
  if (data.format !== 'esp32-remote') return '文件格式不正确（缺少 format 字段）';
  if (!Array.isArray(data.macros)) return '缺少 macros 数组';
  return '';
}

function onImportFileChange(event) {
  importError.value = '';
  importData.value = null;
  missingKeyRemap.value = {};
  const file = event.target.files[0];
  if (!file) return;
  if (!file.name.endsWith('.json')) {
    importError.value = '请选择 JSON 文件';
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
      importError.value = 'JSON 解析失败：' + parseErr.message;
    }
  };
  reader.onerror = () => { importError.value = '文件读取失败'; };
  reader.readAsText(file);
}

// 分析导入的宏中哪些红外 keyId 在当前配置中不存在
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

// 尝试从导入数据的 keys 里找回原始名称
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

// 可用的红外按键选项（用于重新映射）
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
    importError.value = '请先选择文件';
    return;
  }
  if (hasMissingKeys.value && !allRemapped.value) {
    importError.value = '请为所有缺失的红外按键选择替换目标';
    return;
  }

  await run(async () => {
    const data = importData.value;
    // 构建 keyId 映射表
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
        // 若原 keyId 仍然存在，保留；否则且无映射则删除该步骤
        if (step.kind === 'ir' && step.keyId && !catalog.keyById(step.keyId) && !keyIdMap[step.keyId]) {
          return null;
        }
        return step;
      }).filter(Boolean);

      if (!newSteps.length) continue; // 跳过无有效步骤的宏

      await catalog.save('macros', {
        name: macro.name || '未命名动作',
        steps: newSteps,
      });
      imported++;
    }

    device.setNotice(`导入完成，共 ${imported} 个动作序列`);
    closeImportModal();
    await device.refresh();
  }, '导入失败');
}
</script>

<template>
  <section>
    <!-- 已保存的动作 -->
    <article>
      <h2>已保存的动作</h2>
      <div class="actions" style="margin-bottom: 12px;">
        <button @click="newMacro">新建动作</button>
        <button class="secondary" @click="openImportModal">导入动作</button>
      </div>
      <p v-if="!catalog.macros.length" class="hint">尚无动作，请点击"新建动作"开始创建</p>
      <div class="macro-list" v-else>
        <div
          v-for="macro in catalog.macros"
          :key="macro.id"
          class="macro-chip"
          :class="{active: macro.id === selectedId}"
          @click="selectMacro(macro)"
        >
          <span class="macro-chip-name">{{ macro.name }}</span>
          <span class="macro-chip-meta">{{ macro.steps.length }} 步</span>
          <span class="macro-chip-actions">
            <button class="secondary" @click.stop="device.exportData('macro', macro.id, macro.name)">导出</button>
            <button class="secondary" @click.stop="testMacro(macro)">测试</button>
            <button class="danger" @click.stop="removeMacro(macro)">删除</button>
          </span>
        </div>
      </div>
      <div class="actions" style="margin-top: 16px;">
        <button class="danger" :disabled="busy" @click="stopHid">中止当前动作</button>
      </div>
    </article>

    <!-- 编辑动作序列 -->
    <article class="macro-editor">
      <h2>编辑动作序列</h2>

      <form v-if="selectedId || creating" @submit.prevent="submit">
        <label>名称
          <input v-model="form.name" required maxlength="63" placeholder="例如：打开搜索并输入">
        </label>

        <StepEditor
          :key="selectedId || 'new'"
          v-model="steps"
          @limit="device.setNotice($event, 'error')"
        />

        <div class="actions">
          <button :disabled="busy">{{ selectedId ? '更新动作' : '保存动作' }}</button>
          <button type="button" class="secondary" :disabled="busy" @click="newMacro">清空</button>
        </div>
      </form>

      <p v-else class="hint">请从上方选择一个动作进行编辑，或点击"新建动作"开始创建</p>
    </article>

    <!-- 导入模态框 -->
    <Teleport to="body">
      <div v-if="importModalOpen" class="modal-backdrop" @click.self="closeImportModal">
        <div class="modal">
          <h3>导入动作序列</h3>
          <form @submit.prevent="submitImport">
            <label>选择文件
              <input id="macro-import-file" type="file" accept=".json" @change="onImportFileChange">
            </label>
            <p v-if="importData" class="hint">
              包含 {{ importData.macros?.length || 0 }} 个动作序列
            </p>

            <!-- 缺失红外按键重映射 -->
            <div v-if="hasMissingKeys" class="remap-section">
              <h4>以下导入的红外按键在当前配置中不存在，请选择替换目标</h4>
              <div v-for="(info, oldId) in missingKeyRemap" :key="oldId" class="remap-row">
                <span class="remap-original">{{ info.name }}</span>
                <select v-model="info.selected" class="remap-select">
                  <option value="">请选择…</option>
                  <option v-for="opt in remapOptions" :key="opt.value" :value="opt.value">{{ opt.label }}</option>
                </select>
              </div>
              <p v-if="!remapOptions.length" class="error-text">
                当前没有可重放的红外按键，请先学习遥控器后再导入。
              </p>
            </div>

            <p v-if="importError" class="error-text">{{ importError }}</p>
            <div class="modal-actions">
              <button
                type="submit"
                :disabled="busy || !importData || !!importError || (hasMissingKeys && (!allRemapped || !remapOptions.length))"
              >导入</button>
              <button class="secondary" type="button" @click="closeImportModal">取消</button>
            </div>
          </form>
        </div>
      </div>
    </Teleport>
  </section>
</template>

<style scoped>
/* 动作列表 */
.macro-list{display:grid;gap:10px;margin-top:12px}
.macro-chip{display:flex;align-items:center;gap:12px;padding:12px 16px;background:#182437;border:1px solid var(--line);border-radius:10px;cursor:pointer;transition:border-color .15s,background .15s;flex-wrap:wrap}
.macro-chip:hover{border-color:var(--accent)}
.macro-chip.active{border-color:var(--accent);background:#1a2a42}
.macro-chip-name{font-weight:500;color:var(--text)}
.macro-chip-meta{font-size:13px;color:var(--muted)}
.macro-chip-actions{display:flex;gap:6px;margin-left:auto}

/* 编辑面板 */
.macro-editor form{display:flex;flex-direction:column;gap:4px}

/* 导入：重映射区域 */
.remap-section{margin-top:12px;padding-top:12px;border-top:1px solid var(--line)}
.remap-section h4{margin:0 0 10px;font-size:13px;color:var(--muted);font-weight:500}
.remap-row{display:flex;align-items:center;gap:8px;margin-bottom:8px}
.remap-original{font-size:13px;color:var(--text);flex:0 0 auto;min-width:80px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.remap-select{flex:1;font-size:13px;padding:6px 8px}

.error-text{color:#ff6b6b;font-size:13px;margin:8px 0 0}
</style>