// Experimental (docs/startup-runtime.md): a cross-process lock on one small file, used by the
// game-save store so that readers and the publication of an update exclude each other. Windows:
// LockFileEx on byte 0 of the file; elsewhere flock. The system releases the lock when the holder's
// handle closes, so a process that died while holding it never blocks the others for good. A wait
// is always bounded: acquire gives up with LockResult::timeout.
#pragma once

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <thread>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace x2w_save {

enum class LockResult { locked, timeout, error };

// Win32 codes (winerror.h values, repeated here so the mapping builds and is tested on every platform).
constexpr std::uint32_t win_error_sharing_violation = 32;
constexpr std::uint32_t win_error_lock_violation = 33;
constexpr std::uint32_t win_error_io_pending = 997;

// Whether a failed LockFileEx(LOCKFILE_FAIL_IMMEDIATELY) means "another holder has it, try again
// later" rather than a real failure. ERROR_LOCK_VIOLATION is the documented answer; a sharing
// violation or ERROR_IO_PENDING (the request was not granted at once) mean the same thing and must
// not be reported as an I/O error.
constexpr bool win_lock_error_is_busy(std::uint32_t code)
{
    return code == win_error_lock_violation || code == win_error_sharing_violation || code == win_error_io_pending;
}

class FileLock {
public:
    FileLock() = default;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock() { release(); }

    // Opens (creating it when needed) and locks the file, shared or exclusive, trying again every
    // few milliseconds until the timeout ends. The directory of the file must exist.
    LockResult acquire(const std::filesystem::path& file, bool exclusive, std::chrono::milliseconds timeout)
    {
        release();
        last_error_ = 0;
        if (!open(file)) return LockResult::error;
        const auto deadline = deadline_after(std::chrono::steady_clock::now(), timeout);
        for (int pause = 1;; pause = pause < 16 ? pause * 2 : pause) {
            last_error_ = 0;
            const Attempt attempt = try_lock_once(exclusive);
            if (attempt == Attempt::locked) return LockResult::locked;
            if (attempt == Attempt::error) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                release();
                return LockResult::timeout;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(pause));
        }
        release();
        return LockResult::error;
    }

    // The system error (GetLastError, or errno) of the last failed open or lock attempt; 0 when none
    // failed. Kept for diagnostics: it says why a lock ended in busy or error.
    std::uint32_t last_error() const noexcept { return last_error_; }

    void release() noexcept
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);  // closing the handle drops the lock
        handle_ = INVALID_HANDLE_VALUE;
#else
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
#endif
    }

private:
    enum class Attempt { locked, busy, error };

    // now + timeout without overflow: a timeout too large for the clock saturates at the latest time
    // point (wait for ever); zero or negative means one attempt only.
    static std::chrono::steady_clock::time_point deadline_after(std::chrono::steady_clock::time_point now,
                                                                std::chrono::milliseconds timeout)
    {
        using Clock = std::chrono::steady_clock;
        if (timeout <= std::chrono::milliseconds::zero()) return now;
        const auto max_step = std::chrono::duration_cast<std::chrono::milliseconds>((Clock::duration::max)());
        while (now.time_since_epoch() < Clock::duration::zero()) {
            if (timeout <= max_step) return now + timeout;
            now += max_step;
            timeout -= max_step;
        }
        const auto room = std::chrono::duration_cast<std::chrono::milliseconds>((Clock::time_point::max)() - now);
        if (timeout >= room) return (Clock::time_point::max)();
        return now + timeout;
    }

    bool open(const std::filesystem::path& file)
    {
#ifdef _WIN32
        handle_ = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE) return true;
        last_error_ = GetLastError();
        return false;
#else
        fd_ = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ >= 0) return true;
        last_error_ = static_cast<std::uint32_t>(errno);
        return false;
#endif
    }

    Attempt try_lock_once(bool exclusive)
    {
        // x2w-seam: lock-attempt
#ifdef _WIN32
        OVERLAPPED where{};
        const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
        if (LockFileEx(handle_, flags, 0, 1, 0, &where)) return Attempt::locked;
        last_error_ = GetLastError();
        if (!win_lock_error_is_busy(last_error_)) return Attempt::error;
        if (last_error_ == win_error_io_pending) {
            // The request may still be queued: withdraw it and wait for that to finish, because
            // `where` is about to go out of scope.
            CancelIoEx(handle_, &where);
            DWORD ignored = 0;
            GetOverlappedResult(handle_, &where, &ignored, TRUE);
        }
        return Attempt::busy;
#else
        if (flock(fd_, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0) return Attempt::locked;
        last_error_ = static_cast<std::uint32_t>(errno);
        return errno == EWOULDBLOCK ? Attempt::busy : Attempt::error;
#endif
    }

    std::uint32_t last_error_ = 0;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

}  // namespace x2w_save
