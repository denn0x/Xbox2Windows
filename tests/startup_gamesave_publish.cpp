// Experimental (docs/startup-runtime.md): a multi-blob update of the game-save store is published
// under a lock that readers in other processes share, so a reader sees the whole old generation or
// the whole new one. This file is built twice from generated copies of save_store.h and
// file_lock.h (see CMakeLists.txt): one adds a pause point at each step of the publication, the
// other also turns the lock into a no-op. The first must pass; the second must be caught (a reader
// sees a mixed or incomplete generation), which shows the check can fail. The test starts itself
// again for the writer, the readers and a lock holder; handshakes are files, every wait has a
// deadline. Synthetic data only.
#include <startup_stubs/save_store.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <ctime>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using x2w_save::Status;

// Unique per process and per call even when several tests start in the same
// second (std::rand() without srand() is constant on some runtimes).
static std::string unique_suffix() {
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    return std::to_string(pid) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

const char* status_name(Status status)
{
    switch (status) {
    case Status::ok: return "ok";
    case Status::bad_name: return "bad_name";
    case Status::too_large: return "too_large";
    case Status::not_found: return "not_found";
    case Status::io_error: return "io_error";
    case Status::busy: return "busy";
    }
    return "unknown";
}

namespace {
constexpr int exit_old = 20, exit_new = 21, exit_mixed = 22, exit_busy = 23, exit_other = 24;
constexpr auto handshake_deadline = std::chrono::seconds(30);

fs::path pause_dir;  // set in the writer process only
unsigned pause_point = ~0u;

bool wait_for(const fs::path& file)
{
    const auto deadline = std::chrono::steady_clock::now() + handshake_deadline;
    std::error_code ec;
    while (!fs::exists(file, ec)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
void touch(const fs::path& file) { std::ofstream(file, std::ios::binary) << "x"; }

// Two blobs per generation; the bytes tell which generation a blob belongs to.
std::vector<unsigned char> bytes_of(unsigned generation, unsigned blob)
{
    std::vector<unsigned char> bytes(4096);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<unsigned char>(generation * 53 + blob * 17 + i * 7 + 1);
    return bytes;
}
std::vector<x2w_save::Action> generation(unsigned number)
{
    return {x2w_save::Action{false, "a.dat", bytes_of(number, 0)}, x2w_save::Action{false, "b.dat", bytes_of(number, 1)}};
}

// Reader process: one read of both blobs; the exit code says which generation it saw.
int reader(const char* root, const char* dir, const char* timeout_ms)
{
    touch(fs::path(dir) / (std::string("started-") + timeout_ms));
    const x2w_save::Store store(root, std::chrono::milliseconds(std::atoi(timeout_ms)));
    std::vector<std::vector<unsigned char>> blobs;
    const Status status = store.read_blobs("slot-p", {"a.dat", "b.dat"}, blobs);
    std::fprintf(stderr, "reader (timeout %s ms): status %s\n", timeout_ms, status_name(status));
    if (status == Status::busy) return exit_busy;
    if (status == Status::not_found) return exit_mixed;  // a blob of the generation is missing
    if (status != Status::ok) return exit_other;
    if (blobs[0] == bytes_of(1, 0) && blobs[1] == bytes_of(1, 1)) return exit_old;
    if (blobs[0] == bytes_of(2, 0) && blobs[1] == bytes_of(2, 1)) return exit_new;
    return exit_mixed;
}

// Writer process: commits generation 2 and stops at the chosen point of the publication until the
// parent creates "go".
int writer(const char* root, const char* dir, const char* point)
{
    pause_dir = dir;
    pause_point = static_cast<unsigned>(std::atoi(point));
    return x2w_save::Store(root).commit("slot-p", generation(2)) == Status::ok ? 0 : 1;
}

// Holder process: takes the exclusive lock of the container, then ends without releasing it.
int holder(const char* root, const char* dir)
{
    x2w_save::FileLock lock;
    if (lock.acquire(fs::path(root) / ("slot-p" + std::string(x2w_save::lock_suffix)), true, std::chrono::seconds(10)) != x2w_save::LockResult::locked) return 1;
    touch(fs::path(dir) / "held");
    if (!wait_for(fs::path(dir) / "release")) return 2;
    std::_Exit(0);  // no unlock, no destructors: the system must free the lock
}

int run_child(const std::string& self, const std::string& arguments)
{
    std::string command = "\"" + self + "\" " + arguments;
#ifdef _WIN32
    command = "\"" + command + "\"";  // cmd.exe drops the outer quotes
#endif
    const int code = std::system(command.c_str());
#ifdef _WIN32
    return code;
#else
    return code == -1 ? -1 : (code >> 8) & 0xff;
#endif
}
std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

int failures;
void expect(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
fs::path make_case(const fs::path& base, const char* name)
{
    const fs::path dir = base / name;
    fs::create_directories(dir / "sync");
    return dir;
}

int read_code(const std::string& self, const fs::path& dir, int timeout_ms)
{
    return run_child(self, "read " + quoted(dir / "store") + " " + quoted(dir / "sync") + " " + std::to_string(timeout_ms));
}

// The writer stops at `point` of the publication of a two-blob update. A reader that cannot wait
// gives up with busy; a reader that waits sees the whole new generation once the writer is done.
// point 0: the first old blob is moved aside; 1: the first new blob is in; 2: the second old blob
// is moved aside (it is missing on disk); 3: the second new blob is in.
void paused_publication(const std::string& self, const fs::path& base, unsigned point)
{
    const std::string label = "pause point " + std::to_string(point);
    const fs::path dir = make_case(base, ("point-" + std::to_string(point)).c_str());
    const x2w_save::Store store(dir / "store");
    expect(store.create_container("slot-p") == Status::ok && store.commit("slot-p", generation(1)) == Status::ok, "fixture: generation 1");
    expect(read_code(self, dir, 2000) == exit_old, "fixture: a second process reads generation 1");

    int writer_code = -1, waiting_reader = -1;
    std::thread write_thread([&] { writer_code = run_child(self, "write " + quoted(dir / "store") + " " + quoted(dir / "sync") + " " + std::to_string(point)); });
    const bool paused = wait_for(dir / "sync" / "paused");
    expect(paused, "the writer reached its pause point");
    if (paused) {
        // While the publication is half done another process must not see half of it.
        const int during = read_code(self, dir, 150);
        if (during == exit_mixed) std::fprintf(stderr, "FAIL: reader saw a mixed generation or a missing blob at %s\n", label.c_str());
        expect(during == exit_busy, (label + ": a reader that cannot wait is refused, not shown a partial state (reader exit code " + std::to_string(during) + ", expected " + std::to_string(exit_busy) + ": 20 old, 21 new, 22 mixed, 23 busy, 24 other status)").c_str());
        std::thread read_thread([&] { waiting_reader = read_code(self, dir, 20000); });
        expect(wait_for(dir / "sync" / "started-20000"), "the waiting reader started");
        touch(dir / "sync" / "go");
        read_thread.join();
    } else {
        touch(dir / "sync" / "go");
    }
    write_thread.join();
    expect(writer_code == 0, "the writer commits");
    expect(waiting_reader == exit_new, (label + ": a reader that waited sees the whole new generation").c_str());
    expect(read_code(self, dir, 2000) == exit_new, "after the publication every reader sees generation 2");
    std::size_t entries = 0;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(dir / "store" / "slot-p", ec)) { (void)item; ++entries; }
    expect(entries == 2, "no staging or backup file is left");
}

// The second operation of an update fails: generation 1 stays whole for a reader in another process,
// and the lock is free again for the next update.
void second_operation_fails(const std::string& self, const fs::path& base)
{
    const fs::path dir = make_case(base, "second-fails");
    const x2w_save::Store store(dir / "store");
    expect(store.create_container("slot-p") == Status::ok && store.commit("slot-p", generation(1)) == Status::ok, "fixture: generation 1");
    std::vector<x2w_save::Action> failing = generation(2);
    failing.resize(1);
    failing.push_back(x2w_save::Action{true, "absent.dat", {}});
    expect(store.commit("slot-p", failing) == Status::not_found, "the second operation fails the update");
    expect(read_code(self, dir, 2000) == exit_old, "a failed update leaves generation 1 whole for another process");
    expect(store.commit("slot-p", generation(2)) == Status::ok && read_code(self, dir, 2000) == exit_new, "the next update still publishes");
}

// The publication lock is held by another process: a bounded wait ends in busy and changes nothing;
// when that process ends without unlocking, the system frees the lock and the store works again.
void holder_that_dies(const std::string& self, const fs::path& base)
{
    const fs::path dir = make_case(base, "holder");
    const x2w_save::Store store(dir / "store", std::chrono::milliseconds(150));
    expect(store.create_container("slot-p") == Status::ok && store.commit("slot-p", generation(1)) == Status::ok, "fixture: generation 1");
    int holder_code = -1;
    std::thread hold_thread([&] { holder_code = run_child(self, "hold " + quoted(dir / "store") + " " + quoted(dir / "sync")); });
    if (wait_for(dir / "sync" / "held")) {
        std::vector<unsigned char> bytes;
        const Status commit = store.commit("slot-p", generation(2));
        const Status read = store.read_blob("slot-p", "a.dat", bytes);
        const Status del = store.delete_blob("slot-p", "a.dat");
        const std::string got = std::string(" (got ") + status_name(commit) + "/" + status_name(read) + "/" + status_name(del) + " for commit/read/delete)";
        expect(commit == Status::busy, ("a commit gives up with busy while the lock is held elsewhere" + got).c_str());
        expect(read == Status::busy, ("a read gives up with busy while the lock is held elsewhere" + got).c_str());
        expect(del == Status::busy, ("a delete gives up with busy while the lock is held elsewhere" + got).c_str());
        // The same attempt on the bare lock: the result and the system error code say why.
        x2w_save::FileLock probe;
        const x2w_save::LockResult direct = probe.acquire(dir / "store" / ("slot-p" + std::string(x2w_save::lock_suffix)), false, std::chrono::milliseconds(0));
        expect(direct == x2w_save::LockResult::timeout,
               ("a lock request that cannot wait ends in timeout (LockResult " + std::to_string(static_cast<int>(direct)) + ", system error " + std::to_string(probe.last_error()) + ")").c_str());
    } else {
        expect(false, "the holder took the lock");
    }
    touch(dir / "sync" / "release");
    hold_thread.join();
    expect(holder_code == 0, "the holder ended without unlocking");
    expect(store.commit("slot-p", generation(2)) == Status::ok && read_code(self, dir, 2000) == exit_new, "the store works once the dead holder's lock is gone");
}
}  // namespace

// Called by the generated copy of save_store.h at each step of a publication (see CMakeLists.txt).
void x2w_save_test_point(unsigned point)
{
    if (pause_dir.empty() || point != pause_point) return;
    touch(pause_dir / "paused");
    if (!wait_for(pause_dir / "go")) std::_Exit(3);
}

int main(int argc, char** argv)
{
    if (argc == 5 && std::strcmp(argv[1], "read") == 0) return reader(argv[2], argv[3], argv[4]);
    if (argc == 5 && std::strcmp(argv[1], "write") == 0) return writer(argv[2], argv[3], argv[4]);
    if (argc == 4 && std::strcmp(argv[1], "hold") == 0) return holder(argv[2], argv[3]);
    if (argc != 1) return 2;

    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / ("x2w-gamesave-publish-" + unique_suffix());
    fs::remove_all(base, ec);
    const std::string self = argv[0];
    for (unsigned point = 0; point < 4; ++point) paused_publication(self, base, point);
    second_operation_fails(self, base);
    holder_that_dies(self, base);
    fs::remove_all(base, ec);
    if (failures) return 1;
    std::puts("startup_gamesave_publish: ok");
    return 0;
}
