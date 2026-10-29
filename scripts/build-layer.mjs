import { spawnSync } from "node:child_process";

if (process.platform !== "linux") {
  console.log("[zyrax] skipping Vulkan layer build (Linux only)");
  process.exit(0);
}

const result = spawnSync("bash", ["vk-layer/build.sh"], { stdio: "inherit" });
if (result.error) {
  console.error(`[zyrax] could not run vk-layer/build.sh: ${result.error.message}`);
  process.exit(1);
}
process.exit(result.status ?? 1);
