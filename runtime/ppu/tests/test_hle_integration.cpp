/*
 * ps3recomp - NID -> HLE end-to-end integration test (real library).
 *
 * Proves the full bridge with a *real* HLE function (not a mock): the generated
 * NID table (gen_hle_nids.py, computed NIDs) registers cellGcmSys's functions;
 * we compute a function's NID with the same firmware algorithm (nid.h) and
 * dispatch through ps3_hle_call, confirming the real cellGcmSys handler runs
 * and returns via the PPC ABI (r3).
 *
 * Build/run: python tools/test_hle_integration.py
 * Links production ppu_hle.cpp, generated cellGcmSys registrations and C code.
 */
#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include "ps3emu/guest_call.h"
#include <cstdlib>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" {
void     ps3_hle_call(uint32_t nid, ppu_context* ctx);
uint32_t ps3_hle_count(void);
void     ppu_hle_init(void);
int      ps3_hle_has(uint32_t nid);
}

/* Host-only linkage, as in test_sysmodule_unload_module_ex.cpp. There is no
 * PRX, lifted guest, GPU or callback here; tested HLE functions are all real. */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0x10000, g_ww_lo = 0, g_ww_hi = 0;
int g_resv_store_active = 0;
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
thread_local ppu_context* g_active_ctx = nullptr;
void (*g_spu_put_hook)(uint32_t, uint32_t) = nullptr;
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t, uint64_t, uint64_t) { std::abort(); }
void ppu_prof_stamp(void*, unsigned) {}
uint32_t ppu_prof_resolve_host(void*) { return 0; }
void ps3_msf(const char*, ...) {}
void ps3_ms(const char*) {}
void ppu_dump_guest_stack(ppu_context*, const char*) { std::abort(); }
void ppu_dump_bctrl_ring(uint32_t, const char*) { std::abort(); }
void ppu_guest_callstack(const char*) { std::abort(); }
void ppu_guest_caller(char*, size_t) { std::abort(); }
void ppu_resv_break_store(uint64_t) { std::abort(); }
void ps3_ww_report_inline(uint32_t, uint64_t, int) { std::abort(); }
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock() { std::abort(); }
void spu_lockline_unlock() { std::abort(); }
int rsx_live_draw_enabled() { return 0; }
int rsx_draw_engine_enabled() { return 0; }
/* These paths must never be entered in this non-rendering fixture. */
#define UNEXPECTED(name) void name(...) { std::abort(); }
UNEXPECTED(rsx_live_draw_method)
UNEXPECTED(rsx_live_draw_blit)
UNEXPECTED(rsx_live_draw_note_resolve)
UNEXPECTED(rsx_live_draw_resolve_blit)
UNEXPECTED(rsx_live_draw_set_display_buffer)
UNEXPECTED(rsx_draw_engine_method)
UNEXPECTED(rsx_draw_engine_note_flip)
UNEXPECTED(rsx_draw_engine_set_display_buffer)
UNEXPECTED(rsx_draw_engine_guest_read)
UNEXPECTED(rsx_draw_engine_guest_wrote)
UNEXPECTED(rsx_raise_user_cmd)
#undef UNEXPECTED
uint32_t vm_read32(uint64_t a) {
    if (a > ppu_vm_size - 4) std::abort();
    uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v);
}
uint64_t vm_read64(uint64_t a) {
    if (a > ppu_vm_size - 8) std::abort();
    uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v);
}
void vm_write32(uint64_t a, uint32_t v) {
    if (a > ppu_vm_size - 4) std::abort();
    v = __builtin_bswap32(v); memcpy(vm_base + a, &v, 4);
}
void vm_write64(uint64_t a, uint64_t v) {
    if (a > ppu_vm_size - 8) std::abort();
    v = __builtin_bswap64(v); memcpy(vm_base + a, &v, 8);
}
}
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0;

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("[PASS] %s\n", m); else { printf("[FAIL] %s\n", m); fails++; } } while (0)

static uint64_t call(uint32_t nid, uint64_t a0, uint64_t a1 = 0,
                     uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0)
{
    ppu_context ctx{};
    ctx.gpr[1] = 0x8000;
    ctx.gpr[3] = a0; ctx.gpr[4] = a1; ctx.gpr[5] = a2;
    ctx.gpr[6] = a3; ctx.gpr[7] = a4;
    ps3_hle_call(nid, &ctx);
    return ctx.gpr[3];
}

static void expect_prepare(uint32_t buffer, uint64_t wanted, const char* label)
{
    const uint64_t got = call(0x0B4B62D5u, 0x100, buffer);
    printf("prepare buffer %u: guest r3=%llu, expected=%llu\n", buffer,
           (unsigned long long)got, (unsigned long long)wanted);
    CHECK(got == wanted, label);
}

int main()
{
    static uint8_t memory[0x10000]; vm_base = memory;
    ppu_hle_init();                       /* registers cellGcmSys (computed NIDs) */
    uint32_t n = ps3_hle_count();
    printf("registered HLE NIDs: %u\n", n);
    CHECK(n >= 26, "cellGcmSys HLE registered");

    /* cellGcmGetMaxIoMapSize() takes no args and, with no IO mappings yet,
     * returns 256 MiB (0x10000000) -- a deterministic, non-zero value that
     * proves the *real* handler ran (an unresolved NID would return 0). */
    uint32_t nid = ps3_compute_nid("cellGcmGetMaxIoMapSize");
    printf("NID(cellGcmGetMaxIoMapSize) = 0x%08X\n", nid);

    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ps3_hle_call(nid, &ctx);
    printf("ps3_hle_call -> r3 = 0x%016llX\n", (unsigned long long)ctx.gpr[3]);
    CHECK(ctx.gpr[3] == 0x10000000ull,
          "real cellGcmGetMaxIoMapSize ran via NID dispatch (r3 = 256 MiB)");

    /* A second, distinct function to confirm dispatch isn't a fluke. */
    uint32_t nid2 = ps3_compute_nid("cellGcmGetCurrentField");
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 0xDEAD;
    ps3_hle_call(nid2, &ctx);
    CHECK(ctx.gpr[3] == 0, "cellGcmGetCurrentField dispatched (returns 0)");

    CHECK(ps3_compute_nid("cellGcmSetPrepareFlip") == 0x0B4B62D5u &&
          ps3_hle_has(0x0B4B62D5u), "prepare-flip real NID is registered");
    CHECK(ps3_compute_nid("cellGcmSetDisplayBuffer") == 0xA53D12AEu &&
          ps3_hle_has(0xA53D12AEu), "display-buffer real NID is registered");
    /* Preserve native validation, including the highest valid but unset ID. */
    expect_prepare(1, 0x80210703u, "unregistered buffer 1 is rejected");
    expect_prepare(7, 0x80210703u, "unregistered buffer 7 is rejected");
    expect_prepare(8, 0x80210703u, "out-of-range buffer 8 is rejected");
    expect_prepare(UINT32_MAX, 0x80210703u, "maximal buffer ID is rejected");
    for (uint32_t id : {0u, 1u, 7u})
        CHECK(call(0xA53D12AEu, id, 0x1000 + id * 0x1000, 16, 4, 4) == 0,
              "display buffer registered through real dispatch");
    expect_prepare(1, 1, "successful buffer 1 preserves identity in full guest r3");
    expect_prepare(0, 0, "successful buffer 0 remains the zero control");
    expect_prepare(7, 7, "successful buffer 7 preserves highest valid identity");
    expect_prepare(2, 0x80210703u, "other unregistered buffer remains rejected");
    expect_prepare(8, 0x80210703u, "out-of-range ID remains rejected after setup");

    printf("\nResults: %s (%d failure(s))\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
