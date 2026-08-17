import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    coverage: { enabled: false },
    exclude: ["dist/**", "node_modules/**"],
    testTimeout: 10_000,
  },
});
