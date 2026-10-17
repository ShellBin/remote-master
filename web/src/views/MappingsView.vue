<script setup>
import {computed, onUnmounted, reactive, ref} from 'vue';
import {useAction} from '@/composables/useAction';
import {hidKeyLabel, MEDIA_CODES, MODIFIERS, MOUSE_BUTTONS, keyboardCodeToHid} from '@/lib/hid';
import {ACTION_TYPES, IR_ACTION_TYPES, TARGETS} from '@/lib/mapping';
import {useCatalogStore} from '@/stores/catalog';
import ItemCard from '@/components/ItemCard.vue';
import KeyPickerModal from '@/components/KeyPickerModal.vue';

const catalog = useCatalogStore();
const {busy, run} = useAction();

const form = reactive({
  id: '',
  keyId: '',
  target: 'ble-hid',
  actionType: 0,   // target=ble-hid 时的动作类型
  irType: 4,       // target=ir-send 时的动作类型
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
const mapRemoteId = ref('');
const onlyUnmapped = ref(false);
const listening = ref(false);
const pickerOpen = ref(false);

function startListenKey() {
  listening.value = true;
  window.addEventListener('keydown', onKeyDown, true);
}

function stopListenKey() {
  listening.value = false;
  window.removeEventListener('keydown', onKeyDown, true);
}

function onKeyDown(event) {
  event.preventDefault();
  event.stopPropagation();
  const hid = keyboardCodeToHid(event.code);
  if (hid >= 0) form.hidKey = hid;
  stopListenKey();
}

onUnmounted(stopListenKey);

// HID 按键选择模态框
function openKeyPicker() {
  pickerOpen.value = true;
}
function onPickerSelect(result) {
  form.hidKey = result.key;
  modifiers.value = MODIFIERS.filter(m => result.modifiers & m.value).map(m => m.value);
  pickerOpen.value = false;
}
function closePicker() {
  pickerOpen.value = false;
}

const selectableKeys = computed(() => catalog.keysOfRemote(mapRemoteId.value)
  .filter(key => !onlyUnmapped.value || !catalog.mappingOf(key.id)));

const irOutGroups = computed(() => catalog.remotes
  .map(remote => ({
    label: remote.name,
    children: catalog.irKeysOfRemote(remote.id).map(key => ({label: key.name, value: key.id})),
  }))
  .filter(group => group.children.length));

function resetForm() {
  Object.assign(form, {
    id: '', keyId: '', target: 'ble-hid', actionType: 0, irType: 4,
    hidKey: 4, mouseButton: 0, mouseX: 0, mouseY: 0, mouseWheel: 0,
    mediaCode: MEDIA_CODES[0].value, macroId: '', outKeyId: '', irMacroId: '',
  });
  modifiers.value = [];
  onlyUnmapped.value = false;
}

async function submit() {
  await run(async () => {
    if (!form.keyId) throw new Error('请选择输入按键');
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
        if (!form.macroId) throw new Error('请选择宏');
        payload.macroId = form.macroId;
      }
    } else {
      payload.type = form.irType;
      if (form.irType === 4) {
        if (!form.outKeyId) throw new Error('请选择要重放的红外按键');
        payload.outKeyId = form.outKeyId;
      } else {
        if (!form.irMacroId) throw new Error('请选择红外序列宏');
        payload.macroId = form.irMacroId;
      }
    }

    await catalog.save('mappings', payload);
    resetForm();
  }, '映射已保存');
}

function editMapping(mapping) {
  resetForm();
  Object.assign(form, {
    id: mapping.id,
    keyId: mapping.keyId,
    target: mapping.target || 'ble-hid',
  });

  const key = catalog.keyById(mapping.keyId);
  mapRemoteId.value = key?.remoteId || '';

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
  return unmapped ? `${unmapped} 个按键未映射` : '按键全部映射';
}

const testMapping = mapping => run(() => catalog.test('mappings', mapping.id), '已下发执行指令');

async function removeMapping(mapping) {
  if (!confirm('确定删除此映射？')) return;
  await run(() => catalog.remove('mappings', mapping.id), '映射已删除');
}
</script>

<template>
  <section>
    <div class="columns">
      <article>
        <h2>新建映射</h2>
        <form @submit.prevent="submit">
          <h3>输入按键</h3>
          <div class="fields">
            <label>遥控器
              <select v-model="mapRemoteId">
                <option value="">请选择</option>
                <option v-for="remote in catalog.remotes" :key="remote.id" :value="remote.id">
                  {{ remote.name }}
                </option>
              </select>
            </label>
            <label>按键
              <select v-model="form.keyId" required>
                <option value="">请选择</option>
                <option v-for="key in selectableKeys" :key="key.id" :value="key.id">{{ key.name }}</option>
              </select>
            </label>
          </div>
          <label class="check"><input v-model="onlyUnmapped" type="checkbox">只列出未映射的按键</label>

          <h3>输出目标</h3>
          <div class="segment">
            <label v-for="target in TARGETS" :key="target.value">
              <input v-model="form.target" type="radio" name="target" :value="target.value">
              {{ target.label }}
              <em v-if="target.reserved" class="badge">预留</em>
            </label>
          </div>

          <template v-if="form.target === 'ble-hid'">
            <label>动作类型
              <select v-model.number="form.actionType">
                <option v-for="type in ACTION_TYPES" :key="type.value" :value="type.value">{{ type.label }}</option>
              </select>
            </label>

            <template v-if="form.actionType === 0">
              <label>按键
                <button type="button" class="secondary" @click="openKeyPicker" style="width:100%;text-align:left">
                  {{ hidKeyLabel(form.hidKey) }}{{ modifiers.length ? ' + ' + modifiers.map(m => MODIFIERS.find(mod => mod.value === m)?.label?.replace(' / Win', '')).join('+') : '' }}
                </button>
              </label>
              <div class="actions">
                <label v-for="modifier in MODIFIERS" :key="modifier.value" class="check">
                  <input v-model="modifiers" type="checkbox" :value="modifier.value">{{ modifier.label }}
                </label>
              </div>
            </template>

            <template v-else-if="form.actionType === 1">
              <label>鼠标按钮
                <select v-model.number="form.mouseButton">
                  <option v-for="button in MOUSE_BUTTONS" :key="button.value" :value="button.value">
                    {{ button.label }}
                  </option>
                </select>
              </label>
              <div class="fields">
                <label>X<input v-model.number="form.mouseX" type="number" min="-127" max="127"></label>
                <label>Y<input v-model.number="form.mouseY" type="number" min="-127" max="127"></label>
                <label>滚轮<input v-model.number="form.mouseWheel" type="number" min="-127" max="127"></label>
              </div>
            </template>

            <template v-else-if="form.actionType === 2">
              <label>媒体动作
                <select v-model.number="form.mediaCode">
                  <option v-for="media in MEDIA_CODES" :key="media.value" :value="media.value">
                    {{ media.label }}
                  </option>
                </select>
              </label>
            </template>

            <template v-else>
              <label>选择宏
                <select v-model="form.macroId" required>
                  <option value="">请选择</option>
                  <option v-for="macro in catalog.macros" :key="macro.id" :value="macro.id">{{ macro.name }}</option>
                </select>
              </label>
            </template>
          </template>

          <template v-else>
            <label>动作类型
              <select v-model.number="form.irType">
                <option v-for="type in IR_ACTION_TYPES" :key="type.value" :value="type.value">{{ type.label }}</option>
              </select>
            </label>

            <template v-if="form.irType === 4">
              <label>目标红外按键
                <CascadingSelect
                  :modelValue="form.outKeyId"
                  :groups="irOutGroups"
                  placeholder="选择遥控器 → 按键"
                  @update:modelValue="form.outKeyId = $event"
                />
              </label>
            </template>

            <template v-else>
              <label>选择宏（仅含红外重放与等待步骤）
                <select v-model="form.irMacroId" required>
                  <option value="">请选择</option>
                  <option v-for="macro in catalog.irOnlyMacros" :key="macro.id" :value="macro.id">
                    {{ macro.name }}
                  </option>
                </select>
              </label>
            </template>

            <p class="hint">
              请先在设置页面启用 GPIO4 红外发射。重放使用按键保存的原始脉冲，
              发射期间暂停接收；红外序列只支持红外重放与等待步骤。
            </p>
          </template>

          <div class="actions">
            <button :disabled="busy">保存映射</button>
            <button type="button" class="secondary" :disabled="busy" @click="resetForm">新建</button>
          </div>
        </form>
      </article>

      <article>
        <h2>已配置映射</h2>
        <div v-for="remote in catalog.remotes" :key="remote.id" class="group">
          <h3>{{ remote.name }}<small>{{ unmappedLabel(remote.id) }}</small></h3>
          <ItemCard
            v-for="mapping in mappingsOfRemote(remote.id)"
            :key="mapping.id"
            :title="catalog.keyById(mapping.keyId)?.name || '按键缺失'"
            :description="catalog.describeMapping(mapping)"
            testable
            :disabled="busy"
            @edit="editMapping(mapping)"
            @test="testMapping(mapping)"
            @remove="removeMapping(mapping)"
          />
          <p v-if="!mappingsOfRemote(remote.id).length" class="hint">尚无映射</p>
        </div>
        <p v-if="!catalog.remotes.length" class="hint">尚未定义遥控器</p>
      </article>
    </div>
  </section>

  <KeyPickerModal
    v-if="pickerOpen"
    mode="hid"
    :value="form.hidKey"
    :modifiers="modifiers.reduce((sum, v) => sum | v, 0)"
    @select="onPickerSelect"
    @close="closePicker"
  />
</template>

<style scoped>
.listen-btn{font-size:13px;padding:6px 10px}
.listen-btn.active{border-color:var(--warn);color:var(--warn);animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
</style>
