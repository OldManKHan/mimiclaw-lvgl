# TODO: Script Runtime Support (ESP32-S3 N8R8)

## Current constraints (from this project)

- Flash layout uses two OTA app slots, each `2MB`:
  - `ota_0`: `0x200000`
  - `ota_1`: `0x200000`
  - Source: `partitions.csv`
- Current firmware size:
  - `build/mimiclaw.bin` ~= `1,315,504` bytes
  - Remaining headroom per OTA slot ~= `781,648` bytes (~0.75MB)
- PSRAM is enabled and should be preferred for script heaps:
  - `CONFIG_SPIRAM=y`
  - `CONFIG_SPIRAM_MODE_OCT=y`
  - Source: `sdkconfig.defaults.esp32s3`
- Project has shown DRAM pressure before (`.dram0.bss` overflow), so new runtimes must minimize internal RAM usage.

## Feasibility summary

For on-device script execution under current space and memory constraints:

1. `JerryScript` (JavaScript) - most feasible JS option
- Small footprint class
- Better chance to fit in current OTA headroom
- Suitable for sandboxed "skills", not Node.js ecosystem compatibility

2. `Duktape` (JavaScript) - feasible
- Larger than JerryScript in typical setups
- Still likely workable with careful feature trimming

3. `Lua` (if JS is not mandatory) - best footprint/effort tradeoff
- Mature embedded option
- Usually lower integration risk than JS engines

4. `QuickJS` (JavaScript) - not recommended for current layout
- Higher code/data footprint
- Higher risk of hitting OTA slot or runtime memory limits

## Recommendation

- If JavaScript is required: start with `JerryScript`.
- If language flexibility is acceptable: use `Lua`.
- Most robust path overall: use external script execution over HTTP (ESP32 only dispatches calls).

## Minimum implementation plan (if choosing on-device JS)

1. Add `run_js` tool to current tool registry.
2. Isolate runtime in dedicated task with:
- execution timeout
- memory cap
- output size cap
3. Allocate script heap in PSRAM where possible.
4. Expose only a strict whitelist API (no unrestricted file/network/system access).
5. Add watchdog-safe cancellation path for long-running scripts.

