/* Context-open descriptors must survive the generated generic HLE fallback.
 * Real registrations, bridge, file operations and synthetic bytes only.
 * Driven by tools/test_ppu_fs_ftruncate.py; no private handler calls.
 */
#include "ppu_fs.cpp"
#include "ppu_hle.cpp"
#include <cstdlib>
#include <utility>
#include <initializer_list>
#ifndef _WIN32
#include <signal.h>
#include <sys/resource.h>
#endif

/* Host-only linkage, following test_sysmodule_unload_module_ex.cpp. No PRX
 * is loaded; unexpected guest execution or unrelated APIs abort the test. */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0x10000;
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
int g_resv_store_active = 0;
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t, uint64_t, uint64_t) { std::abort(); }
void ppu_prof_stamp(void*, unsigned) {}
uint32_t ppu_prof_resolve_host(void*) { return 0; }
void ps3_msf(const char*, ...) {}
void ppu_guest_caller(char*, size_t) { std::abort(); }
void ps3_vfs_ps3game_fallback(char*, size_t) { std::abort(); }
void ydkj_host_bt(const char*) { std::abort(); }
void ppu_dump_guest_stack(ppu_context*, const char*) { std::abort(); }
void ppu_dump_bctrl_ring(uint32_t, const char*) { std::abort(); }
void ppu_guest_callstack(const char*) { std::abort(); }
int spu_coh_is_reserved(uint32_t) { std::abort(); }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
void ppu_resv_break_store(uint64_t) { std::abort(); }
void ps3_ww_report_inline(uint32_t, uint64_t, int) { std::abort(); }
uint32_t vm_read32(uint64_t a) {
    if (a + 4 > ppu_vm_size) std::abort();
    uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v);
}
uint64_t vm_read64(uint64_t a) {
    if (a + 8 > ppu_vm_size) std::abort();
    uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v);
}
void vm_write32(uint64_t a, uint32_t v) {
    if (a + 4 > ppu_vm_size) std::abort();
    v = __builtin_bswap32(v); memcpy(vm_base + a, &v, 4);
}
void vm_write64(uint64_t a, uint64_t v) {
    if (a + 8 > ppu_vm_size) std::abort();
    v = __builtin_bswap64(v); memcpy(vm_base + a, &v, 8);
}
}
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0;

static unsigned failures;
static void expect(uint64_t actual, uint64_t wanted, const char* label)
{
    if (actual != wanted) ++failures;
    printf("[%s] %s: got 0x%llX, want 0x%llX\n", actual == wanted ? "PASS" : "FAIL",
           label, (unsigned long long)actual, (unsigned long long)wanted);
}

static uint32_t call(uint32_t nid, uint64_t a0, uint64_t a1 = 0,
                     uint64_t a2 = 0, uint64_t a3 = 0)
{
    ppu_context ctx{}; ctx.gpr[1] = 0x8000; ctx.gpr[2] = 0x12345678;
    ctx.gpr[3] = a0; ctx.gpr[4] = a1; ctx.gpr[5] = a2; ctx.gpr[6] = a3;
    ps3_hle_call(nid, &ctx);
    return uint32_t(ctx.gpr[3]);
}

static int64_t host_size(const char* path)
{
#ifdef _WIN32
    struct _stat64 st;
    return _stat64(path, &st) == 0 ? st.st_size : -1;
#else
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : -1;
#endif
}

static int open_fd(uint32_t flags)
{
    vm_write32(0x300, UINT32_MAX);
    expect(call(0x718BF5F8, 0x100, flags, 0x300), 0, "Open");
    int fd = int(vm_read32(0x300));
    if (fd < 3 || fd >= FS_MAX) std::exit(2);
    return fd;
}

static void expect_position(int fd, uint64_t wanted)
{
    expect(call(0xA397D042, fd, 0, 1, 0x310), 0, "Lseek(CUR) status");
    expect(vm_read64(0x310), wanted, "Ftruncate preserves logical file position");
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);
    static uint8_t memory[0x10000]; vm_base = memory;
    strcpy((char*)memory + 0x100, "/dev_hdd0/contract.bin");
    for (unsigned i = 0; i < 5056; ++i) memory[0x1000 + i] = uint8_t(37 * i + 11);
    const char* path = argv[1];
    ppu_hle_init();                 // actual generator output for cellFs.c
    ppu_fs_register();              // same precedence as the native runner
    // Read-only identity check ensures this is a context-open lifecycle, even
    // though generic Open would also return a superficially valid integer fd.
    for (auto wanted : {std::pair<uint32_t, hle_ctx_fn>{0x718BF5F8, cellFsOpen},
                        {0xECDCF2AB, cellFsWrite}, {0x2CB51F0D, cellFsClose}}) {
        hle_ctx_fn selected = nullptr;
        for (uint32_t i = 0; i < g_ctx_count; ++i)
            if (g_ctx[i].nid == wanted.first) { selected = g_ctx[i].fn; break; }
        expect(selected == wanted.second, true, "context lifecycle registration identity");
    }

    // Exact reported transaction: no seek/flush/other HLE operation inserted.
    int fd = open_fd(0x242);
    expect(host_size(path), 0, "truncating Open length");
    uint32_t resize = call(0x0E2939E5, fd, 9026120);
    expect(resize, 0, "Ftruncate context-open descriptor (base fails EBADF 0x8001002A)");
    expect(host_size(path), 9026120, "length immediately after Ftruncate");
    vm_write64(0x308, UINT64_MAX);
    expect(call(0xECDCF2AB, fd, 0x1000, 5056, 0x308), 0, "Write");
    expect(vm_read64(0x308), 5056, "actual bytes written");
    expect(host_size(path), 9026120, "small overwrite preserves established extent");
    expect(call(0x2CB51F0D, fd), 0, "Close");
    expect(host_size(path), 9026120, "final extent after Close");
    FILE* witness = fopen(path, "rb");
    if (!witness) return 2;
    uint8_t bytes[5056];
    expect(fread(bytes, 1, sizeof bytes, witness), sizeof bytes, "payload length");
    expect(memcmp(bytes, memory + 0x1000, sizeof bytes) == 0, true, "payload at offset zero");
    expect(fseek(witness, -1, SEEK_END), 0, "seek to extended tail");
    expect(fgetc(witness), 0, "extended tail is zero-filled");
    fclose(witness);

    // Buffered writes must be committed BEFORE shrinking; a position beyond
    // new EOF must survive both shrinking and subsequent growth.
    fd = open_fd(0x242);
    expect(call(0xECDCF2AB, fd, 0x1000, 5056, 0x308), 0, "buffered Write before shrink");
    expect(call(0x0E2939E5, fd, 1024), 0, "shrink after buffered Write");
    expect(host_size(path), 1024, "shrink length");
    expect_position(fd, 5056);
    expect(call(0x0E2939E5, fd, 9026120), 0, "grow with nonzero position");
    expect_position(fd, 5056);
    expect(call(0x0E2939E5, fd, UINT64_MAX), 0x80010002, "unrepresentable size returns EINVAL");
    expect(host_size(path), 9026120, "rejected size leaves extent unchanged");
    expect_position(fd, 5056);
    expect(call(0x2CB51F0D, fd), 0, "Close after position checks");
    expect(host_size(path), 9026120, "Close cannot replay stale buffered writes");

    for (int invalid : {-1, 0, fd, 256})
        expect(call(0x0E2939E5, uint32_t(invalid), 64), 0x8001002A, "invalid/closed fd returns EBADF");
    fd = open_fd(0);
    expect(call(0x0E2939E5, fd, 64), 0x8001002A, "read-only descriptor returns EBADF");
    expect(host_size(path), 9026120, "read-only rejection preserves file");
    expect(call(0x2CB51F0D, fd), 0, "Close read-only fd");

    // A real host refusal on an otherwise writable descriptor; no resize or
    // HLE return is mocked. Windows forbids shrinking a file with a mapped view.
#ifdef _WIN32
    HANDLE host = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE mapping = host != INVALID_HANDLE_VALUE ? CreateFileMappingA(host, nullptr, PAGE_READWRITE, 0, 0, nullptr) : nullptr;
    void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 4096) : nullptr;
    if (!view) return 2;
    fd = open_fd(2);
    expect(call(0x0E2939E5, fd, 64) != 0, true, "host-refused resize cannot report success");
    expect(host_size(path), 9026120, "host-refused resize preserves extent");
    expect_position(fd, 0);
    expect(call(0x2CB51F0D, fd), 0, "Close host-refused fd");
    UnmapViewOfFile(view); CloseHandle(mapping); CloseHandle(host);
#else
    struct rlimit previous, limited;
    if (getrlimit(RLIMIT_FSIZE, &previous) != 0) return 2;
    limited = previous; limited.rlim_cur = 1024;
    auto old_signal = signal(SIGXFSZ, SIG_IGN);
    if (setrlimit(RLIMIT_FSIZE, &limited) != 0) return 2;
    fd = open_fd(2);
    expect(call(0x0E2939E5, fd, 9026121), 0x80010020, "host-refused resize returns EFBIG");
    expect(host_size(path), 9026120, "host-refused resize preserves extent");
    expect_position(fd, 0);
    expect(call(0x2CB51F0D, fd), 0, "Close host-refused fd");
    if (setrlimit(RLIMIT_FSIZE, &previous) != 0) return 2;
    signal(SIGXFSZ, old_signal);
#endif
    // Shrinking after buffered input must discard bytes beyond the new EOF.
    // Read immediately after resize: an intervening seek could hide stale input.
    fd = open_fd(2);
    vm_write64(0x308, UINT64_MAX);
    expect(call(0x4D5FF8E2, fd, 0x400, 1, 0x308), 0, "Read before shrink");
    expect(vm_read64(0x308), 1, "buffered input reads one byte");
    expect(memory[0x400], memory[0x1000], "buffered input prefix");
    expect(call(0x0E2939E5, fd, 0), 0, "shrink after buffered Read");
    expect(host_size(path), 0, "buffered-input shrink length");
    vm_write64(0x308, UINT64_MAX);
    expect(call(0x4D5FF8E2, fd, 0x400, 1, 0x308), 0, "Read after shrink");
    expect(vm_read64(0x308), 0, "no stale buffered input beyond new EOF");
    expect_position(fd, 1);
    expect(call(0xA397D042, fd, 0, 0, 0x310), 0, "Lseek(SET) after input shrink");
    expect(vm_read64(0x310), 0, "seek returns position zero");
    vm_write64(0x308, UINT64_MAX);
    expect(call(0x4D5FF8E2, fd, 0x400, 1, 0x308), 0, "Read at zero after shrink");
    expect(vm_read64(0x308), 0, "EOF after seeking to zero");
    expect(call(0x2CB51F0D, fd), 0, "Close after input shrink");
    remove(path);
    printf("%s: context HLE Ftruncate contract (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
