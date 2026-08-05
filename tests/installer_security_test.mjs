import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {readFileSync, readdirSync} from "node:fs";
import {basename, dirname, resolve} from "node:path";
import {
  FIRMWARE_ORIGIN,
  FIRMWARE_PATH,
  applyVersion,
  isAllowedVersion,
  manifestForVersion,
} from "../static/installer.mjs";

assert.equal(FIRMWARE_ORIGIN, "https://firmware.esphome.io");
assert.equal(
  FIRMWARE_PATH,
  "/True-Family-Voice-Firmware/home-assistant-voice/",
);

for (const version of ["0.19.0", "1.0.0", "2.4.6-beta", "2.4.6-rc.3"]) {
  assert.equal(isAllowedVersion(version), true);
  assert.equal(
    manifestForVersion(version),
    `${FIRMWARE_ORIGIN}${FIRMWARE_PATH}${version}/manifest.json`,
  );
}

for (const version of [
  "",
  "v0.19.0",
  "01.2.3",
  "1.2",
  "1.2.3/../../evil",
  "1.2.3?origin=https://evil.invalid",
  "<img src=x onerror=alert(1)>",
  "1.2.3+metadata",
  "1.2.3-dev",
]) {
  assert.equal(isAllowedVersion(version), false);
  assert.equal(manifestForVersion(version), null);
}

const title = {textContent: "original"};
const installButton = {manifest: "original"};
const versionLink = {textContent: "original", href: "original", hidden: true};
const documentRef = {
  querySelector(selector) {
    return new Map([
      ["h1", title],
      ["esp-web-install-button", installButton],
      ["#version-link", versionLink],
    ]).get(selector);
  },
};

assert.equal(applyVersion(documentRef, "<script>alert(1)</script>"), false);
assert.deepEqual(title, {textContent: "original"});
assert.deepEqual(installButton, {manifest: "original"});
assert.deepEqual(versionLink, {
  textContent: "original",
  href: "original",
  hidden: true,
});

assert.equal(applyVersion(documentRef, "0.19.0"), true);
assert.equal(title.textContent, "Install True Family Voice Realtime v0.19.0");
assert.equal(
  installButton.manifest,
  "https://firmware.esphome.io/True-Family-Voice-Firmware/home-assistant-voice/0.19.0/manifest.json",
);
assert.equal(versionLink.textContent, "Link to v0.19.0");
assert.equal(versionLink.href, "?version=0.19.0");
assert.equal(versionLink.hidden, false);

const vendorDir = process.argv[2];
assert.ok(vendorDir, "checksum-locked installer vendor directory is required");
const expectedVendorFiles = [
  "LICENSE",
  "esp32-D9Bry5AK.js",
  "esp32c2-C0aHw_np.js",
  "esp32c3-1QKN64_Z.js",
  "esp32c6-CgjBrh_Q.js",
  "esp32h2-Bm3EZXXU.js",
  "esp32s2-DxMNCsFV.js",
  "esp32s3-DkYcGTTD.js",
  "esp8266-DEFNY3lv.js",
  "index-BbuTar3J.js",
  "install-button.js",
  "install-dialog-BWZCBYvU.js",
  "rom-B2LvkjpK.js",
  "styles-ChWDJ3ue.js",
];
assert.deepEqual(readdirSync(vendorDir).sort(), expectedVendorFiles.sort());

const installButtonBytes = readFileSync(resolve(vendorDir, "install-button.js"));
assert.equal(
  createHash("sha256").update(installButtonBytes).digest("hex"),
  "9203f43fc432ba2c65041fb83f30180f809f4ca3a6656a14931886d5174c2893",
);

for (const filename of expectedVendorFiles.filter((name) => name.endsWith(".js"))) {
  const source = readFileSync(resolve(vendorDir, filename), "utf8");
  const imports = source.matchAll(/(?:from|import\()\s*["']\.\/([^"'?]+)/g);
  for (const match of imports) {
    const dependency = resolve(dirname(resolve(vendorDir, filename)), match[1]);
    assert.ok(
      readdirSync(dirname(dependency)).includes(basename(dependency)),
      `${filename} has an unresolved local import: ${match[1]}`,
    );
  }
}
