const kDurationMs = 1500;
const kPeakMs = kDurationMs * 0.15;
const kGlow = '0 0 0 2px var(--accent), 0 0 12px 4px color-mix(in srgb, var(--accent) 40%, transparent)';

export function createKeyHighlighter() {
  const elements = new Map();
  const animations = new Map();

  function cancel(keyId) {
    const animation = animations.get(keyId);
    if (!animation) return;
    animation.onfinish = null;
    animation.cancel();
    animations.delete(keyId);
  }

  function setElement(keyId, element) {
    if (elements.get(keyId) === element) return;
    cancel(keyId);
    if (element) elements.set(keyId, element);
    else elements.delete(keyId);
  }

  function flash(keyId) {
    const element = elements.get(keyId);
    if (!element?.isConnected) return;
    const previous = animations.get(keyId);
    if (previous && previous.playState !== 'finished') {
      // Let the initial fade-in complete even under rapid repeats, then hold
      // the peak until input stops. Never keep restarting from invisible.
      previous.currentTime = Math.min(previous.currentTime ?? 0, kPeakMs);
      return;
    }
    cancel(keyId);
    const animation = element.animate([
      {boxShadow: 'none', offset: 0, easing: 'ease-in-out'},
      {boxShadow: kGlow, offset: 0.15},
      {boxShadow: kGlow, offset: 0.85, easing: 'ease-in-out'},
      {boxShadow: 'none', offset: 1},
    ], {duration: kDurationMs});
    animations.set(keyId, animation);
    animation.onfinish = () => {
      // A queued finish event from an older animation must not clear a new one.
      if (animations.get(keyId) === animation) cancel(keyId);
    };
  }

  function clear() {
    for (const keyId of animations.keys()) cancel(keyId);
  }

  function dispose() {
    clear();
    elements.clear();
  }

  return {setElement, flash, clear, dispose};
}
