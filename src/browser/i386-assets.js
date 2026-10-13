// This pre-js mounts remote game files before Emscripten initializes the filesystem.
Module["preInit"] = Module["preInit"] || [];
Module["preInit"].push(function () {
  // Match __ecv_i386_cpuid and __ecv_i386_read_tsc: one virtual CPU,
  // invariant nanosecond counter (1 GHz), never the host CPU's frequency.
  FS.createPath("/", "proc", true, true);
  FS.createDataFile("/proc", "cpuinfo",
      "processor\t: 0\nvendor_id\t: GenuineIntel\n" +
      "model name\t: elfconv virtual i386 CPU, 1 GHz TSC\n" +
      "cpu MHz\t\t: 1000.000\nphysical id\t: 0\ncore id\t\t: 0\n",
      true, false);
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
