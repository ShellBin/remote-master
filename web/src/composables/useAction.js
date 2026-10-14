import {ref} from 'vue';
import {useDeviceStore} from '@/stores/device';

/**
 * 统一的"执行一次设备操作"封装：处理忙碌态、成功提示与失败提示。
 *
 *   const {busy, run} = useAction();
 *   run(() => device.command('learn.start', {remoteId}), '已开始学习');
 *
 * busy 是全局互斥的：设备端单连接串行处理，并发下发命令本身就没有意义，
 * 顺带避免用户连点造成的重复写入。
 */
export function useAction() {
  const device = useDeviceStore();
  const busy = ref(false);

  async function run(work, success = '操作已完成') {
    if (busy.value) return;
    busy.value = true;
    try {
      await work();
      device.setNotice(success);
    } catch (error) {
      device.setNotice(error?.message || String(error), 'error');
    } finally {
      busy.value = false;
    }
  }

  return {busy, run};
}
