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

// 分组下拉用：每组 {label, children: [{value, label}]}
export const HID_KEY_GROUPS = [
  {label: '特殊', children: [{value: 0, label: '仅修饰键'}]},
  {label: '字母', children: buildRange(4, 29, i => ({value: i, label: String.fromCharCode(65 + i - 4)}))},
  {label: '数字', children: [...buildRange(30, 38, i => ({value: i, label: String(i - 29)})), {value: 39, label: '0'}]},
  {label: '符号', children: [
    {value: 45, label: '- _'}, {value: 46, label: '= +'}, {value: 47, label: '[ {'}, {value: 48, label: '] }'}, {value: 49, label: '\\ |'},
    {value: 51, label: '; :'}, {value: 52, label: '\' "'}, {value: 53, label: '` ~'}, {value: 54, label: ', <'}, {value: 55, label: '. >'}, {value: 56, label: '/ ?'},
  ]},
  {label: '控制键', children: [
    {value: 40, label: 'Enter'}, {value: 41, label: 'Esc'}, {value: 42, label: 'Backspace'}, {value: 43, label: 'Tab'}, {value: 44, label: 'Space'}, {value: 57, label: 'CapsLock'},
  ]},
  {label: '功能键', children: buildRange(58, 69, i => ({value: i, label: `F${i - 57}`}))},
  {label: '编辑键', children: [
    {value: 74, label: 'Home'}, {value: 75, label: 'PageUp'}, {value: 76, label: 'Delete'}, {value: 77, label: 'End'}, {value: 78, label: 'PageDown'},
    {value: 79, label: '右'}, {value: 80, label: '左'}, {value: 81, label: '下'}, {value: 82, label: '上'}, {value: 83, label: 'NumLock'},
  ]},
];

function buildRange(from, to, fn) {
  const arr = [];
  for (let i = from; i <= to; i++) arr.push(fn(i));
  return arr;
}

// 键盘 code → HID 键码映射，用于监听物理键盘输入
const KEYBOARD_TO_HID = new Map([
  ['KeyA', 4], ['KeyB', 5], ['KeyC', 6], ['KeyD', 7], ['KeyE', 8], ['KeyF', 9],
  ['KeyG', 10], ['KeyH', 11], ['KeyI', 12], ['KeyJ', 13], ['KeyK', 14], ['KeyL', 15],
  ['KeyM', 16], ['KeyN', 17], ['KeyO', 18], ['KeyP', 19], ['KeyQ', 20], ['KeyR', 21],
  ['KeyS', 22], ['KeyT', 23], ['KeyU', 24], ['KeyV', 25], ['KeyW', 26], ['KeyX', 27],
  ['KeyY', 28], ['KeyZ', 29],
  ['Digit1', 30], ['Digit2', 31], ['Digit3', 32], ['Digit4', 33], ['Digit5', 34],
  ['Digit6', 35], ['Digit7', 36], ['Digit8', 37], ['Digit9', 38], ['Digit0', 39],
  ['Enter', 40], ['Escape', 41], ['Backspace', 42], ['Tab', 43], ['Space', 44],
  ['Minus', 45], ['Equal', 46], ['BracketLeft', 47], ['BracketRight', 48], ['Backslash', 49],
  ['Semicolon', 51], ['Quote', 52], ['Backquote', 53], ['Comma', 54], ['Period', 55], ['Slash', 56],
  ['CapsLock', 57],
  ['F1', 58], ['F2', 59], ['F3', 60], ['F4', 61], ['F5', 62], ['F6', 63],
  ['F7', 64], ['F8', 65], ['F9', 66], ['F10', 67], ['F11', 68], ['F12', 69],
  ['Home', 74], ['PageUp', 75], ['Delete', 76], ['End', 77], ['PageDown', 78],
  ['ArrowRight', 79], ['ArrowLeft', 80], ['ArrowDown', 81], ['ArrowUp', 82], ['NumLock', 83],
]);

/** 将 KeyboardEvent.code 转为 HID 键码，无法识别返回 -1。 */
export function keyboardCodeToHid(code) {
  return KEYBOARD_TO_HID.get(code) ?? -1;
}

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
  // ── 系统控制 ──
  {value: 48, label: '电源'},
  {value: 49, label: '重置'},
  {value: 50, label: '睡眠'},
  {value: 64, label: '菜单'},
  {value: 65, label: '菜单选择'},
  {value: 66, label: '菜单上'},
  {value: 67, label: '菜单下'},
  {value: 68, label: '菜单左'},
  {value: 69, label: '菜单右'},
  {value: 70, label: '菜单退出'},
  {value: 71, label: '菜单值增加'},
  {value: 72, label: '菜单值减少'},
  {value: 148, label: '退出'},
  {value: 149, label: '帮助'},
  // ── 播放控制 ──
  {value: 176, label: '播放'},
  {value: 177, label: '暂停'},
  {value: 178, label: '录制'},
  {value: 179, label: '快进'},
  {value: 180, label: '快退'},
  {value: 181, label: '下一曲'},
  {value: 182, label: '上一曲'},
  {value: 183, label: '停止'},
  {value: 184, label: '弹出'},
  {value: 185, label: '随机播放'},
  {value: 188, label: '循环播放'},
  {value: 205, label: '播放 / 暂停'},
  {value: 206, label: '播放 / 跳过'},
  // ── 音量控制 ──
  {value: 224, label: '音量'},
  {value: 226, label: '静音'},
  {value: 227, label: '低音'},
  {value: 228, label: '高音'},
  {value: 229, label: '低音增强'},
  {value: 233, label: '音量增加'},
  {value: 234, label: '音量降低'},
  // ── 截图与显示 ──
  {value: 96, label: '屏幕显示'},
  {value: 97, label: '字幕'},
  {value: 101, label: '截图'},
  {value: 102, label: '静止画面'},
  {value: 103, label: '画中画'},
  {value: 109, label: '3D 模式'},
  // ── 频道控制 ──
  {value: 136, label: '频道'},
  {value: 156, label: '频道增加'},
  {value: 157, label: '频道减少'},
  // ── 应用启动 ──
  {value: 384, label: '分配选择'},
  {value: 387, label: '启动浏览器'},
  {value: 388, label: '启动搜索'},
  {value: 402, label: '本地浏览器'},
  {value: 404, label: '远程网络'},
  {value: 407, label: '启动计算器'},
  {value: 409, label: '启动文件管理器'},
  {value: 412, label: '启动浏览器搜索'},
  {value: 414, label: '启动主页'},
  {value: 416, label: '启动浏览器主页'},
  {value: 423, label: '启动邮件'},
  {value: 425, label: '启动日历'},
  {value: 428, label: '启动任务管理器'},
  {value: 430, label: '启动播放器'},
  {value: 431, label: '启动浏览器标签'},
  // ── 浏览器 / 编辑控制 ──
  {value: 512, label: '新建'},
  {value: 513, label: '打开'},
  {value: 515, label: '关闭'},
  {value: 516, label: '最大化'},
  {value: 517, label: '最小化'},
  {value: 518, label: '保存'},
  {value: 519, label: '打印'},
  {value: 520, label: '属性'},
  {value: 538, label: '搜索'},
  {value: 540, label: '主页'},
  {value: 541, label: '后退'},
  {value: 542, label: '前进'},
  {value: 543, label: '停止浏览'},
  {value: 544, label: '刷新'},
  {value: 547, label: '书签'},
  {value: 548, label: '历史'},
  {value: 550, label: '放大'},
  {value: 551, label: '缩小'},
  {value: 556, label: '向上滚动'},
  {value: 557, label: '向下滚动'},
  {value: 562, label: '新建窗口'},
  {value: 566, label: '编辑'},
  {value: 594, label: '文件新建'},
  {value: 595, label: '文件打开'},
  {value: 596, label: '文件关闭'},
  {value: 598, label: '文件保存'},
  {value: 599, label: '文件打印'},
];

const mediaLabels = new Map(MEDIA_CODES.map(media => [media.value, media.label]));

export function mediaLabel(code) {
  return mediaLabels.get(Number(code)) ?? `媒体码 ${code}`;
}

// 分组下拉用
export const MEDIA_CODE_GROUPS = [
  {label: '系统控制', children: [
    {value: 48, label: '电源'}, {value: 49, label: '重置'}, {value: 50, label: '睡眠'},
    {value: 64, label: '菜单'}, {value: 65, label: '菜单选择'},
    {value: 66, label: '菜单上'}, {value: 67, label: '菜单下'},
    {value: 68, label: '菜单左'}, {value: 69, label: '菜单右'},
    {value: 70, label: '菜单退出'}, {value: 71, label: '菜单值增加'}, {value: 72, label: '菜单值减少'},
    {value: 148, label: '退出'}, {value: 149, label: '帮助'},
  ]},
  {label: '播放控制', children: [
    {value: 176, label: '播放'}, {value: 177, label: '暂停'}, {value: 178, label: '录制'},
    {value: 179, label: '快进'}, {value: 180, label: '快退'},
    {value: 181, label: '下一曲'}, {value: 182, label: '上一曲'},
    {value: 183, label: '停止'}, {value: 184, label: '弹出'},
    {value: 185, label: '随机播放'}, {value: 188, label: '循环播放'},
    {value: 205, label: '播放 / 暂停'}, {value: 206, label: '播放 / 跳过'},
  ]},
  {label: '音量控制', children: [
    {value: 224, label: '音量'}, {value: 226, label: '静音'},
    {value: 227, label: '低音'}, {value: 228, label: '高音'}, {value: 229, label: '低音增强'},
    {value: 233, label: '音量增加'}, {value: 234, label: '音量降低'},
  ]},
  {label: '截图与显示', children: [
    {value: 96, label: '屏幕显示'}, {value: 97, label: '字幕'},
    {value: 101, label: '截图'}, {value: 102, label: '静止画面'},
    {value: 103, label: '画中画'}, {value: 109, label: '3D 模式'},
    {value: 136, label: '频道'}, {value: 156, label: '频道增加'}, {value: 157, label: '频道减少'},
  ]},
  {label: '应用启动', children: [
    {value: 384, label: '分配选择'},
    {value: 387, label: '启动浏览器'}, {value: 388, label: '启动搜索'},
    {value: 402, label: '本地浏览器'}, {value: 404, label: '远程网络'},
    {value: 407, label: '启动计算器'}, {value: 409, label: '启动文件管理器'},
    {value: 412, label: '启动浏览器搜索'}, {value: 414, label: '启动主页'},
    {value: 416, label: '启动浏览器主页'}, {value: 423, label: '启动邮件'},
    {value: 425, label: '启动日历'}, {value: 428, label: '启动任务管理器'},
    {value: 430, label: '启动播放器'}, {value: 431, label: '启动浏览器标签'},
  ]},
  {label: '浏览器 / 编辑', children: [
    {value: 512, label: '新建'}, {value: 513, label: '打开'}, {value: 515, label: '关闭'},
    {value: 516, label: '最大化'}, {value: 517, label: '最小化'},
    {value: 518, label: '保存'}, {value: 519, label: '打印'}, {value: 520, label: '属性'},
    {value: 538, label: '搜索'}, {value: 540, label: '主页'},
    {value: 541, label: '后退'}, {value: 542, label: '前进'},
    {value: 543, label: '停止浏览'}, {value: 544, label: '刷新'},
    {value: 547, label: '书签'}, {value: 548, label: '历史'},
    {value: 550, label: '放大'}, {value: 551, label: '缩小'},
    {value: 556, label: '向上滚动'}, {value: 557, label: '向下滚动'},
    {value: 562, label: '新建窗口'}, {value: 566, label: '编辑'},
    {value: 594, label: '文件新建'}, {value: 595, label: '文件打开'},
    {value: 596, label: '文件关闭'}, {value: 598, label: '文件保存'}, {value: 599, label: '文件打印'},
  ]},
];

export const MOUSE_BUTTONS = [
  {value: 0, label: '无'},
  {value: 1, label: '左键'},
  {value: 2, label: '右键'},
  {value: 4, label: '中键'},
];
