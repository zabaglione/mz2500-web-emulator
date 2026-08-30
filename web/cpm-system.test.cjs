"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const CpmSystem = require("./cpm-system.js");
const manifest = JSON.parse(fs.readFileSync(path.join(__dirname, "cpm-system.json")));

async function main() {
  CpmSystem.validateManifest(manifest, manifest.hddSize, manifest.systemSha256);
  assert.equal(CpmSystem.SYSTEM_OFFSET, 8192);
  assert.equal(CpmSystem.SYSTEM_LENGTH, 20480);
  assert.deepEqual(
    CpmSystem.findUpgradeSource(manifest,
      "7d1f15fd46da32e9114d0b5126729c2e741d0601931023d2fdaa885947ea777b").versions,
    ["v1.3.1"],
  );
  assert.equal(CpmSystem.findUpgradeSource(manifest, manifest.systemSha256), null);
  assert.equal(CpmSystem.findUpgradeSource(manifest, "0".repeat(64)), null);
  assert.throws(() => CpmSystem.validateManifest(manifest, manifest.hddSize + 1,
    manifest.systemSha256));

  const invalidUpgradeSources = structuredClone(manifest);
  invalidUpgradeSources.upgradeFrom = null;
  assert.throws(() => CpmSystem.validateManifest(invalidUpgradeSources,
    manifest.hddSize, manifest.systemSha256));

  const hash = await CpmSystem.sha256Hex(new Uint8Array());
  assert.equal(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  console.log("CP/M system update tests passed");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
