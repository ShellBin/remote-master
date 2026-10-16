import assert from 'node:assert/strict';
import {test} from 'node:test';
import {ref, watch} from 'vue';
import {createKeyHighlighter} from './key-highlighter.js';

function createCard() {
  return {
    isConnected: true,
    animations: [],
    animate(frames, options) {
      const animation = {
        frames, options,
        currentTime: 0,
        playState: 'running',
        onfinish: null,
        cancelled: false,
        cancel() { this.cancelled = true; },
        finish() {
          this.playState = 'finished';
          this.onfinish?.();
        },
      };
      this.animations.push(animation);
      return animation;
    },
  };
}

test('repeated input preserves fade-in and renews the peak without accumulating animations', () => {
  const highlighter = createKeyHighlighter();
  const card = createCard();
  highlighter.setElement('a', card);
  highlighter.flash('a');
  const animation = card.animations[0];
  animation.currentTime = 100;
  highlighter.flash('a');
  assert.equal(animation.currentTime, 100);
  for (let i = 0; i < 1000; i++) {
    animation.currentTime = 1200;
    highlighter.flash('a');
    assert.equal(animation.currentTime, 225);
  }
  assert.equal(card.animations.length, 1);
  animation.finish();
  assert.equal(animation.cancelled, true);
  highlighter.flash('a');
  assert.equal(card.animations.length, 2);
});

test('alternating keys finish independently and both can replay', () => {
  const highlighter = createKeyHighlighter();
  const a = createCard();
  const b = createCard();
  highlighter.setElement('a', a);
  highlighter.setElement('b', b);
  highlighter.flash('a');
  highlighter.flash('b');
  a.animations[0].finish();
  assert.equal(b.animations[0].cancelled, false);
  b.animations[0].finish();
  highlighter.flash('a');
  highlighter.flash('b');
  assert.equal(a.animations.length, 2);
  assert.equal(b.animations.length, 2);
});

test('a late finish callback cannot cancel a replacement animation', () => {
  const highlighter = createKeyHighlighter();
  const card = createCard();
  highlighter.setElement('a', card);
  highlighter.flash('a');
  const old = card.animations[0];
  const lateFinish = old.onfinish;
  old.playState = 'finished';
  highlighter.flash('a');
  lateFinish();
  assert.equal(old.cancelled, true);
  assert.equal(card.animations[1].cancelled, false);
});

test('rerender, card removal, selection changes and disposal release the correct resources', () => {
  const highlighter = createKeyHighlighter();
  const card = createCard();
  highlighter.setElement('a', card);
  highlighter.flash('a');
  highlighter.setElement('a', card);
  assert.equal(card.animations[0].cancelled, false);
  highlighter.setElement('a', null);
  assert.equal(card.animations[0].cancelled, true);
  highlighter.flash('a');
  assert.equal(card.animations.length, 1);
  highlighter.setElement('a', card);
  highlighter.flash('a');
  highlighter.clear();
  assert.equal(card.animations[1].cancelled, true);
  highlighter.flash('a');
  highlighter.dispose();
  assert.equal(card.animations[2].cancelled, true);
  highlighter.flash('a');
  assert.equal(card.animations.length, 3);
});

test('detached cards do not animate', () => {
  const highlighter = createKeyHighlighter();
  const card = createCard();
  card.isConnected = false;
  highlighter.setElement('a', card);
  highlighter.flash('a');
  highlighter.flash('missing');
  assert.equal(card.animations.length, 0);
});

test('sync watcher receives identical JSON payloads and same-tick alternating inputs without _seq', () => {
  const lastInput = ref(null);
  const received = [];
  const stop = watch(lastInput, input => received.push(input.code), {flush: 'sync'});
  for (const code of ['a', 'a', 'b', 'a']) {
    lastInput.value = JSON.parse(JSON.stringify({event: 'input', code}));
  }
  assert.deepEqual(received, ['a', 'a', 'b', 'a']);
  stop();
});
