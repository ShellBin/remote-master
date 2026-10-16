export function irSendDisabledReason(key, settings, status, connected) {
  if (!connected) return '设备未连接';
  if (!key.capturePath) return '缺少原始脉冲，请重新学习';
  if (!['push-pull', 'sink'].includes(settings.irTxMode)) return '请先在设置页面启用红外发射';
  if (status.irTxRunning || status.hidRunning) return '设备正在执行动作，请稍候';
  return '';
}
