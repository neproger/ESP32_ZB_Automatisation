'use strict';

const { packageRoot, devicesDir } = require('./zhc.cjs');

const positional = process.argv.slice(2).filter((arg) => !arg.startsWith('--'));

module.exports = {
  pkg: positional[0] || packageRoot,
  devicesDir: positional[1] || devicesDir,
};