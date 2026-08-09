const fs = require('node:fs');
const path = require('node:path');

const candidates = [
  path.join(__dirname, '../build/node-addon/pp_client_node.node'),
  path.join(__dirname, 'build/Release/pp_client_node.node'),
  path.join(__dirname, 'pp_client_node.node')
];

for (const candidate of candidates) {
  if (fs.existsSync(candidate)) {
    module.exports = require(candidate);
    return;
  }
}

throw new Error(`Unable to find pp_client_node addon. Looked in: ${candidates.join(', ')}`);
