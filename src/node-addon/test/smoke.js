#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');

async function main() {
  const addon = require('../index.js');

  assert.equal(typeof addon, 'object', 'Addon should export an object');
  assert.equal(typeof addon.Client, 'function', 'Addon should export Client class');

  const client = new addon.Client();
  const methods = [
    'setEndpoint',
    'fetchBeaconState',
    'fetchCalibration',
    'fetchMinerList',
    'fetchMinerStatus',
    'fetchBlock',
    'fetchUserAccount',
    'fetchTransactionsByWallet',
    'buildTransactionHex',
    'addTransaction'
  ];

  for (const method of methods) {
    assert.equal(typeof client[method], 'function', `Missing method: ${method}`);
  }

  assert.throws(
    () => client.setEndpoint(123),
    /endpoint/i,
    'setEndpoint should validate input type'
  );

  assert.throws(
    () => client.addTransaction('0x1234'),
    /object/i,
    'addTransaction should require object input'
  );

  const transactionHex = client.buildTransactionHex({
    fromWalletId: 1,
    toWalletId: 2,
    amount: 3,
    fee: 1
  });
  assert.equal(typeof transactionHex, 'string', 'buildTransactionHex should return a string');
  assert.match(transactionHex, /^[0-9a-f]+$/i, 'buildTransactionHex should return hex');
  assert.equal(transactionHex.length % 2, 0, 'buildTransactionHex should return even-length hex');

  assert.throws(
    () => client.addTransaction({ transactionHex, signaturesHex: [] }),
    /at least one signature/i,
    'addTransaction should require at least one signature'
  );

  assert.throws(
    () => client.addTransaction({ transactionHex, signaturesHex: ['abcd'] }),
    /128 hex chars/i,
    'addTransaction should validate signature size'
  );

  const pending = client.fetchBeaconState();
  assert.equal(typeof pending.then, 'function', 'fetchBeaconState should return a Promise');

  try {
    await pending;
    console.log('Addon smoke test passed (request unexpectedly succeeded without endpoint).');
  } catch (error) {
    assert.ok(error instanceof Error, 'Promise rejection should be an Error');
    console.log('Addon smoke test passed (request rejected as expected):', error.message);
  }
}

main().catch((error) => {
  console.error('Addon smoke test failed:', error);
  process.exit(1);
});
