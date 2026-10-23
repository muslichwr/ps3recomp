/* TASK-013: original synthetic policy-service regression. No game inputs.
 * Reuses the upstream CPU-only fixture's context and fail-fast link boundaries.
 * Compile with the same runtime source list/flags as TASK-005, adding the SDK
 * runtime/spu/tests include directory. Keep executable, diagnostics, and runtime
 * working directory under F:/Games/MLB16PS3/tmp/task013-interpreter-exit.
 *
 * The direct-dispatch control must pass. The interpreter case must also satisfy
 * the SAME service contract; its present failure is the intended RED result.
 * This catches interpreter execution of an HLE service address as LS code.
 */
#define main upstream_machine_status_main
#include "test_spu_machine_status.c"
#undef main

extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
extern SPU_THREAD_LOCAL volatile unsigned g_spurs_pm_exited;
extern uint64_t g_spu_interp_steps;
extern uint32_t g_spu_interp_last_pc;

static void direct_exit(spu_context* c)
{
    c->pc = 0x9C0;
    g_spu_trampoline_fn = spu_indirect_branch;
}

static void interpreted_exit(spu_context* c)
{
    c->pc = 0x4000;
    g_spu_trampoline_fn = spu_indirect_branch;
}

static void initialize(void)
{
    spu_context_init(&ctx, 0);
    ctx.image_id = 77;
    ctx.policy_mode = 1;
    ctx.status = SPU_STATUS_RUNNING;
    /* Match the existing policy-context exit/select fields. These addresses
     * are intercepted services; no executable payload belongs at LS 0x9C0. */
    u128 services = {{0}};
    services._u32[0] = 0x9C0;
    services._u32[1] = 0x9D0;
    spu_ls_write128(&ctx, 0x1E0, services);
    g_spurs_pm_exited = 0;
    g_spu_interp_steps = 0;
    g_spu_interp_last_pc = 0xFFFFFFFFu;
}

static int check_exit(const char* path, int halted, int interpreted)
{
    printf("%s: halted=%d exited=%u status=0x%X pc=0x%X lr=0x%X "
           "interp-last=0x%X interp-steps=%llu\n", path, halted,
           g_spurs_pm_exited, ctx.status, ctx.pc, ctx.gpr[0]._u32[0],
           g_spu_interp_last_pc, (unsigned long long)g_spu_interp_steps);
    int failures = 0;
#define REQUIRE(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL [%s]: %s\n", path, message); ++failures; } \
} while (0)
    REQUIRE(halted == 1, "exit service must terminate through spu_halt");
    REQUIRE(g_spurs_pm_exited == 1, "SPURS policy exit flag must be set");
    REQUIRE(ctx.pc == 0x9C0, "termination PC must be service 0x9C0, not 0x9C4");
    REQUIRE(ctx.status == SPU_STATUS_STOPPED_BY_STOP, "expected service exit status");
    if (interpreted) {
        REQUIRE(ctx.gpr[0]._u32[0] == 0x400C, "indirect-call LR must be next instruction");
        REQUIRE(g_spu_interp_last_pc == 0x4008,
                "full interpreter must execute the call, then intercept before decoding service LS");
    } else {
        REQUIRE(g_spu_interp_last_pc == 0xFFFFFFFFu, "direct control must not enter interpreter");
    }
#undef REQUIRE
    fflush(stdout);
    return failures;
}

int main(void)
{
    initialize();
    int halted = spu_run_with_halt(direct_exit, &ctx);
    if (check_exit("direct-control", halted, 0)) return 2;
    puts("PASS: direct policy-service control");

    initialize();
    /* Original hand-encoded fixture: ori $7,$7,0 forces the full interpreter
     * (outside the microstep subset), lqa $2,0x1E0 reads the exit address,
     * bisl $0,$2 calls it. No guest entry is registered for these addresses. */
    word(0x4000, 0x04000000u | (7u << 7) | 7u);
    word(0x4004, (0x061u << 23) | ((0x1E0u >> 2) << 7) | 2u);
    word(0x4008, (0x1A9u << 21) | (2u << 7));
    halted = spu_run_with_halt(interpreted_exit, &ctx);
    int failures = check_exit("full-interpreter", halted, 1);
    if (failures) {
        fprintf(stderr, "RED: %d policy-service contract assertion(s) failed\n", failures);
        return 1;
    }
    puts("PASS: full-interpreter policy exit without decoding service LS");
    initialize();
    ctx.policy_mode = 0;
    ctx.image_id = -1; /* Ordinary interpretation must still decode LS here. */
    spu_interp_run(&ctx, 0x9C0);
    assert(g_spurs_pm_exited == 0 && ctx.pc == 0x9C4);
    assert(ctx.status == SPU_STATUS_STOPPED_BY_STOP);
    assert(g_spu_interp_last_pc == 0x9C0);
    puts("PASS: non-policy interpreter behavior unchanged");
    return 0;
}
