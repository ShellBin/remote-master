<script setup>
import {reactive, ref} from 'vue';
import {useAction} from '@/composables/useAction';
import {MACRO_HINTS} from '@/lib/mapping';
import {useCatalogStore} from '@/stores/catalog';
import {useDeviceStore} from '@/stores/device';
import ItemCard from '@/components/ItemCard.vue';
import StepEditor from '@/components/StepEditor.vue';

const catalog = useCatalogStore();
const device = useDeviceStore();
const {busy, run} = useAction();

const form = reactive({id: '', name: ''});
const steps = ref([]);

function resetForm() {
  Object.assign(form, {id: '', name: ''});
  steps.value = [];
}

function editMacro(macro) {
  Object.assign(form, {id: macro.id, name: macro.name});
  steps.value = structuredClone(macro.steps || []);
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
}

const testMacro = macro => run(() => catalog.test('macros', macro.id), '已下发执行指令');
const stopHid = () => run(() => catalog.stopHid(), '已请求中止当前动作');
</script>

<template>
  <section>
    <div class="columns">
      <article>
        <h2>动作序列</h2>
        <form @submit.prevent="submit">
          <label>名称
            <input v-model="form.name" required maxlength="63" placeholder="例如：打开搜索并输入">
          </label>

          <StepEditor
            v-model="steps"
            @limit="device.setNotice($event, 'error')"
          />

          <div class="actions">
            <button :disabled="busy">保存动作</button>
            <button type="button" class="secondary" :disabled="busy" @click="resetForm">新建</button>
          </div>
          <p class="hint">{{ MACRO_HINTS }}</p>
        </form>
      </article>

      <article>
        <h2>已保存的动作</h2>
        <ItemCard
          v-for="macro in catalog.macros"
          :key="macro.id"
          :title="macro.name"
          :description="macro.steps.map(catalog.stepSummary).join(' → ')"
          testable
          :disabled="busy"
          @edit="editMacro(macro)"
          @test="testMacro(macro)"
          @remove="removeMacro(macro)"
        />
        <p v-if="!catalog.macros.length" class="hint">尚未保存动作</p>

        <div class="actions">
          <button class="danger" :disabled="busy" @click="stopHid">中止当前动作</button>
        </div>
      </article>
    </div>
  </section>
</template>
