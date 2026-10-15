<script setup>
import {computed, reactive, ref, watch} from 'vue';
import {useAction} from '@/composables/useAction';
import {useCatalogStore} from '@/stores/catalog';
import {useDeviceStore} from '@/stores/device';
import ItemCard from '@/components/ItemCard.vue';

const catalog = useCatalogStore();
const device = useDeviceStore();
const {busy, run} = useAction();

const remoteForm = reactive({id: '', name: '', transport: 'ir'});
const keyForm = reactive({id: '', name: '', proto: '', code: ''});
const selectedRemoteId = ref('');
const learnedRemote = ref('');
const captureId = ref('');

// 模态框状态
const modalOpen = ref(false);
const modalMode = ref('create'); // 'create' | 'edit'

const keysHere = computed(() => catalog.keysOfRemote(selectedRemoteId.value));
const learningHere = computed(() => (
  Boolean(selectedRemoteId.value) && device.status.learningRemote === selectedRemoteId.value
));

function openCreateModal() {
  modalMode.value = 'create';
  Object.assign(remoteForm, {id: '', name: '', transport: 'ir'});
  modalOpen.value = true;
}

function openEditModal(remote) {
  modalMode.value = 'edit';
  Object.assign(remoteForm, {id: remote.id, name: remote.name, transport: remote.transport || 'ir'});
  modalOpen.value = true;
}

function closeModal() {
  modalOpen.value = false;
}

function resetKeyForm() {
  Object.assign(keyForm, {id: '', name: '', proto: '', code: ''});
  learnedRemote.value = '';
  captureId.value = '';
}

// 收到学习信号：填充按键表单；若同码按键已存在则进入"更新"模式。
watch(() => device.lastInput, input => {
  if (!input || input.remoteId !== selectedRemoteId.value) return;

  const existing = catalog.keys.find(key => (
    key.remoteId === input.remoteId && key.proto === input.proto && key.code === input.code
  ));
  Object.assign(keyForm, {
    id: existing?.id || '',
    name: existing?.name || '',
    proto: input.proto,
    code: input.code,
  });
  learnedRemote.value = input.remoteId;
  captureId.value = input.captureId;

  const others = catalog.keys.filter(key => (
    key.proto === input.proto && key.code === input.code && key.remoteId !== input.remoteId
  ));
  device.setNotice(
    `收到 ${input.proto} 按键，${input.pulseCount} 段原始脉冲，请命名后保存`
      + (others.length ? `；注意该红外码还存在于其他 ${others.length} 个按键` : ''),
  );
});

async function submitRemote() {
  await run(async () => {
    await catalog.save('remotes', {
      id: remoteForm.id,
      name: remoteForm.name.trim(),
      transport: remoteForm.transport,
    });
    closeModal();
  }, '遥控器已保存');
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

const stopLearning = () => run(() => catalog.stopLearning(), '已停止学习');

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
    selectedRemoteId.value = '';
    resetKeyForm();
  }
}

async function removeKey(key) {
  if (!confirm(`确定删除按键「${key.name}」？引用它的映射需要先删除。`)) return;
  await run(() => catalog.remove('keys', key.id), '按键已删除');
}
</script>

<template>
  <section>
    <article>
      <h2>遥控器按键学习</h2>

      <div class="remote-selector">
        <div class="actions">
          <select v-model="selectedRemoteId" class="remote-select">
            <option value="">请选择遥控器</option>
            <option v-for="remote in catalog.remotes" :key="remote.id" :value="remote.id">
              {{ remote.name }}（{{ catalog.keysOfRemote(remote.id).length }} 键）
            </option>
          </select>
          <button class="secondary" @click="openCreateModal">添加遥控器</button>
        </div>
      </div>

      <div v-if="selectedRemoteId" class="learning-section">
        <div class="actions">
          <button :disabled="busy || learningHere" @click="startLearning">开始学习</button>
          <button class="secondary" :disabled="busy" @click="stopLearning">停止学习</button>
        </div>

        <form @submit.prevent="submitKey">
          <label>按键名称
            <input v-model="keyForm.name" required maxlength="63" placeholder="例如：音量增加">
          </label>
          <div class="fields">
            <label>协议<input :value="keyForm.proto" readonly required></label>
            <label>红外码<input :value="keyForm.code" readonly required></label>
          </div>
          <button :disabled="busy">保存按键</button>
        </form>

        <ItemCard
          v-for="key in keysHere"
          :key="key.id"
          :title="key.name"
          :badge="catalog.mappingOf(key.id) ? '' : '未映射'"
          :disabled="busy"
          @edit="editKey(key)"
          @remove="removeKey(key)"
        >
          {{ key.proto }} / {{ key.code }}
          <template v-if="!key.capturePath"> · 缺少原始脉冲，请重新学习</template>
          <template v-else-if="catalog.mappingOf(key.id)"> · 已映射</template>
        </ItemCard>
        <p v-if="!keysHere.length" class="hint">该遥控器尚无按键，开始学习后按实体遥控器按键</p>
      </div>

      <p v-else class="hint">请先选择一个遥控器，或点击"添加遥控器"创建新的遥控器</p>
    </article>

    <!-- 遥控器列表 -->
    <article v-if="catalog.remotes.length">
      <h3>已定义遥控器</h3>
      <div class="remote-list">
        <div
          v-for="remote in catalog.remotes"
          :key="remote.id"
          class="remote-chip"
          :class="{active: remote.id === selectedRemoteId}"
          @click="selectedRemoteId = remote.id; resetKeyForm()"
        >
          <span class="remote-chip-name">{{ remote.name }}</span>
          <span class="remote-chip-meta">
            {{ remote.transport === 'ir' ? '红外' : '蓝牙' }} · {{ catalog.keysOfRemote(remote.id).length }} 键
          </span>
          <span class="remote-chip-actions">
            <button class="secondary" @click.stop="openEditModal(remote)">编辑</button>
            <button class="danger" @click.stop="removeRemote(remote)">删除</button>
          </span>
        </div>
      </div>
    </article>

    <!-- 模态框：添加/编辑遥控器 -->
    <Teleport to="body">
      <div v-if="modalOpen" class="modal-backdrop" @click.self="closeModal">
        <div class="modal">
          <h3>{{ modalMode === 'edit' ? '编辑遥控器' : '添加遥控器' }}</h3>
          <form @submit.prevent="submitRemote">
            <label>遥控器名称
              <input v-model="remoteForm.name" required maxlength="63" placeholder="例如：客厅遥控器" autofocus>
            </label>
            <label>输入方式
              <select v-model="remoteForm.transport">
                <option value="ir">红外遥控器</option>
                <option disabled>蓝牙遥控器（预留）</option>
              </select>
            </label>
            <div class="modal-actions">
              <button :disabled="busy" type="submit">{{ modalMode === 'edit' ? '保存' : '添加' }}</button>
              <button class="secondary" type="button" @click="closeModal">取消</button>
            </div>
          </form>
        </div>
      </div>
    </Teleport>
  </section>
</template>
