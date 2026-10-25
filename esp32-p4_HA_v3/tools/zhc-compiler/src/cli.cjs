'use strict';

const { packageRoot, devicesDir } = require('./zhc.cjs');

module.exports = {
  pkg: process.argv[2] || packageRoot,
  devicesDir: process.argv[3] || devicesDir,
};