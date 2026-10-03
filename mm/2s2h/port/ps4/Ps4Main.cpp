// PS4 process entry point.

#ifdef __PS4__

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/SystemService.h>
#include <orbis/libkernel.h>

#include <spdlog/spdlog.h>

#include <exception>
#include <stdexcept>
#include <string>

extern "C" int ship_main(int argc, char* argv[]);
// Not declared by the OpenOrbis headers.
extern "C" int sceKernelInstallExceptionHandler(int signalNumber, void (*handler)(int, void*));
extern "C" int sceKernelRemoveExceptionHandler(int signalNumber);
// From the OpenOrbis linker script.
extern "C" char __text_start;
extern "C" char __eh_frame_start;
extern "C" void Ps4Heap_GetStats(size_t* arenaSize, size_t* inUse, size_t* peak);

namespace {
// Everything the game reads and writes (ROM archives, saves, config, mods, logs) lives here.
constexpr const char* kDataDirectory = "/data/2ship";
// The decompiled game code, the randomizer and std::regex recurse deeper than the default
// main thread stack is comfortable with.
constexpr size_t kGameThreadStackSize = 16 * 1024 * 1024;

// The game only opens its log file after the window exists. Until then (and for anything that
// goes wrong inside the graphics setup) stdout/stderr are the only trace, so they are sent here.
constexpr const char* kBootLog = "/data/2ship/ps4_boot.log";
int sBootLogFd = -1;

void BootLogRaw(const char* text) {
    if (sBootLogFd >= 0) {
        write(sBootLogFd, text, strlen(text));
        fsync(sBootLogFd);
    }
}

void FatalSignalHandler(int signalNumber, siginfo_t* info, void*) {
    char line[160];
    snprintf(line, sizeof(line), "[PS4] FATAL: signal %d, fault address %p\n", signalNumber,
             info != nullptr ? info->si_addr : nullptr);
    BootLogRaw(line);
    signal(signalNumber, SIG_DFL);
    raise(signalNumber);
}

// Faults don't reach POSIX signal handlers on this system; the kernel's own exception handler
// API does. It gets a FreeBSD amd64 ucontext. Everything is logged as an offset from the start of
// the executable's code, which tools/symbolize turns back into functions with the matching ELF.
constexpr int kMcontextOffset = 16; // after uc_sigmask
constexpr int kRipOffset = kMcontextOffset + 160;
constexpr int kRspOffset = kMcontextOffset + 184;
constexpr int kRbpOffset = kMcontextOffset + 72;
constexpr int kAddrOffset = kMcontextOffset + 136;

uintptr_t CodeOffset(uintptr_t address) {
    const uintptr_t start = (uintptr_t)&__text_start;
    const uintptr_t end = (uintptr_t)&__eh_frame_start;
    return (address >= start && address < end) ? address - start : 0;
}

void LogStackCodeAddresses(const uintptr_t* from, const char* what) {
    char line[160];
    snprintf(line, sizeof(line), "[PS4] code addresses on the %s stack (offsets):", what);
    BootLogRaw(line);
    int found = 0;
    for (int i = 0; i < 8192 && found < 48; i++) {
        const uintptr_t offset = CodeOffset(from[i]);
        if (offset != 0) {
            snprintf(line, sizeof(line), " %lx", (unsigned long)offset);
            BootLogRaw(line);
            found++;
        }
    }
    BootLogRaw("\n");
}

void KernelExceptionHandler(int signalNumber, void* context) {
    char line[256];
    const uint8_t* ctx = static_cast<const uint8_t*>(context);
    uintptr_t rip = 0;
    uintptr_t rsp = 0;
    uintptr_t rbp = 0;
    uintptr_t addr = 0;
    if (ctx != nullptr) {
        memcpy(&rip, ctx + kRipOffset, sizeof(rip));
        memcpy(&rsp, ctx + kRspOffset, sizeof(rsp));
        memcpy(&rbp, ctx + kRbpOffset, sizeof(rbp));
        memcpy(&addr, ctx + kAddrOffset, sizeof(addr));
    }
    snprintf(line, sizeof(line),
             "[PS4] FATAL: exception %d at rip %p (code offset %lx), fault address %p, rsp %p, rbp %p, text "
             "start %p\n",
             signalNumber, (void*)rip, (unsigned long)CodeOffset(rip), (void*)addr, (void*)rsp, (void*)rbp,
             (void*)&__text_start);
    BootLogRaw(line);
    if (rsp != 0 && (rsp & 7) == 0) {
        LogStackCodeAddresses(reinterpret_cast<const uintptr_t*>(rsp), "faulting");
    }
    uintptr_t here = 0;
    LogStackCodeAddresses(&here, "handler");
    BootLogRaw("[PS4] end of crash report\n");
    // Let the fault happen again without us, so the system ends the process as usual.
    sceKernelRemoveExceptionHandler(signalNumber);
}

void TerminateHandler() {
    char line[512];
    const char* what = "unknown";
    std::string message;
    if (std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            message = e.what();
            what = message.c_str();
        } catch (...) {
        }
    } else {
        what = "std::terminate called without an active exception";
    }
    snprintf(line, sizeof(line), "[PS4] FATAL: uncaught C++ exception: %s\n", what);
    BootLogRaw(line);
    uintptr_t here = 0;
    LogStackCodeAddresses(&here, "terminate");
    abort();
}

// Exceptions go through libunwind's bare-metal mode here; check once that they actually work.
void CheckExceptions() {
    bool caught = false;
    try {
        throw std::runtime_error("self test");
    } catch (const std::exception& e) {
        caught = strcmp(e.what(), "self test") == 0;
    }
    BootLogRaw(caught ? "[PS4] C++ exceptions: ok\n" : "[PS4] C++ exceptions: BROKEN\n");
}

void InstallBootLog() {
    sBootLogFd = open(kBootLog, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (sBootLogFd >= 0) {
        // Same descriptor for both streams, so the order of the lines is preserved.
        dup2(sBootLogFd, STDOUT_FILENO);
        dup2(sBootLogFd, STDERR_FILENO);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::trace);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    // The OpenOrbis header misspells its own sa_sigaction macro, go through the union.
    action.__sa_handler.__sa_sigaction = FatalSignalHandler;
    action.sa_flags = SA_SIGINFO;
    for (int signalNumber : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT }) {
        sigaction(signalNumber, &action, nullptr);
    }
    for (int signalNumber : { SIGSEGV, SIGBUS, SIGILL, SIGFPE }) {
        const int result = sceKernelInstallExceptionHandler(signalNumber, KernelExceptionHandler);
        if (result < 0) {
            char line[96];
            snprintf(line, sizeof(line), "[PS4] sceKernelInstallExceptionHandler(%d) failed: 0x%08X\n", signalNumber,
                     (unsigned int)result);
            BootLogRaw(line);
        }
    }
    std::set_terminate(TerminateHandler);
}

// stat() used to report garbage sizes with the stock OpenOrbis headers (see ps4_fixups.h in the
// build scripts). Compare it against lseek() on a file that is always there, so the log says
// whether file sizes can be trusted on this console.
void CheckFileSizes() {
    const char* path = "/app0/2ship.o2r";
    struct stat info;
    memset(&info, 0, sizeof(info));
    const int statResult = stat(path, &info);
    long long seekSize = -1;
    const int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        seekSize = (long long)lseek(fd, 0, SEEK_END);
        close(fd);
    }
    char line[200];
    snprintf(line, sizeof(line), "[PS4] file size check on %s: stat %s, st_size %lld, lseek %lld -> %s\n", path,
             statResult == 0 ? "ok" : "failed", (long long)info.st_size, seekSize,
             (statResult == 0 && (long long)info.st_size == seekSize) ? "consistent" : "MISMATCH");
    BootLogRaw(line);
}

void* GameThread(void*) {
    BootLogRaw("[PS4] game thread started\n");
    static char arg0[] = "2ship";
    char* argv[] = { arg0, nullptr };
    ship_main(1, argv);
    BootLogRaw("[PS4] ship_main returned\n");
    return nullptr;
}

void ReturnToSystem() {
    size_t arenaSize = 0;
    size_t inUse = 0;
    size_t peak = 0;
    Ps4Heap_GetStats(&arenaSize, &inUse, &peak);
    printf("[PS4] heap arena %zu MiB, in use %zu MiB, peak %zu MiB\n", arenaSize >> 20, inUse >> 20, peak >> 20);
    fflush(stdout);

    // The orderly way for an application to quit back to the home screen.
    sceSystemServiceLoadExec("exit", nullptr);
}
} // namespace

int main(int argc, char* argv[]) {
    if (mkdir(kDataDirectory, 0777) != 0 && errno != EEXIST) {
        printf("[PS4] could not create %s: %s\n", kDataDirectory, strerror(errno));
    }
    if (chdir(kDataDirectory) != 0) {
        printf("[PS4] could not change directory to %s: %s\n", kDataDirectory, strerror(errno));
    }

    InstallBootLog();
    BootLogRaw("[PS4] 2 Ship 2 Harkinian boot\n");
    CheckFileSizes();
    CheckExceptions();

    atexit(ReturnToSystem);

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kGameThreadStackSize);

    pthread_t thread;
    const int result = pthread_create(&thread, &attributes, GameThread, nullptr);
    pthread_attr_destroy(&attributes);

    if (result != 0) {
        printf("[PS4] could not create the game thread (%d), running on the main thread\n", result);
        GameThread(nullptr);
    } else {
        pthread_join(thread, nullptr);
    }

    return 0;
}

#endif // __PS4__
