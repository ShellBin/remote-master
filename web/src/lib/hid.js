// HID 键码表（USB HID Usage ID，Keyboard/Keypad page 0x07）。
// 固件侧 bindings.cpp 校验 key ∈ [0, 115]，这里的取值均在该范围内。

function buildHidKeys() {
  const map = new Map();
  map.set(0, '仅修饰键');

  // 4-29: a-z
  for (let i = 0; i < 26; i++) map.set(4 + i, String.fromCharCode(65 + i));
  // 30-38: 1-9，39: 0
  for (let i = 0; i < 9; i++) map.set(30 + i, String(i + 1));
  map.set(39, '0');

  const misc = {
    40: 'Enter', 41: 'Esc', 42: 'Backspace', 43: 'Tab', 44: 'Space',
    45: '- _', 46: '= +', 47: '[ {', 48: '] }', 49: '\\ |',
    51: '; :', 52: '\' "', 53: '` ~', 54: ', <', 55: '. >', 56: '/ ?',
    57: 'CapsLock',
  };
  for (const [code, label] of Object.entries(misc)) map.set(Number(code), label);

  // 58-69: F1-F12
  for (let i = 0; i < 12; i++) map.set(58 + i, `F${i + 1}`);

  const editing = {
    74: 'Home', 75: 'PageUp', 76: 'Delete', 77: 'End', 78: 'PageDown',
    79: '右', 80: '左', 81: '下', 82: '上', 83: 'NumLock',
  };
  for (const [code, label] of Object.entries(editing)) map.set(Number(code), label);

  return [...map.entries()]
    .sort((a, b) => a[0] - b[0])
    .map(([value, label]) => ({value, label}));
}

export const HID_KEYS = buildHidKeys();

const hidLabels = new Map(HID_KEYS.map(key => [key.value, key.label]));

export function hidKeyLabel(code) {
  return hidLabels.get(Number(code)) ?? `键码 ${code}`;
}

// 修饰键位图（HID 规范：bit0 Ctrl / bit1 Shift / bit2 Alt / bit3 GUI）
export const MODIFIERS = [
  {value: 1, label: 'Ctrl'},
  {value: 2, label: 'Shift'},
  {value: 4, label: 'Alt'},
  {value: 8, label: 'GUI / Win'},
];

// 媒体键（Consumer page 0x0C），固件校验 mediaCode ∈ [1, 1023]
export const MEDIA_CODES = [
  {value: 205, label: '播放 / 暂停'},
  {value: 176, label: '播放'},
  {value: 177, label: '暂停'},
  {value: 183, label: '停止'},
  {value: 181, label: '下一曲'},
  {value: 182, label: '上一曲'},
  {value: 179, label: '快进'},
  {value: 180, label: '快退'},
  {value: 233, label: '音量增加'},
  {value: 234, label: '音量降低'},
  {value: 226, label: '静音'},
  {value: 48, label: '电源'},
  {value: 64, label: '菜单'},
];

const mediaLabels = new Map(MEDIA_CODES.map(media => [media.value, media.label]));

export function mediaLabel(code) {
  return mediaLabels.get(Number(code)) ?? `媒体码 ${code}`;
}

export const MOUSE_BUTTONS = [
  {value: 0, label: '无'},
  {value: 1, label: '左键'},
  {value: 2, label: '右键'},
  {value: 4, label: '中键'},
];
