(function (root) {
  "use strict";

  const SYSTEM_OFFSET = 32 * 256;
  const SYSTEM_LENGTH = 80 * 256;
  const SHA256_PATTERN = /^[0-9a-f]{64}$/;

  const api = {
    SYSTEM_OFFSET,
    SYSTEM_LENGTH,

    async sha256Hex(bytes) {
      const digest = await root.crypto.subtle.digest("SHA-256", bytes);
      return Array.from(new Uint8Array(digest), (value) =>
        value.toString(16).padStart(2, "0")).join("");
    },

    validateManifest(manifest, hddSize, systemSha256) {
      if (!manifest || manifest.schema !== 1) throw new Error("Unsupported CP/M system manifest");
      if (!/^v\d+\.\d+\.\d+$/.test(manifest.version)) throw new Error("Invalid CP/M system version");
      if (manifest.hddSize !== hddSize) throw new Error("CP/M HDD size does not match manifest");
      if (manifest.systemOffset !== SYSTEM_OFFSET || manifest.systemLength !== SYSTEM_LENGTH) {
        throw new Error("CP/M system region does not match boot layout");
      }
      if (!SHA256_PATTERN.test(manifest.hddSha256) ||
          !SHA256_PATTERN.test(manifest.systemSha256)) {
        throw new Error("Invalid CP/M manifest hash");
      }
      if (manifest.systemSha256 !== systemSha256) {
        throw new Error("CP/M system hash does not match manifest");
      }
      if (!Array.isArray(manifest.upgradeFrom) || manifest.upgradeFrom.some((entry) =>
          !entry || !Array.isArray(entry.versions) || entry.versions.length === 0 ||
          entry.versions.some((version) => !/^v\d+\.\d+\.\d+$/.test(version)) ||
          !SHA256_PATTERN.test(entry.systemSha256))) {
        throw new Error("Invalid CP/M upgrade source list");
      }
    },

    findUpgradeSource(manifest, systemSha256) {
      return manifest.upgradeFrom.find((entry) =>
        entry.systemSha256 === systemSha256) || null;
    },
  };

  root.CpmSystem = Object.freeze(api);
  if (typeof module !== "undefined" && module.exports) module.exports = root.CpmSystem;
})(globalThis);
