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
// 学习结果的归属遥控器。与 selectedRemoteId 分开记录，避免用户中途切换遥控器时
// 把 A 遥控器的信号存到 B 名下。
const learnedRemote = ref('');
const captureId = ref('');

const keysHere = computed(() => catalog.keysOfRemote(selectedRemoteId.value));
const learningHere = computed(() => (
  Boolean(selectedRemoteId.value) && device.status.learningRemote === selectedRemoteId.value
));

function resetRemoteForm() {
  Object.assign(remoteForm, {id: '', name: '', transport: 'ir'});
}

function resetKeyForm() {
  Object.assign(keyForm, {id: '', name: '', proto: '', code: ''});
  learnedRemote.value = '';
  captureId.value = '';
}

// 收到学习信号：填充按键表单；若同码按键已存在则进入"更新"模式。
// 共码普遍存在，这里只提示而不阻止——重复学习会刷新它们共用的原始脉冲。
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
    resetRemoteForm();
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

function editRemote(remote) {
  Object.assign(remoteForm, {id: remote.id, name: remote.name, transport: remote.transport || 'ir'});
}

function editKey(key) {
  selectedRemoteId.value = key.remoteId;
  Object.assign(keyForm, {id: key.id, name: key.name, proto: key.proto, code: key.code});
  learnedRemote.value = key.remoteId;
  captureId.value = '';
}

async function removeRemote(remote) {
  if (!confirm(`确定删除遥控器「${remote.name}」？需要先删除它下面的所有按键。`)) return;
  await run(() => catalog.remove('remotes', remote.id), '遥控器已删除');
}

async function removeKey(key) {
  if (!confirm(`确定删除按键「${key.name}」？引用它的映射需要先删除。`)) return;
  await run(() => catalog.remove('keys', key.id), '按键已删除');
}
</script>

<template>
  <section>
    <div class="columns">
      <article>
        <h2>遥控器</h2>
        <form @submit.prevent="submitRemote">
          <label>遥控器名称
            <input v-model="remoteForm.name" required maxlength="63" placeholder="例如：客厅遥控器">
          </label>
          <label>输入方式
            <select v-model="remoteForm.transport">
              <option value="ir">红外遥控器</option>
              <option disabled>蓝牙遥控器（预留）</option>
            </select>
          </label>
          <div class="actions">
            <button :disabled="busy">保存遥控器</button>
            <button type="button" class="secondary" :disabled="busy" @click="resetRemoteForm">新建</button>
          </div>
        </form>
        <p class="hint">
          不同遥控器允许学习出相同的红外码（多遥控器共码很常见）。执行时按列表顺序匹配第一个带映射的按键；
          重新学习共码按键会同步更新它们共用的原始脉冲。
        </p>

        <ItemCard
          v-for="remote in catalog.remotes"
          :key="remote.id"
          :title="remote.name"
          :disabled="busy"
          @edit="editRemote(remote)"
          @remove="removeRemote(remote)"
        >
          {{ remote.transport === 'ir' ? '红外' : '蓝牙' }}
          · {{ catalog.keysOfRemote(remote.id).length }} 个按键
          · {{ catalog.mappedCountOfRemote(remote.id) }} 个已映射
        </ItemCard>
        <p v-if="!catalog.remotes.length" class="hint">尚未定义遥控器</p>
      </article>

      <article>
        <h2>学习按键</h2>
        <label>当前遥控器
          <select v-model="selectedRemoteId" @change="resetKeyForm">
            <option value="">请选择</option>
            <option v-for="remote in catalog.remotes" :key="remote.id" :value="remote.id">{{ remote.name }}</option>
          </select>
        </label>
        <div class="actions">
          <button :disabled="busy || !selectedRemoteId || learningHere" @click="startLearning">开始学习</button>
          <button class="secondary" :disabled="busy" @click="stopLearning">停止学习</button>
        </div>
        <p class="hint">
          选择遥控器后按实体按键；收到信号后命名保存，可连续学习多个按键。完整原始脉冲会随按键保存，
          供红外重放使用；接收器不能测量载波频率。空闲 5 分钟自动停止。
        </p>

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
      </article>
    </div>
  </section>
</template>
