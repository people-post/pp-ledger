#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');

async function main() {
  const addon = require('../index.js');
  const endpoint = process.env.BEACON_ENDPOINT || 'localhost:8517';

  assert.equal(typeof addon.Client, 'function', 'Addon should export Client class');

  const client = new addon.Client(endpoint);

  const state = await client.fetchBeaconState();
  assert.equal(typeof state, 'object', 'fetchBeaconState should resolve to an object');
  assert.ok(state !== null, 'fetchBeaconState should not resolve to null');

  const calibration = await client.fetchCalibration();
  assert.equal(typeof calibration, 'object', 'fetchCalibration should resolve to an object');
  assert.ok(calibration !== null, 'fetchCalibration should not resolve to null');

  console.log(`Live beacon test passed against ${endpoint}`);
}

main().catch((error) => {
  console.error('Live beacon test failed. Make sure pp-beacon is running.');
  console.error(`Tip: set BEACON_ENDPOINT if needed (current default: localhost:8517)`);
  console.error(error && error.message ? error.message : error);
  process.exit(1);
});
