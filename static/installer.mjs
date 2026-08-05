export const FIRMWARE_ORIGIN = "https://firmware.esphome.io";
export const FIRMWARE_PATH =
  "/True-Family-Voice-Firmware/home-assistant-voice/";
export const VERSION_PATTERN =
  /^(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)(?:-(?:alpha|beta|rc)(?:\.(?:0|[1-9]\d*))?)?$/;

export function isAllowedVersion(version) {
  return typeof version === "string" && VERSION_PATTERN.test(version);
}

export function manifestForVersion(version) {
  if (!isAllowedVersion(version)) {
    return null;
  }
  return new URL(
    `${FIRMWARE_PATH}${encodeURIComponent(version)}/manifest.json`,
    FIRMWARE_ORIGIN,
  ).href;
}

export function applyVersion(documentRef, version) {
  const manifest = manifestForVersion(version);
  if (manifest === null) {
    return false;
  }

  const title = documentRef.querySelector("h1");
  const installButton = documentRef.querySelector("esp-web-install-button");
  const versionLink = documentRef.querySelector("#version-link");
  title.textContent = `Install True Family Voice Realtime v${version}`;
  installButton.manifest = manifest;
  versionLink.textContent = `Link to v${version}`;
  versionLink.href = `?version=${encodeURIComponent(version)}`;
  versionLink.hidden = false;
  return true;
}

export function selectVersion(documentRef, version) {
  const select = documentRef.querySelector("#releases");
  const option = Array.from(select.options).find(
    (candidate) => candidate.value === version,
  );
  select.selectedIndex = option ? option.index : 0;
}

export function initializeInstaller(documentRef, locationRef) {
  documentRef.querySelector("#releases").addEventListener("change", (event) => {
    const version = event.target.value;
    if (applyVersion(documentRef, version)) {
      selectVersion(documentRef, version);
    }
  });

  const version = new URLSearchParams(locationRef.search).get("version");
  if (version !== null && applyVersion(documentRef, version)) {
    selectVersion(documentRef, version);
  }
}

if (typeof document !== "undefined" && typeof window !== "undefined") {
  initializeInstaller(document, window.location);
}
