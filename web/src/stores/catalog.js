import {computed} from 'vue';
import {defineStore} from 'pinia';
import {useDeviceStore} from './device';
import {hidKeyLabel, mediaLabel} from '@/lib/hid';
import {isIrOnlyMacro} from '@/lib/mapping';

/**
 * 配置数据层：把 device.config 这棵扁平 JSON 变成视图需要的形状，
 * 并把"写设备"的操作收敛成具名方法，视图不再直接拼 op 字符串。
 *
 * 配置结构（remote-config.json）：
 *   remotes  → 数据源（红外；蓝牙为预留）
 *   keys     → 挂在遥控器下的按键，带完整原始脉冲
 *   mappings → 按键 → 输出目标（ble-hid / ir-send）
 *   macros   → 动作序列（蓝牙 HID 序列 / 红外序列共用一套步骤模型）
 *   hosts    → 已配对的蓝牙主机
 *   settings → 热点、蓝牙名称、脉冲过滤阈值、红外发射模式
 */
export const useCatalogStore = defineStore('catalog', () => {
  const device = useDeviceStore();

  const remotes = computed(() => device.config.remotes || []);
  const keys = computed(() => device.config.keys || []);
  const macros = computed(() => device.config.macros || []);
  const mappings = computed(() => device.config.mappings || []);
  const settings = computed(() => device.config.settings || {});
  const hosts = computed(() => [...(device.config.hosts || [])]);

  // ---------- 查询 ----------
  const remoteById = id => remotes.value.find(remote => remote.id === id);
  const remoteName = id => remoteById(id)?.name || '未知遥控器';
  const keyById = id => keys.value.find(key => key.id === id);
  const hostName = address => (device.config.hosts || []).find(host => host.address === address)?.name || address || '';
  const mappingOf = keyId => mappings.value.find(mapping => mapping.keyId === keyId);

  const keysOfRemote = remoteId => keys.value.filter(key => key.remoteId === remoteId);
  const mappedCountOfRemote = remoteId => keysOfRemote(remoteId).filter(key => mappingOf(key.id)).length;

  /** 带原始脉冲、可用于红外重放的按键（也只有这些能进红外映射）。 */
  const irCapableKeys = computed(() => keys.value.filter(key => key.capturePath));
  const irKeysOfRemote = remoteId => keysOfRemote(remoteId).filter(key => key.capturePath);

  /** 只含红外重放与等待步骤的动作序列，才能用于 ir-send 映射。 */
  const irOnlyMacros = computed(() => macros.value.filter(isIrOnlyMacro));

  // ---------- 文案描述（原实现里的 esc() 由 Vue 模板自动转义替代） ----------
  function describeMapping(mapping) {
    if (!mapping) return '';
    if (mapping.target === 'ir-send') {
      if (mapping.type === 4) {
        const key = keyById(mapping.outKeyId);
        return `红外重放 · ${key ? `${remoteName(key.remoteId)} / ${key.name}` : '按键缺失'}`;
      }
      const macro = macros.value.find(item => item.id === mapping.macroId);
      return `红外序列 · ${macro ? macro.name : '宏缺失'}`;
    }
    if (mapping.type === 0) {
      const modifiers = mapping.modifiers ? ` + 修饰 ${mapping.modifiers}` : '';
      return `键盘 · ${hidKeyLabel(mapping.key)}${modifiers}`;
    }
    if (mapping.type === 1) {
      return `鼠标 · 按钮 ${mapping.mouseButton || 0} / X ${mapping.mouseX || 0}`
        + ` / Y ${mapping.mouseY || 0} / 滚轮 ${mapping.mouseWheel || 0}`;
    }
    if (mapping.type === 2) return `媒体 · ${mediaLabel(mapping.mediaCode)}`;
    const macro = macros.value.find(item => item.id === mapping.macroId);
    return `宏 · ${macro ? macro.name : '宏缺失'}`;
  }

  function stepSummary(step) {
    if (step.kind === 'wait') return `等待 ${step.ms}ms`;
    if (step.kind === 'text') return step.text || '（空文本）';
    if (step.kind === 'ir') {
      const key = keyById(step.keyId);
      return `红外重放 ${key ? `${remoteName(key.remoteId)}/${key.name}` : '缺失按键'}`;
    }
    return `键码 ${step.key} / 修饰 ${step.modifiers}`;
  }

  // ---------- 写操作 ----------
  // 集合名与固件命令前缀一致：remotes / keys / macros / mappings / hosts
  const save = (collection, payload) => device.command(`${collection}.save`, payload);
  const remove = (collection, id) => device.command(`${collection}.delete`, {id});
  const test = (collection, id) => device.command(`${collection}.test`, {id});
  const saveSettings = payload => device.command('settings.save', payload);
  const sendKey = id => device.command('keys.send', {id});

  const startLearning = remoteId => device.command('learn.start', {remoteId});
  const stopLearning = () => device.command('learn.stop');

  const pairHost = () => device.command('hosts.pair');
  const reconnectToHost = hostId => device.command('hosts.reconnect', {id: hostId});
  const stopHid = () => device.command('hid.stop');

  // 恢复初始状态：固件先回包确认，随后在后台格式化整个文件系统——
  // 遥控器、按键、映射、宏、主机、红外采样与前端资源全部清空。
  // 完成后设备回到内置 fallback 页，前端需重新部署。
  const systemReset = () => device.command('system.reset');

  return {
    remotes, keys, macros, mappings, hosts, settings,
    remoteById, remoteName, keyById, hostName, mappingOf,
    keysOfRemote, mappedCountOfRemote, irCapableKeys, irKeysOfRemote, irOnlyMacros,
    describeMapping, stepSummary,
    save, remove, test, saveSettings, sendKey,
    startLearning, stopLearning,
    pairHost, reconnectToHost, stopHid, systemReset,
  };
});
