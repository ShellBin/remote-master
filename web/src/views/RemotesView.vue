<script setup>
import {computed, reactive, ref, watch} from 'vue';
import {onActivated, onDeactivated, onBeforeUnmount} from 'vue';
import {useAction} from '@/composables/useAction';
import {useCatalogStore} from '@/stores/catalog';
import {useDeviceStore} from '@/stores/device';
import {createKeyHighlighter} from '@/lib/key-highlighter';
import {irSendDisabledReason} from '@/lib/ir';

const catalog = useCatalogStore();
const device = useDeviceStore();
const {busy, run} = useAction();

const remoteForm = reactive({id: '', name: '', transport: 'ir'});
const keyForm = reactive({id: '', name: '', proto: '', code: ''});
const selectedRemoteId = ref('');
const learnedRemote = ref('');
const captureId = ref('');
const editingKeyId = ref(null);
const editingName = ref('');

// 模态框状态
const modalOpen = ref(false);
const modalMode = ref('create'); // 'create' | 'rename' | 'import'

// 导入状态
const importData = ref(null); // 解析后的导入数据
const importName = ref(''); // 用户指定的名称
const importError = ref(''); // 前端校验错误
const importProgress = ref(null); // {done, total, currentKey}

const keysHere = computed(() => catalog.keysOfRemote(selectedRemoteId.value));
const learningHere = computed(() => (
  Boolean(selectedRemoteId.value) && device.status.learningRemote === selectedRemoteId.value
));

// 动画由每张卡片独立持有，不依赖 class 切换或共享定时器。
const keyHighlighter = createKeyHighlighter();
let viewActive = true;

// 切换选中遥控器时：若正在学习则停止，清除动画与表单
async function selectRemote(id) {
  if (learningHere.value) {
    await catalog.stopLearning();
  }
  selectedRemoteId.value = id;
  keyHighlighter.clear();
  resetKeyForm();
}
onBeforeUnmount(() => {
  viewActive = false;
  keyHighlighter.dispose();
});

// 切换 Tab 时停止学习并丢弃未保存红外码
onDeactivated(() => {
  viewActive = false;
  keyHighlighter.clear();
  if (learningHere.value) {
    catalog.stopLearning();
  }
  resetKeyForm();
});

// 重新激活时清理本地表单
onActivated(() => {
  viewActive = true;
  resetKeyForm();
});

function openCreateModal() {
  modalMode.value = 'create';
  Object.assign(remoteForm, {id: '', name: '', transport: 'ir'});
  modalOpen.value = true;
}

function openRenameModal(remote) {
  modalMode.value = 'rename';
  Object.assign(remoteForm, {id: remote.id, name: remote.name});
  modalOpen.value = true;
}

// 打开导入模态框
function openImportModal() {
  modalMode.value = 'import';
  importData.value = null;
  importName.value = '';
  importError.value = '';
  modalOpen.value = true;
}

// 校验导入文件格式
function validateImportFile(data) {
  if (!data || typeof data !== 'object') return '文件内容不是有效的 JSON 对象';
  if (data.format !== 'esp32-remote') return '文件格式不正确（缺少 format 字段）';
  if (!Array.isArray(data.remotes)) return '缺少 remotes 数组';
  if (data.remotes.length !== 1) importError.value = '导入文件必须恰好包含 1 个遥控器';
  if (!Array.isArray(data.keys)) importError.value = '缺少 keys 数组';
  if (!data.captures || typeof data.captures !== 'object') importError.value = '缺少 captures 对象';
  return '';
}

// 处理文件选择
function onImportFileChange(event) {
  importError.value = '';
  importData.value = null;
  importName.value = '';
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
      const err = validateImportFile(data);
      if (err) {
        importError.value = err;
        return;
      }
      importData.value = data;
      importName.value = data.remotes[0].name || '未命名遥控器';
      // 检查是否重名
      const duplicate = catalog.remotes.find(r => r.name === importName.value);
      if (duplicate) {
        importError.value = '已存在同名遥控器，请修改名称';
      }
    } catch (parseErr) {
      importError.value = 'JSON 解析失败：' + parseErr.message;
    }
  };
  reader.onerror = () => { importError.value = '文件读取失败'; };
  reader.readAsText(file);
}

// 提交导入：前端解析后逐键写入（flat 格式：proto/code 在按键顶层）
async function submitImport() {
  if (!importData.value) {
    importError.value = '请先选择文件';
    return;
  }
  const name = importName.value.trim();
  if (!name) {
    importError.value = '请输入遥控器名称';
    return;
  }
  const duplicate = catalog.remotes.find(r => r.name === name);
  if (duplicate) {
    importError.value = '已存在同名遥控器';
    return;
  }

  const data = importData.value;
  const keys = data.keys || [];
  const captures = data.captures || {};
  const total = keys.length;

  importProgress.value = {done: 0, total, currentKey: ''};
  importError.value = '';

  try {
    // 1. 先上传所有采样文件（captures 按 capturePath 索引）
    for (let i = 0; i < keys.length; i++) {
      const keyIn = keys[i];
      const capPath = keyIn.capturePath;
      if (!capPath || !captures[capPath]) continue;
      const captureData = captures[capPath];
      const capContent = typeof captureData === 'string' ? captureData : JSON.stringify(captureData);
      const filename = capPath.split('/').pop();
      const fd = new FormData();
      fd.append('file', new Blob([capContent], {type: 'application/json'}), filename);
      const res = await fetch(`/api/files?path=${encodeURIComponent(capPath)}`, {method: 'POST', body: fd});
      if (!res.ok) throw new Error(`上传采样文件失败：${capPath}`);
      await new Promise(r => setTimeout(r, 30));
    }

    // 2. 创建遥控器（不带 ID，固件自动生成）
    await catalog.save('remotes', {
      name: name,
      transport: data.remotes[0]?.transport || 'ir',
    });
    await device.refresh();
    const newRemote = catalog.remotes.find(r => r.name === name);
    if (!newRemote) throw new Error('创建遥控器失败');
    selectedRemoteId.value = newRemote.id;

    // 3. 逐键写入
    for (let i = 0; i < keys.length; i++) {
      const keyIn = keys[i];
      importProgress.value = {done: i, total, currentKey: keyIn.name || keyIn.id};

      const keyPayload = {
        name: keyIn.name || keyIn.id,
        remoteId: newRemote.id,
      };
      if (keyIn.proto) keyPayload.proto = keyIn.proto;
      if (keyIn.code) keyPayload.code = keyIn.code;
      if (keyIn.capturePath) keyPayload.capturePath = keyIn.capturePath;

      await catalog.save('keys', keyPayload);
      await new Promise(r => setTimeout(r, 50));
    }

    importProgress.value = {done: total, total, currentKey: ''};
    device.setNotice(`遥控器「${name}」导入完成，共 ${total} 个按键`);
    closeModal();
  } catch (err) {
    importError.value = err.message || '导入失败';
    importProgress.value = null;
  }
}

function closeModal() {
  modalOpen.value = false;
  importData.value = null;
  importName.value = '';
  importError.value = '';
  importProgress.value = null;
  const fileInput = document.querySelector('#import-file-input');
  if (fileInput) fileInput.value = '';
}

function resetKeyForm() {
  Object.assign(keyForm, {id: '', name: '', proto: '', code: ''});
  learnedRemote.value = '';
  captureId.value = '';
  editingKeyId.value = null;
  editingName.value = '';
}

// 收到信号：若选中遥控器有同码按键则高亮对应卡片（渐入渐出）；
// 学习模式下还会填充表单并进入"更新"模式。
watch(() => device.lastInput, input => {
  if (!viewActive || !input || !selectedRemoteId.value) return;

  // 仅在当前遥控器范围内查找匹配按键（避免多遥控共用码时混乱）
  const existing = catalog.keys.find(key => (
    key.remoteId === selectedRemoteId.value
      && key.proto === input.proto
      && key.code === input.code
  ));

  // 高亮匹配到的按键卡片（学习与非学习模式均生效）
  if (existing?.id) keyHighlighter.flash(existing.id);

  // 仅学习模式：填充表单并进入"更新"模式
  if (!device.status.learningRemote || device.status.learningRemote !== selectedRemoteId.value) return;

  Object.assign(keyForm, {
    id: existing?.id || '',
    name: existing?.name || '',
    proto: input.proto,
    code: input.code,
  });
  learnedRemote.value = selectedRemoteId.value;
  captureId.value = input.captureId;

  const others = catalog.keys.filter(key => (
    key.proto === input.proto && key.code === input.code && key.remoteId !== selectedRemoteId.value
  ));
  device.setNotice(
    `收到 ${input.proto} 按键，${input.pulseCount} 段原始脉冲，请命名后保存`
      + (others.length ? `；注意该红外码还存在于其他 ${others.length} 个按键` : ''),
  );
}, {flush: 'sync'});

async function submitRemote() {
  await run(async () => {
    const existing = catalog.remotes.find(r => r.id === remoteForm.id);
    await catalog.save('remotes', {
      id: remoteForm.id,
      name: remoteForm.name.trim(),
      transport: existing?.transport || remoteForm.transport || 'ir',
    });
    closeModal();
  }, modalMode.value === 'rename' ? '遥控器已重命名' : '遥控器已保存');
}

async function submitKey() {
  await run(async () => {
    if (learnedRemote.value !== selectedRemoteId.value || !keyForm.proto || !keyForm.code) {
      throw new Error('请先学习当前遥控器的信号');
    }
    const payload = {
      id: keyForm.id,
      name: keyForm.name.trim(),
      proto: keyForm.proto,
      code: keyForm.code,
      remoteId: learnedRemote.value,
    };
    if (captureId.value) payload.captureId = captureId.value;
    await catalog.save('keys', payload);
    resetKeyForm();
  }, '按键已保存');
}

const startLearning = () => run(async () => {
  if (!selectedRemoteId.value) throw new Error('请先选择遥控器');
  await catalog.startLearning(selectedRemoteId.value);
}, '已开始学习，请按实体遥控器按键');

const stopLearning = () => run(async () => {
  await catalog.stopLearning();
  resetKeyForm();
}, '已停止学习');

function editKey(key) {
  selectedRemoteId.value = key.remoteId;
  Object.assign(keyForm, {id: key.id, name: key.name, proto: key.proto, code: key.code});
  learnedRemote.value = key.remoteId;
  captureId.value = '';
}

async function removeRemote(remote) {
  if (!confirm(`确定删除遥控器「${remote.name}」？需要先删除它下面的所有按键。`)) return;
  await run(() => catalog.remove('remotes', remote.id), '遥控器已删除');
  if (selectedRemoteId.value === remote.id) {
    if (learningHere.value) await catalog.stopLearning();
    selectedRemoteId.value = '';
    resetKeyForm();
  }
}

async function removeKey(key) {
  if (!confirm(`确定删除按键「${key.name}」？引用它的映射需要先删除。`)) return;
  await run(() => catalog.remove('keys', key.id), '按键已删除');
}

const sendDisabledReason = key => irSendDisabledReason(
  key, catalog.settings, device.status, device.ready,
);
const sendKey = key => run(() => catalog.sendKey(key.id), `已开始发射「${key.name}」`);

// 开始重命名按键
function startRename(key) {
  editingKeyId.value = key.id;
  editingName.value = key.name;
}

// 提交重命名（失焦时自动保存）
async function commitRename(key) {
  const newName = editingName.value.trim();
  if (!newName || newName === key.name) {
    editingKeyId.value = null;
    return;
  }
  await run(async () => {
    await catalog.save('keys', {
      id: key.id,
      name: newName,
      proto: key.proto,
      code: key.code,
      remoteId: key.remoteId,
      capturePath: key.capturePath,
    });
    editingKeyId.value = null;
  }, '按键名称已更新');
}
</script>

<template>
  <section>
    <!-- 已定义遥控器 -->
    <article>
      <h2>已定义遥控器</h2>
      <div class="actions" style="margin-bottom: 12px;">
        <button @click="openCreateModal">添加遥控器</button>
        <button class="secondary" @click="openImportModal">导入遥控器</button>
      </div>
      <p v-if="!catalog.remotes.length" class="hint">尚无遥控器，请点击"添加遥控器"或"导入遥控器"</p>
      <div class="remote-list" v-else>
        <div
          v-for="remote in catalog.remotes"
          :key="remote.id"
          class="remote-chip"
          :class="{active: remote.id === selectedRemoteId}"
          @click="selectRemote(remote.id)"
        >
          <span class="remote-chip-name">{{ remote.name }}</span>
          <span class="remote-chip-meta">
            {{ remote.transport === 'ir' ? '红外' : '蓝牙' }} · {{ catalog.keysOfRemote(remote.id).length }} 键
          </span>
          <span class="remote-chip-actions">
            <button class="secondary" @click.stop="device.exportData('remote', remote.id, remote.name)">导出</button>
            <button class="secondary" @click.stop="openRenameModal(remote)">重命名</button>
            <button class="danger" @click.stop="removeRemote(remote)">删除</button>
          </span>
        </div>
      </div>
    </article>

    <!-- 遥控器按键学习 -->
    <article>
      <h2>遥控器按键学习</h2>

      <div class="remote-selector">
        <select :value="selectedRemoteId" @change="selectRemote($event.target.value)" class="remote-select">
          <option value="">请选择遥控器</option>
          <option v-for="remote in catalog.remotes" :key="remote.id" :value="remote.id">
            {{ remote.name }}（{{ catalog.keysOfRemote(remote.id).length }} 键）
          </option>
        </select>
      </div>

      <div v-if="selectedRemoteId" class="learning-section">
        <div class="actions">
          <button v-if="!learningHere" :disabled="busy" @click="startLearning">开始学习</button>
          <button v-if="learningHere" class="secondary" :disabled="busy" @click="stopLearning">停止学习</button>
        </div>
        <p v-if="!['push-pull', 'sink'].includes(catalog.settings.irTxMode)" class="hint">
          红外发射已关闭，请在设置页面选择 GPIO4 驱动模式后使用发射按钮。
        </p>

        <form v-if="learningHere" @submit.prevent="submitKey">
          <div class="key-form-row">
            <label>按键名称
              <input v-model="keyForm.name" required maxlength="63" placeholder="例如：音量增加">
            </label>
            <button :disabled="busy">保存按键</button>
          </div>
          <div class="fields">
            <span class="field-text">协议：{{ keyForm.proto || '等待遥控输入' }}</span>
            <span class="field-text">红外码：{{ keyForm.code || '—' }}</span>
          </div>
        </form>

        <div class="key-grid">
          <div
            v-for="key in keysHere"
            :key="key.id"
            :ref="element => keyHighlighter.setElement(key.id, element)"
            class="key-card"
          >
            <div class="key-card-header">
              <span
                v-if="editingKeyId !== key.id"
                class="key-name"
                @click="startRename(key)"
              >{{ key.name }}</span>
              <input
                v-else
                v-model="editingName"
                class="key-name-input"
                maxlength="63"
                @blur="commitRename(key)"
                @keyup.enter="commitRename(key)"
                v-focus
              />
              <span
                v-if="!catalog.mappingOf(key.id)"
                class="badge"
              >未映射</span>
            </div>
            <div class="key-card-body">
              <span class="key-proto">{{ key.proto }}</span>
              <span class="key-code">{{ key.code }}</span>
            </div>
            <div class="key-card-actions">
              <button
                class="secondary"
                :disabled="busy || !!sendDisabledReason(key)"
                :title="sendDisabledReason(key) || '发射已保存的红外信号'"
                @click="sendKey(key)"
              >发射</button>
              <button class="danger" :disabled="busy" @click="removeKey(key)">删除</button>
            </div>
            <p v-if="!key.capturePath" class="key-hint">缺少原始脉冲，请重新学习</p>
            <p v-else-if="catalog.mappingOf(key.id)" class="key-hint ok">已映射</p>
          </div>
        </div>
        <p v-if="!keysHere.length" class="hint">该遥控器尚无按键，开始学习后按实体遥控器按键</p>
      </div>

      <p v-else class="hint">请先选择一个遥控器</p>
    </article>

    <!-- 模态框：添加/重命名/导入遥控器 -->
    <Teleport to="body">
      <div v-if="modalOpen" class="modal-backdrop" @click.self="closeModal">
        <div class="modal">
          <!-- 添加/重命名模式 -->
          <template v-if="modalMode !== 'import'">
            <h3>{{ modalMode === 'rename' ? '重命名遥控器' : '添加遥控器' }}</h3>
            <form @submit.prevent="submitRemote">
              <label>遥控器名称
                <input v-model="remoteForm.name" required maxlength="63" placeholder="例如：客厅遥控器" autofocus>
              </label>
              <label v-if="modalMode === 'create'">输入方式
                <select v-model="remoteForm.transport">
                  <option value="ir">红外遥控器</option>
                  <option disabled>蓝牙遥控器（预留）</option>
                </select>
              </label>
              <div class="modal-actions">
                <button :disabled="busy" type="submit">{{ modalMode === 'rename' ? '保存' : '添加' }}</button>
                <button class="secondary" type="button" @click="closeModal">取消</button>
              </div>
            </form>
          </template>
          <!-- 导入模式 -->
          <template v-else>
            <h3>导入遥控器</h3>
            <!-- 进度显示 -->
            <div v-if="importProgress" class="import-progress">
              <p class="hint">正在导入… {{ importProgress.done }}/{{ importProgress.total }}</p>
              <p v-if="importProgress.currentKey" class="current-key">{{ importProgress.currentKey }}</p>
              <div class="progress-bar"><i :style="{width: (importProgress.total ? (importProgress.done / importProgress.total * 100) : 0) + '%'}"></i></div>
            </div>
            <!-- 配置表单 -->
            <form v-else @submit.prevent="submitImport">
              <label>选择文件
                <input id="import-file-input" type="file" accept=".json" @change="onImportFileChange">
              </label>
              <p v-if="importData" class="hint">
                包含 {{ importData.keys?.length || 0 }} 个按键，{{ Object.keys(importData.captures || {}).length }} 个采样
              </p>
              <label v-if="importData">遥控器名称
                <input v-model="importName" required maxlength="63" placeholder="遥控器名称" :class="{error: importError}">
              </label>
              <p v-if="importError" class="error-text">{{ importError }}</p>
              <div class="modal-actions">
                <button :disabled="busy || !importData || !!importError" type="submit">导入</button>
                <button class="secondary" type="button" @click="closeModal">取消</button>
              </div>
            </form>
          </template>
        </div>
      </div>
    </Teleport>
  </section>
</template>

<style scoped>
/* 遥控器选择器 */
.remote-selector{margin-bottom:16px}
.remote-select{min-width:200px;flex:1}

/* 遥控器列表 */
.remote-list{display:grid;gap:10px;margin-top:12px}
.remote-chip{display:flex;align-items:center;gap:12px;padding:12px 16px;background:#182437;border:1px solid var(--line);border-radius:10px;cursor:pointer;transition:border-color .15s,background .15s;flex-wrap:wrap}
.remote-chip:hover{border-color:var(--accent)}
.remote-chip.active{border-color:var(--accent);background:#1a2a42}
.remote-chip-name{font-weight:500;color:var(--text)}
.remote-chip-meta{font-size:13px;color:var(--muted)}
.remote-chip-actions{display:flex;gap:6px;margin-left:auto}

/* 学习面板：卡片样式 */
.learning-section{margin-top:16px;background:#141f31;border:1px solid var(--line);border-radius:10px;padding:16px;display:flex;flex-direction:column;gap:14px}
.learning-section .actions{flex-wrap:wrap}
.learning-section form{display:flex;flex-direction:column;gap:4px}
.key-form-row{display:flex;gap:10px;align-items:flex-end}
.key-form-row label{flex:1;margin:4px 0}
.key-form-row label input{margin-top:4px}
.key-form-row button{flex:0 0 auto;margin-bottom:6px}

/* 协议/红外码：强制竖直堆叠 */
.fields{display:flex;flex-direction:column;gap:2px;margin:6px 0}
.field-text{display:block;color:var(--muted);font-size:13px;line-height:1.6}

/* 按键紧凑网格 */
.key-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px;margin-top:16px}
.key-card{background:#141f31;border:1px solid var(--line);border-radius:8px;padding:10px 12px;display:flex;flex-direction:column;gap:6px;overflow-wrap:anywhere}
.key-card-header{display:flex;align-items:center;gap:6px;flex-wrap:wrap}
.key-name{font-weight:500;color:var(--text);cursor:pointer;border-bottom:1px dashed transparent;transition:border-color .15s}
.key-name:hover{border-bottom-color:var(--accent)}
.key-name-input{font:inherit;font-size:14px;padding:4px 6px;border:1px solid var(--accent);border-radius:4px;background:#101b2c;color:var(--text);width:100%;box-sizing:border-box}
.key-card-body{display:flex;flex-direction:column;gap:2px;font-size:12px;color:var(--muted)}
.key-proto{font-family:monospace;color:var(--accent)}
.key-code{font-family:monospace;word-break:break-all}
.key-card-actions{display:flex;gap:6px;justify-content:flex-end}
.key-card-actions button{padding:5px 10px;font-size:12px}
.key-hint{margin:4px 0 0;font-size:11px;color:var(--muted)}
.key-hint.ok{color:var(--ok)}

/* 导入进度 */
.import-progress{margin:12px 0;display:flex;flex-direction:column;gap:8px}
.import-progress .current-key{font-size:13px;color:var(--accent);font-weight:500}
.progress-bar{height:6px;border-radius:3px;background:var(--line);overflow:hidden}
.progress-bar i{display:block;height:100%;background:var(--accent);transition:width .2s}

.error-text{color:var(--err);font-size:13px;margin:8px 0}

@media (max-width:600px){
  .key-grid{grid-template-columns:repeat(auto-fill,minmax(120px,1fr))}
}
</style>
