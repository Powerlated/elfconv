// This pre-js runs inside the Emscripten worker, where createLazyFile's
// synchronous ranged XHR is permitted.
Module["preInit"] = Module["preInit"] || [];
Module["preInit"].push(function () {
  const config = Module["elfconvAssets"];
  if (!config) return;
  const root = new URL(config.root);
  root.pathname = root.pathname.replace(/\/$/, "") + "/";
  for (const path of config.manifest.directories) {
    if (typeof path !== "string" || !path.startsWith("/") || path.split("/").includes("..")) {
      throw new Error("Invalid guest asset directory: " + path);
    }
    if (path !== "/") FS.createPath("/", path.slice(1), true, true);
  }
  for (const file of config.manifest.files) {
    if (typeof file.path !== "string" || !file.path.startsWith("/") ||
        file.path.split("/").includes("..") || !Number.isSafeInteger(file.size) || file.size < 0) {
      throw new Error("Invalid guest asset file: " + JSON.stringify(file));
    }
    const parts = file.path.slice(1).split("/");
    const name = parts.pop();
    const parent = parts.length ? "/" + parts.join("/") : "/";
    const url = new URL(parts.concat(name).map(encodeURIComponent).join("/"), root).href;
    FS.createLazyFile(parent, name, url, true, false);
  }
});
