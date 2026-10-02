QuickJS 2026-06-04, MIT license (see LICENSE).
Vendored from bellard/quickjs commit 535a7c250ff4a577ec36c3e103daab6dadeea650.
Only the engine and its dependencies are compiled. quickjs-libc and its OS
bindings are excluded. The PSP build disables CONFIG_ATOMICS using
ARKB_NO_ATOMICS; runtimes belong exclusively to the browser worker.
ARK supplies an accounting allocator, memory/stack/time limits and cancellation.
Local portability fixes: optional atomics guard; unsigned opcode-mask shifts; portable fallback for the unused default allocator.
