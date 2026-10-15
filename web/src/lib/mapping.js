// 映射与动作序列的枚举定义，与固件 remote_store.cpp / bindings.cpp 的取值一一对应。

// 输出目标。ir-send 是预留能力：硬件没有红外发射管，映射可以先保存，
// 固件对 ir 目标的执行与测试会返回明确提示。
export const TARGETS = [
  {value: 'ble-hid', label: '蓝牙 HID（键盘 / 鼠标 / 媒体）'},
  {value: 'ir-send', label: '红外发射', reserved: true},
];

// 蓝牙 HID 目标的动作类型
export const ACTION_TYPES = [
  {value: 0, label: '键盘按键'},
  {value: 1, label: '鼠标'},
  {value: 2, label: '媒体键'},
  {value: 3, label: '宏（动作序列）'},
];

// 红外发射目标的动作类型
export const IR_ACTION_TYPES = [
  {value: 4, label: '重放一个红外按键'},
  {value: 3, label: '红外按键序列（宏）'},
];

// 动作序列的步骤类型
export const STEP_KINDS = [
  {value: 'key', label: '按键'},
  {value: 'text', label: '文本'},
  {value: 'wait', label: '等待'},
  {value: 'ir', label: '红外重放'},
];

// 动作序列约束（与固件 validMacro 一致）
export const MACRO_LIMITS = {
  maxSteps: 12,
  maxTextBytes: 512,
  maxDurationMs: 60000,
};

export const MACRO_HINTS = '最多 12 步、累计 512 个可打印 ASCII 字符、总时长 60 秒。'
  + '“红外重放”步骤需要发射管硬件（预留）；含红外步骤的动作只能用于红外发射映射，'
  + '纯 HID 步骤的动作用于蓝牙映射。文本按美式键盘布局输出。';

/** 判断一个动作序列是否只包含红外重放与等待步骤（可用于红外发射映射）。 */
export function isIrOnlyMacro(macro) {
  return Boolean(macro?.steps?.length)
    && macro.steps.every(step => step.kind === 'ir' || step.kind === 'wait');
}

/** 时间戳形式的导出文件名，例如 esp32-all-2026-10-14.json */
export function exportFilename(scope, remoteName = '') {
  const date = new Date().toISOString().slice(0, 10);
  const name = remoteName ? `${remoteName.replace(/[^\w一-鿿-]/g, '_')}-` : '';
  return scope === 'remote' && remoteName
    ? `${name}${date}.json`
    : `esp32-${scope}-${name}${date}.json`;
}
