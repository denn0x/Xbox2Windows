// Source-built x64 check for the second commit/protection case on #870:
// compares ordinary committed pages (MEM_PRIVATE) against a live shared-page view
// (MEM_MAPPED via desktop xmem.dll physical-page mappings) for CPU read/write
// accessibility, write-protection transitions (PAGE_READWRITE <-> PAGE_READONLY
// <-> PAGE_NOACCESS), aliasing visibility, and state preservation after failed
// allocation or failed reprotection.
//
// Scope:
// - Separate from the first-commit alignment/zero-fill/rounding fixture
//   (xmem_committed_memory), #888 (reserve-only), and #874 (device/queue).
// - Uses synthetic 64 KiB page patterns only.
// - Proposed desktop policy (not established for console):
//   * Ordinary committed pages use Win32 VirtualAlloc (MEM_PRIVATE).
//   * Live shared-page views use xmem.dll XMemAllocatePhysicalPages +
//     XMemMapPhysicalPages / XMemMapPhysicalPagesScatter over caller-owned
//     Windows placeholders (MEM_MAPPED), with per-view VirtualProtect.
//   * XMemVirtualAlloc on the desktop shim accepts the #888 reserve-only
//     profile (null address, 16 or 24 GiB, type 0x20402000, attribute/protection
//     0x21/PAGE_READWRITE or 3/0x801) and the startup commit profile (null address,
//     type 0x20003000 with 0x183/0x22004, 0x283/0x2404 or 0x283/0x2004, or type 0x20403000 with
//     0x181/PAGE_READWRITE), which commits a fresh block
//     (cpu_native_xmem_pool_commit). The 0x21 commit variant below stays refused.
//     No accepted argument set
//     names an existing address, so a second commit of an existing range through
//     XMemVirtualAlloc is not reachable through the current shim. The cases below
//     prove that each refusal comes from argument validation (a paired call that
//     differs only in the variable under test succeeds) instead of claiming a
//     state-dependent refusal.
//
// Mutation controls: `--mutate <name>` flips exactly one expected value. Each
// mutation must be reported as `check_failed: <check>` by the check that owns
// that expectation (registered as xmem_commit_protection_mutate_* in CMake).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "era_physical.h"

extern "C" __declspec(dllimport) BOOL WINAPI XMemAllocatePhysicalPages(ULONG_PTR flags, PULONG_PTR count,
                                                                     PULONG_PTR pages);
extern "C" __declspec(dllimport) BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR count, PULONG_PTR pages);
extern "C" __declspec(dllimport) void* WINAPI XMemMapPhysicalPages(void* base, ULONG_PTR count,
                                                                   const ULONG_PTR* tokens);
extern "C" __declspec(dllimport) BOOL WINAPI XMemMapPhysicalPagesScatter(void* const* addresses, ULONG_PTR count,
                                                                         const ULONG_PTR* tokens);
extern "C" __declspec(dllimport) void* WINAPI XMemVirtualAlloc(void* address, SIZE_T bytes, DWORD allocation_type,
                                                               ULONGLONG attributes, DWORD protection);
extern "C" __declspec(dllimport) BOOL XMemVirtualQuery(HANDLE process, LPCVOID address, void* buffer);

namespace {
constexpr SIZE_T page_bytes = static_cast<SIZE_T>(era_physical::page_bytes);
constexpr ULONG_PTR mem_64k_pages = static_cast<ULONG_PTR>(era_physical::mem_64k_pages);
constexpr SIZE_T probe_offsets[] = {0, 4096, page_bytes / 2, page_bytes - sizeof(std::uint32_t)};

constexpr SIZE_T gib = SIZE_T{1} << 30;
constexpr DWORD startup_reserve_type = 0x20402000;

const char* const known_mutations[] = {
    "xva_positive_refused",      // expect the accepted reserve-only call to be refused
    "xva_commit_accepted",       // expect the MEM_COMMIT variant to be accepted
    "xva_readonly_accepted",     // expect the PAGE_READONLY variant to be accepted
    "xva_state_dependent",       // expect live and free target addresses to be refused differently
    "private_second_va_aliases", // expect a second private commit to show the first page's bytes
    "private_type_mapped",       // expect ordinary committed pages to report MEM_MAPPED
    "mapped_decommit_succeeds",  // expect MEM_DECOMMIT on a live mapped view to succeed
};
const char* active_mutation = nullptr;

bool mutated(const char* name) noexcept
{
    return active_mutation && std::strcmp(active_mutation, name) == 0;
}

// Reports a named check. Every failing check prints one `check_failed:` line so a
// mutation run can show which check caught it.
bool record(bool pass, const char* name) noexcept
{
    if (!pass) std::printf("check_failed: %s\n", name);
    return pass;
}

// Values measured for the first_observable_discrepancy line. Each field is valid
// only when the check that measured it passed.
struct Discrepancy {
    bool private_type_ok = false;
    DWORD private_type = 0;
    bool private_decommit_ok = false;
    bool private_second_va_ok = false;
    bool private_second_va_aliases = true;
    bool mapped_type_ok = false;
    DWORD mapped_type = 0;
    bool mapped_decommit_ok = false;
    DWORD mapped_decommit_error = ERROR_SUCCESS;
    bool mapped_alias_ok = false;
    bool mapped_isolation_ok = false;
    bool mapped_persistence_ok = false;
};

struct QueryBuffer {
    MEMORY_BASIC_INFORMATION info;
    unsigned char tail[16];
};
static_assert(offsetof(QueryBuffer, tail) == 0x30 && sizeof(QueryBuffer) == 64);

struct FaultInfo {
    bool faulted = false;
    DWORD code = 0;
    ULONG_PTR access_type = 0;  // 0 = read, 1 = write
    ULONG_PTR fault_address = 0;
};

DWORD capture_fault(EXCEPTION_POINTERS* ep, FaultInfo* out) noexcept
{
    out->faulted = true;
    out->code = ep->ExceptionRecord->ExceptionCode;
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        out->access_type = ep->ExceptionRecord->ExceptionInformation[0];
        out->fault_address = ep->ExceptionRecord->ExceptionInformation[1];
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

FaultInfo probe_write_u32(volatile std::uint32_t* ptr, std::uint32_t value) noexcept
{
    FaultInfo info{};
    __try {
        *ptr = value;
    } __except (capture_fault(GetExceptionInformation(), &info)) {
    }
    return info;
}

FaultInfo probe_read_u32(const volatile std::uint32_t* ptr, std::uint32_t* out_value) noexcept
{
    FaultInfo info{};
    __try {
        *out_value = *ptr;
    } __except (capture_fault(GetExceptionInformation(), &info)) {
    }
    return info;
}

volatile std::uint32_t* word_at(void* base, SIZE_T offset) noexcept
{
    return reinterpret_cast<volatile std::uint32_t*>(static_cast<unsigned char*>(base) + offset);
}

bool write_pattern(void* page, std::uint32_t seed) noexcept
{
    for (std::size_t i = 0; i < sizeof(probe_offsets) / sizeof(probe_offsets[0]); ++i) {
        const auto expected = seed ^ static_cast<std::uint32_t>(i * 0x11111111u);
        const FaultInfo fault = probe_write_u32(word_at(page, probe_offsets[i]), expected);
        if (fault.faulted) return false;
    }
    return true;
}

bool check_pattern(void* page, std::uint32_t seed) noexcept
{
    for (std::size_t i = 0; i < sizeof(probe_offsets) / sizeof(probe_offsets[0]); ++i) {
        std::uint32_t observed = 0;
        const FaultInfo fault = probe_read_u32(word_at(page, probe_offsets[i]), &observed);
        const auto expected = seed ^ static_cast<std::uint32_t>(i * 0x11111111u);
        if (fault.faulted || observed != expected) return false;
    }
    return true;
}

bool check_query(const void* address, DWORD expected_state, DWORD expected_protect, DWORD expected_type) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) return false;
    if (info.State != expected_state || info.Protect != expected_protect || info.Type != expected_type) return false;

    QueryBuffer xmem_query{};
    if (!XMemVirtualQuery(GetCurrentProcess(), address, &xmem_query)) return false;
    return xmem_query.info.State == expected_state && xmem_query.info.Protect == expected_protect &&
           xmem_query.info.Type == expected_type;
}

// Reserves two adjacent 64 KiB Windows placeholders: [base, base + 64 KiB) and
// [base + 64 KiB, base + 128 KiB). The second slot stays unmapped as a boundary
// control for cross-region VirtualProtect checks.
struct TwoPlaceholders {
    void* first = nullptr;
    void* second = nullptr;

    bool create() noexcept
    {
        void* base = VirtualAlloc2(GetCurrentProcess(), nullptr, 2 * page_bytes,
                                   MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        if (!base) return false;
        if (!VirtualFree(base, page_bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            VirtualFree(base, 0, MEM_RELEASE);
            return false;
        }
        first = base;
        second = static_cast<unsigned char*>(base) + page_bytes;
        return true;
    }

    ~TwoPlaceholders()
    {
        if (first) VirtualFree(first, 0, MEM_RELEASE);
        if (second) VirtualFree(second, 0, MEM_RELEASE);
    }
};

// A second commit of ordinary pages at another VA. Win32 aliases memory only through
// section objects (MapViewOfFile*); no call takes an existing MEM_PRIVATE range as a
// mapping source, so this does not try to alias page0 directly. It measures what a
// second private commit does: a placeholder replaced with VirtualAlloc2 gets its own
// zero-filled bytes, and writes through either VA stay invisible to the other.
bool check_private_second_va(void* page0, std::uint32_t page0_seed, Discrepancy& d)
{
    void* placeholder = VirtualAlloc2(GetCurrentProcess(), nullptr, page_bytes,
                                      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!record(placeholder != nullptr, "private_second_va_placeholder")) return false;
    void* second = VirtualAlloc2(GetCurrentProcess(), placeholder, page_bytes,
                                 MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (!record(second == placeholder && check_query(second, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE),
                "private_second_va_commit")) {
        VirtualFree(second ? second : placeholder, 0, MEM_RELEASE);
        return false;
    }

    bool zero_filled = true;
    for (SIZE_T offset : probe_offsets) {
        std::uint32_t value = 1;
        if (probe_read_u32(word_at(second, offset), &value).faulted || value != 0) zero_filled = false;
    }
    // Aliasing would show page0's pattern at the second VA, and a write through the
    // second VA would change page0.
    const bool sees_page0 = check_pattern(second, page0_seed);
    constexpr std::uint32_t seed_second = 0x0f1e2d3cu;
    const bool wrote_second = write_pattern(second, seed_second);
    const bool page0_unchanged = check_pattern(page0, page0_seed);
    const bool page0_sees_second = check_pattern(page0, seed_second);
    const bool aliases = sees_page0 || page0_sees_second;
    VirtualFree(second, 0, MEM_RELEASE);

    const bool expect_alias = mutated("private_second_va_aliases");
    const bool pass = record(zero_filled && wrote_second && (aliases == expect_alias) &&
                                 (expect_alias || page0_unchanged),
                             "private_second_va_independent");
    d.private_second_va_ok = pass;
    d.private_second_va_aliases = aliases;
    std::printf("private_second_va: %s (zero_filled=%d sees_page0=%d page0_sees_second=%d page0_unchanged=%d)\n",
                pass ? "PASS" : "FAIL", zero_filled ? 1 : 0, sees_page0 ? 1 : 0, page0_sees_second ? 1 : 0,
                page0_unchanged ? 1 : 0);
    return pass;
}

bool check_ordinary_commit_protection(Discrepancy& d)
{
    // Reserve 2 pages, commit page 0, leave page 1 reserved (uncommitted).
    void* base = VirtualAlloc(nullptr, 2 * page_bytes, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) {
        std::printf("ordinary: reserve failed error=%lu\n", GetLastError());
        return false;
    }
    auto* page0 = static_cast<unsigned char*>(base);
    auto* page1 = page0 + page_bytes;
    bool ok = true;

    void* committed = VirtualAlloc(page0, page_bytes, MEM_COMMIT, PAGE_READWRITE);
    if (committed != page0 || !check_query(page0, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE) ||
        !check_query(page1, MEM_RESERVE, 0, MEM_PRIVATE)) {
        std::printf("ordinary: initial commit/query mismatch\n");
        VirtualFree(base, 0, MEM_RELEASE);
        return false;
    }

    constexpr std::uint32_t seed_a = 0x2468ace1u;
    constexpr std::uint32_t seed_b = 0x13579bdfu;
    if (!write_pattern(page0, seed_a) || !check_pattern(page0, seed_a)) {
        std::printf("ordinary: initial read/write failed\n");
        VirtualFree(base, 0, MEM_RELEASE);
        return false;
    }

    MEMORY_BASIC_INFORMATION type_info{};
    const bool type_queried = VirtualQuery(page0, &type_info, sizeof(type_info)) == sizeof(type_info);
    const DWORD expected_private_type = mutated("private_type_mapped") ? MEM_MAPPED : MEM_PRIVATE;
    d.private_type = type_queried ? type_info.Type : 0;
    d.private_type_ok = record(type_queried && type_info.Type == expected_private_type, "private_type");
    ok = d.private_type_ok && ok;

    if (!check_private_second_va(page0, seed_a, d)) ok = false;

    // Transition PAGE_READWRITE -> PAGE_READONLY.
    DWORD old_protect = 0;
    if (!VirtualProtect(page0, page_bytes, PAGE_READONLY, &old_protect) || old_protect != PAGE_READWRITE ||
        !check_query(page0, MEM_COMMIT, PAGE_READONLY, MEM_PRIVATE) || !check_pattern(page0, seed_a)) {
        std::printf("ordinary: PAGE_READONLY transition failed\n");
        ok = false;
    }

    // Negative control: writing to PAGE_READONLY must raise EXCEPTION_ACCESS_VIOLATION
    // and leave stored sentinel bytes untouched.
    const FaultInfo ro_write = probe_write_u32(word_at(page0, probe_offsets[1]), 0xdeadbeefu);
    if (!ro_write.faulted || ro_write.code != EXCEPTION_ACCESS_VIOLATION || ro_write.access_type != 1 ||
        ro_write.fault_address != reinterpret_cast<ULONG_PTR>(word_at(page0, probe_offsets[1])) ||
        !check_pattern(page0, seed_a)) {
        std::printf("ordinary: PAGE_READONLY write control mismatch faulted=%d code=0x%lx type=%llu\n",
                    ro_write.faulted ? 1 : 0, ro_write.code, static_cast<unsigned long long>(ro_write.access_type));
        ok = false;
    }

    // Transition PAGE_READONLY -> PAGE_NOACCESS.
    if (!VirtualProtect(page0, page_bytes, PAGE_NOACCESS, &old_protect) || old_protect != PAGE_READONLY ||
        !check_query(page0, MEM_COMMIT, PAGE_NOACCESS, MEM_PRIVATE)) {
        std::printf("ordinary: PAGE_NOACCESS transition failed\n");
        ok = false;
    }

    // Negative controls: both read and write on PAGE_NOACCESS must fault.
    std::uint32_t dummy = 0;
    const FaultInfo na_read = probe_read_u32(word_at(page0, probe_offsets[0]), &dummy);
    const FaultInfo na_write = probe_write_u32(word_at(page0, probe_offsets[0]), 0xbadc0de0u);
    if (!na_read.faulted || na_read.code != EXCEPTION_ACCESS_VIOLATION || na_read.access_type != 0 ||
        !na_write.faulted || na_write.code != EXCEPTION_ACCESS_VIOLATION || na_write.access_type != 1) {
        std::printf("ordinary: PAGE_NOACCESS read/write control mismatch\n");
        ok = false;
    }

    // Restore PAGE_NOACCESS -> PAGE_READWRITE and verify bytes survived intact.
    if (!VirtualProtect(page0, page_bytes, PAGE_READWRITE, &old_protect) || old_protect != PAGE_NOACCESS ||
        !check_query(page0, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE) || !check_pattern(page0, seed_a) ||
        !write_pattern(page0, seed_b) || !check_pattern(page0, seed_b)) {
        std::printf("ordinary: restore PAGE_READWRITE failed\n");
        ok = false;
    }

    // Failed reprotection controls: invalid flag (0), conflicting flags, and spanning into
    // uncommitted page 1 must fail and leave page 0's protection and bytes intact.
    SetLastError(ERROR_SUCCESS);
    const BOOL bad_zero = VirtualProtect(page0, page_bytes, 0, &old_protect);
    const DWORD err_zero = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const BOOL bad_conflict = VirtualProtect(page0, page_bytes, PAGE_READWRITE | PAGE_READONLY, &old_protect);
    const DWORD err_conflict = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const BOOL bad_span = VirtualProtect(page0, 2 * page_bytes, PAGE_READONLY, &old_protect);
    const DWORD err_span = GetLastError();
    if (bad_zero || err_zero != ERROR_INVALID_PARAMETER || bad_conflict || err_conflict != ERROR_INVALID_PARAMETER ||
        bad_span || err_span != ERROR_INVALID_ADDRESS ||
        !check_query(page0, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE) || !check_pattern(page0, seed_b)) {
        std::printf("ordinary: failed reprotection control mismatch errs=(%lu,%lu,%lu)\n", err_zero, err_conflict,
                    err_span);
        ok = false;
    }

    // Failed commit past reservation boundary must fail and leave page 0 committed + intact
    // and page 1 uncommitted.
    SetLastError(ERROR_SUCCESS);
    void* over_commit = VirtualAlloc(page0, 3 * page_bytes, MEM_COMMIT, PAGE_READWRITE);
    const DWORD err_over = GetLastError();
    if (over_commit != nullptr || err_over != ERROR_INVALID_ADDRESS ||
        !check_query(page0, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE) ||
        !check_query(page1, MEM_RESERVE, 0, MEM_PRIVATE) || !check_pattern(page0, seed_b)) {
        std::printf("ordinary: failed over-commit control mismatch ptr=%p err=%lu\n", over_commit, err_over);
        ok = false;
    }

    // Idempotent re-commit on ordinary MEM_PRIVATE committed page succeeds, and MEM_DECOMMIT
    // returns it to MEM_RESERVE.
    SetLastError(ERROR_SUCCESS);
    void* recommit = VirtualAlloc(page0, page_bytes, MEM_COMMIT, PAGE_READWRITE);
    if (recommit != page0 || !check_pattern(page0, seed_b)) {
        std::printf("ordinary: idempotent recommit failed err=%lu\n", GetLastError());
        ok = false;
    }
    SetLastError(ERROR_SUCCESS);
    const BOOL private_decommit = VirtualFree(page0, page_bytes, MEM_DECOMMIT);
    const DWORD err_private_decommit = GetLastError();
    d.private_decommit_ok =
        record(private_decommit && check_query(page0, MEM_RESERVE, 0, MEM_PRIVATE), "private_decommit");
    if (!d.private_decommit_ok) {
        std::printf("ordinary: MEM_DECOMMIT failed err=%lu\n", err_private_decommit);
        ok = false;
    }

    VirtualFree(base, 0, MEM_RELEASE);
    std::printf("ordinary_commit_protection: %s (bad_zero=%lu bad_conflict=%lu bad_span=%lu over_commit=%lu)\n",
                ok ? "PASS" : "FAIL", err_zero, err_conflict, err_span, err_over);
    return ok;
}

bool check_shared_view_protection(Discrepancy& d)
{

    ULONG_PTR count = 1;
    ULONG_PTR token = 0;
    if (!XMemAllocatePhysicalPages(mem_64k_pages, &count, &token) || count != 1 || token == 0) {
        std::printf("shared: XMemAllocatePhysicalPages failed err=%lu\n", GetLastError());
        return false;
    }

    TwoPlaceholders slots_a;
    TwoPlaceholders slots_b;
    if (!slots_a.create() || !slots_b.create()) {
        std::printf("shared: placeholder creation failed err=%lu\n", GetLastError());
        XMemFreePhysicalPages(1, &token);
        return false;
    }

    void* view1 = slots_a.first;
    void* view2 = slots_b.first;
    bool ok = true;

    if (XMemMapPhysicalPages(view1, 1, &token) != view1 || !XMemMapPhysicalPagesScatter(&view2, 1, &token)) {
        std::printf("shared: mapping views failed err=%lu\n", GetLastError());
        XMemMapPhysicalPages(view1, 1, nullptr);
        XMemFreePhysicalPages(1, &token);
        return false;
    }

    if (!check_query(view1, MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED) ||
        !check_query(view2, MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED)) {
        std::printf("shared: initial VirtualQuery/XMemVirtualQuery mismatch\n");
        ok = false;
    }
    MEMORY_BASIC_INFORMATION type_info{};
    const bool type_queried = VirtualQuery(view1, &type_info, sizeof(type_info)) == sizeof(type_info);
    d.mapped_type = type_queried ? type_info.Type : 0;
    d.mapped_type_ok = record(type_queried && type_info.Type == MEM_MAPPED, "mapped_type");
    ok = d.mapped_type_ok && ok;

    // Live shared-page aliasing: write via view1, observe immediately via view2.
    constexpr std::uint32_t seed_1 = 0x55aa33ccu;
    constexpr std::uint32_t seed_2 = 0x9966a55au;
    constexpr std::uint32_t seed_3 = 0x71234567u;
    d.mapped_alias_ok = record(write_pattern(view1, seed_1) && check_pattern(view2, seed_1), "mapped_alias");
    if (!d.mapped_alias_ok) {
        std::printf("shared: initial alias visibility failed\n");
        ok = false;
    }
    bool isolation = true;

    // Protect view1 to PAGE_READONLY; view2 must remain PAGE_READWRITE (per-view protection).
    DWORD old_protect = 0;
    if (!VirtualProtect(view1, page_bytes, PAGE_READONLY, &old_protect) || old_protect != PAGE_READWRITE ||
        !check_query(view1, MEM_COMMIT, PAGE_READONLY, MEM_MAPPED) ||
        !check_query(view2, MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED) || !check_pattern(view1, seed_1)) {
        std::printf("shared: PAGE_READONLY transition on view1 failed\n");
        ok = false;
    }

    // Negative control: writing through PAGE_READONLY view1 faults and leaves bytes untouched.
    const FaultInfo ro_write = probe_write_u32(word_at(view1, probe_offsets[2]), 0x0badf00du);
    if (!ro_write.faulted || ro_write.code != EXCEPTION_ACCESS_VIOLATION || ro_write.access_type != 1 ||
        ro_write.fault_address != reinterpret_cast<ULONG_PTR>(word_at(view1, probe_offsets[2])) ||
        !check_pattern(view1, seed_1) || !check_pattern(view2, seed_1)) {
        std::printf("shared: view1 PAGE_READONLY write control mismatch\n");
        isolation = false;
        ok = false;
    }

    // Live shared-page discrepancy: writing through still-RW view2 succeeds and updates
    // what PAGE_READONLY view1 reads.
    if (!write_pattern(view2, seed_2) || !check_pattern(view1, seed_2)) {
        std::printf("shared: write through view2 not observed through RO view1\n");
        isolation = false;
        ok = false;
    }

    // Protect view1 to PAGE_NOACCESS: both read and write through view1 must fault, while
    // view2 remains readable and writable.
    if (!VirtualProtect(view1, page_bytes, PAGE_NOACCESS, &old_protect) || old_protect != PAGE_READONLY ||
        !check_query(view1, MEM_COMMIT, PAGE_NOACCESS, MEM_MAPPED) ||
        !check_query(view2, MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED)) {
        std::printf("shared: PAGE_NOACCESS transition on view1 failed\n");
        ok = false;
    }
    std::uint32_t dummy = 0;
    const FaultInfo na_read = probe_read_u32(word_at(view1, probe_offsets[0]), &dummy);
    const FaultInfo na_write = probe_write_u32(word_at(view1, probe_offsets[0]), 0xfeedfaceu);
    if (!na_read.faulted || na_read.code != EXCEPTION_ACCESS_VIOLATION || na_read.access_type != 0 ||
        !na_write.faulted || na_write.code != EXCEPTION_ACCESS_VIOLATION || na_write.access_type != 1 ||
        !check_pattern(view2, seed_2) || !write_pattern(view2, seed_3)) {
        std::printf("shared: PAGE_NOACCESS isolation control mismatch\n");
        isolation = false;
        ok = false;
    }
    d.mapped_isolation_ok = record(isolation, "mapped_isolation");

    // Restore view1 to PAGE_READWRITE and confirm it observes seed_3 written via view2.
    if (!VirtualProtect(view1, page_bytes, PAGE_READWRITE, &old_protect) || old_protect != PAGE_NOACCESS ||
        !check_query(view1, MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED) || !check_pattern(view1, seed_3)) {
        std::printf("shared: restore view1 PAGE_READWRITE failed\n");
        ok = false;
    }

    // Failed reprotection controls on live shared view: invalid flag (0) and spanning into
    // unmapped placeholder slots_a.second must fail and leave view1/view2 intact.
    SetLastError(ERROR_SUCCESS);
    const BOOL bad_zero = VirtualProtect(view1, page_bytes, 0, &old_protect);
    const DWORD err_zero = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const BOOL bad_span = VirtualProtect(view1, 2 * page_bytes, PAGE_READONLY, &old_protect);
    const DWORD err_span = GetLastError();
    if (bad_zero || err_zero != ERROR_INVALID_PARAMETER || bad_span || err_span != ERROR_INVALID_ADDRESS ||
        !check_query(view1, MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED) || !check_pattern(view1, seed_3) ||
        !check_pattern(view2, seed_3)) {
        std::printf("shared: failed reprotection control mismatch errs=(%lu,%lu)\n", err_zero, err_span);
        ok = false;
    }

    // Observable behavior vs ordinary committed pages:
    // - VirtualAlloc(..., MEM_COMMIT, PAGE_READWRITE) on a live MEM_MAPPED view succeeds
    //   idempotently while keeping MEM_MAPPED and stored bytes intact.
    // - VirtualFree(..., MEM_DECOMMIT) succeeds on ordinary MEM_PRIVATE pages, but fails on
    //   a live MEM_MAPPED view (leaving both views, bytes, and protections intact).
    SetLastError(ERROR_SUCCESS);
    void* mapped_recommit = VirtualAlloc(view1, page_bytes, MEM_COMMIT, PAGE_READWRITE);
    if (mapped_recommit != view1 || !check_query(view1, MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED) ||
        !check_pattern(view1, seed_3) || !check_pattern(view2, seed_3)) {
        std::printf("shared: VirtualAlloc(MEM_COMMIT) on mapped view mismatch ptr=%p err=%lu\n", mapped_recommit,
                    GetLastError());
        ok = false;
    }
    SetLastError(ERROR_SUCCESS);
    const BOOL mapped_decommit = VirtualFree(view1, page_bytes, MEM_DECOMMIT);
    d.mapped_decommit_error = GetLastError();
    const bool expect_decommit = mutated("mapped_decommit_succeeds");
    const bool decommit_as_expected =
        expect_decommit ? mapped_decommit != FALSE
                        : (!mapped_decommit && d.mapped_decommit_error == ERROR_INVALID_PARAMETER);
    d.mapped_decommit_ok =
        record(decommit_as_expected && check_query(view1, MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED) &&
                   check_query(view2, MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED) && check_pattern(view1, seed_3) &&
                   check_pattern(view2, seed_3),
               "mapped_decommit");
    if (!d.mapped_decommit_ok) {
        std::printf("shared: VirtualFree(MEM_DECOMMIT) on mapped view control mismatch ok=%d err=%lu\n",
                    mapped_decommit ? 1 : 0, d.mapped_decommit_error);
        ok = false;
    }

    // Remap over a live view and free while mapped are refused because of the shim's
    // page state (the same calls succeed once the views are unmapped, below) and must
    // leave both live views intact.
    SetLastError(ERROR_SUCCESS);
    void* refused_remap = XMemMapPhysicalPages(view1, 1, &token);
    const DWORD err_remap = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const BOOL refused_free = XMemFreePhysicalPages(1, &token);
    const DWORD err_free = GetLastError();
    if (refused_remap != nullptr || err_remap != ERROR_INVALID_PARAMETER ||
        refused_free || err_free != ERROR_INVALID_PARAMETER ||
        !check_query(view1, MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED) ||
        !check_query(view2, MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED) || !check_pattern(view1, seed_3) ||
        !check_pattern(view2, seed_3)) {
        std::printf("shared: failed xmem remap/free preservation mismatch errs=(%lu,%lu)\n", err_remap, err_free);
        ok = false;
    }

    // Unmap both views, remap view1, and confirm bytes survive unmap/remap while the grant lives.
    d.mapped_persistence_ok = record(XMemMapPhysicalPages(view1, 1, nullptr) == view1 &&
                                         XMemMapPhysicalPagesScatter(&view2, 1, nullptr) &&
                                         XMemMapPhysicalPages(view1, 1, &token) == view1 &&
                                         check_pattern(view1, seed_3),
                                     "mapped_persistence");
    if (!d.mapped_persistence_ok) {
        std::printf("shared: unmap/remap persistence check failed err=%lu\n", GetLastError());
        ok = false;
    }

    if (XMemMapPhysicalPages(view1, 1, nullptr) != view1 || !XMemFreePhysicalPages(1, &token)) {
        std::printf("shared: cleanup unmap/free failed err=%lu\n", GetLastError());
        ok = false;
    }

    std::printf("shared_view_protection: %s (bad_zero=%lu bad_span=%lu mapped_decommit=%lu remap=%lu free=%lu)\n",
                ok ? "PASS" : "FAIL", err_zero, err_span, d.mapped_decommit_error, err_remap, err_free);
    return ok;
}
struct XvaCall {
    void* result = nullptr;
    DWORD error = ERROR_SUCCESS;
};

XvaCall call_xva(void* address, DWORD type, ULONGLONG attributes, DWORD protection) noexcept
{
    XvaCall call{};
    SetLastError(ERROR_SUCCESS);
    call.result = XMemVirtualAlloc(address, 16 * gib, type, attributes, protection);
    call.error = GetLastError();
    return call;
}

void release_if_allocated(const XvaCall& call) noexcept
{
    if (call.result) VirtualFree(call.result, 0, MEM_RELEASE);
}

bool refused_with_invalid_parameter(const XvaCall& call) noexcept
{
    return call.result == nullptr && call.error == ERROR_INVALID_PARAMETER;
}

// Can a second commit be reached through XMemVirtualAlloc on the desktop shim?
// Each case starts from the accepted #888 reserve-only call (null address, 16 GiB,
// type 0x20402000, attribute 0x21, PAGE_READWRITE) and changes exactly one variable.
// The accepted call must succeed in the same run, so a refusal of the variant is
// caused by that variable alone. A refusal caused by argument validation is not a
// second-commit result, so the verdict is "not reachable through the current shim".
bool check_xmem_virtual_alloc_second_commit()
{
    bool ok = true;

    // Paired positive: the accepted reserve-only call reserves address space.
    const XvaCall accepted = call_xva(nullptr, startup_reserve_type, 0x21, PAGE_READWRITE);
    MEMORY_BASIC_INFORMATION info{};
    const bool accepted_reserved = accepted.result &&
                                   VirtualQuery(accepted.result, &info, sizeof(info)) == sizeof(info) &&
                                   info.State == MEM_RESERVE && info.RegionSize >= 16 * gib;
    const bool expect_accepted = !mutated("xva_positive_refused");
    ok = record(accepted_reserved == expect_accepted, "xva_accepted_pair") && ok;

    // Variable: allocation type gains MEM_COMMIT (the only way to ask for a commit).
    const XvaCall commit = call_xva(nullptr, startup_reserve_type | MEM_COMMIT, 0x21, PAGE_READWRITE);
    release_if_allocated(commit);
    const bool expect_commit_refused = !mutated("xva_commit_accepted");
    ok = record(refused_with_invalid_parameter(commit) == expect_commit_refused, "xva_commit_bit") && ok;

    // Variable: protection PAGE_READWRITE -> PAGE_READONLY with attribute 0x21.
    const XvaCall readonly = call_xva(nullptr, startup_reserve_type, 0x21, PAGE_READONLY);
    release_if_allocated(readonly);
    const bool expect_readonly_refused = !mutated("xva_readonly_accepted");
    ok = record(refused_with_invalid_parameter(readonly) == expect_readonly_refused, "xva_protection") && ok;

    // Variable: address. Target the live reservation from the accepted call, then the
    // same address after it is released. A refusal that depends on memory state would
    // differ between the two; argument validation refuses both the same way.
    XvaCall address_live{};
    XvaCall address_free{};
    bool target_was_free = false;
    if (accepted.result) {
        address_live = call_xva(accepted.result, startup_reserve_type, 0x21, PAGE_READWRITE);
        release_if_allocated(address_live);
        VirtualFree(accepted.result, 0, MEM_RELEASE);
        MEMORY_BASIC_INFORMATION freed{};
        target_was_free = VirtualQuery(accepted.result, &freed, sizeof(freed)) == sizeof(freed) &&
                          freed.State == MEM_FREE;
        address_free = call_xva(accepted.result, startup_reserve_type, 0x21, PAGE_READWRITE);
        release_if_allocated(address_free);
    }
    const bool state_independent = accepted.result && target_was_free &&
                                   refused_with_invalid_parameter(address_live) &&
                                   refused_with_invalid_parameter(address_free) &&
                                   address_live.error == address_free.error;
    const bool expect_state_independent = !mutated("xva_state_dependent");
    ok = record(state_independent == expect_state_independent, "xva_address_state_independent") && ok;

    if (ok) {
        std::printf("xmem_virtual_alloc_second_commit: NOT REACHABLE through the current shim "
                    "(accepted=reserved commit_bit_err=%lu readonly_err=%lu address_live_err=%lu "
                    "address_free_err=%lu; each refusal is argument validation, not memory state)\n",
                    commit.error, readonly.error, address_live.error, address_free.error);
    } else {
        std::printf("xmem_virtual_alloc_second_commit: FAIL (accepted=%p/%lu commit=%p/%lu readonly=%p/%lu "
                    "address_live=%p/%lu address_free=%p/%lu target_was_free=%d)\n",
                    accepted.result, accepted.error, commit.result, commit.error, readonly.result, readonly.error,
                    address_live.result, address_live.error, address_free.result, address_free.error,
                    target_was_free ? 1 : 0);
    }
    return ok;
}

// Prints the discrepancy line only from measured values whose checks passed;
// otherwise names every field that could not be established.
bool print_discrepancy(const Discrepancy& d)
{
    struct Field {
        const char* name;
        bool ok;
    };
    const Field fields[] = {
        {"private_type", d.private_type_ok},
        {"private_decommit", d.private_decommit_ok},
        {"private_second_va", d.private_second_va_ok},
        {"mapped_type", d.mapped_type_ok},
        {"mapped_decommit", d.mapped_decommit_ok},
        {"mapped_alias", d.mapped_alias_ok},
        {"mapped_isolation", d.mapped_isolation_ok},
        {"mapped_persistence", d.mapped_persistence_ok},
    };
    bool all_ok = true;
    for (const Field& field : fields) all_ok = all_ok && field.ok;
    if (!all_ok) {
        std::printf("first_observable_discrepancy: not printed; failing_fields=");
        const char* separator = "";
        for (const Field& field : fields) {
            if (field.ok) continue;
            std::printf("%s%s", separator, field.name);
            separator = ",";
        }
        std::printf("\n");
        return false;
    }
    std::printf("first_observable_discrepancy: ordinary_commit=type 0x%lx, MEM_DECOMMIT succeeded, second-VA "
                "commit %s vs live_shared_view=type 0x%lx, MEM_DECOMMIT refused err=%lu, second view aliased "
                "live, per-view VirtualProtect isolation observed, bytes persisted across unmap/remap\n",
                d.private_type, d.private_second_va_aliases ? "aliased" : "independent", d.mapped_type,
                d.mapped_decommit_error);
    return true;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--mutate") == 0) {
        for (const char* name : known_mutations)
            if (std::strcmp(argv[2], name) == 0) active_mutation = name;
        if (!active_mutation) {
            std::printf("unknown mutation: %s\n", argv[2]);
            return 2;
        }
        std::printf("mutation=%s\n", active_mutation);
    } else if (argc != 1) {
        std::printf("usage: xmem_commit_protection_check [--mutate <name>]\n");
        return 2;
    }

    Discrepancy discrepancy{};
    const bool ordinary_ok = check_ordinary_commit_protection(discrepancy);
    const bool shared_ok = check_shared_view_protection(discrepancy);
    const bool xva_ok = check_xmem_virtual_alloc_second_commit();
    const bool discrepancy_ok = print_discrepancy(discrepancy);
    const bool passed = ordinary_ok && shared_ok && xva_ok && discrepancy_ok;

    std::printf("xmem_commit_protection: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
