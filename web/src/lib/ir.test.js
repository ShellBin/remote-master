import assert from 'node:assert/strict';
import {test} from 'node:test';
import {irSendDisabledReason} from './ir.js';

const key = {id: 'key-1', capturePath: '/captures/key-1.json'};

test('IR replay is disabled by default and for unknown modes', () => {
  for (const irTxMode of [undefined, 'off', '', 'invalid']) {
    assert.ok(irSendDisabledReason(key, {irTxMode}, {}, true));
  }
});

test('both active drive modes allow replay, including during learning', () => {
  for (const irTxMode of ['push-pull', 'sink']) {
    assert.equal(irSendDisabledReason(key, {irTxMode}, {}, true), '');
    assert.equal(irSendDisabledReason(key, {irTxMode}, {learningRemote: 'remote-1'}, true), '');
  }
});

test('missing capture, disconnected device and busy outputs prevent replay', () => {
  const settings = {irTxMode: 'push-pull'};
  assert.ok(irSendDisabledReason({}, settings, {}, true));
  assert.ok(irSendDisabledReason(key, settings, {}, false));
  assert.ok(irSendDisabledReason(key, settings, {irTxRunning: true}, true));
  assert.ok(irSendDisabledReason(key, settings, {hidRunning: true}, true));
  assert.equal(irSendDisabledReason(key, settings, {irTxRunning: false, hidRunning: false}, true), '');
});
