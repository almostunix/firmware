// The shim's shared plumbing: deferred logging and thread setup.
#include "TailscaleInternal.h"

#ifdef MESHTASTIC_TAILSCALE

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef ARCH_ESP32
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_rom_sys.h>
#endif

namespace tailscale_shim
{

namespace
{
std::mutex gLogMutex;
// A vector, not a deque: libstdc++'s empty deque allocates ~544 bytes, and this is drained every 100 ms.
std::vector<std::pair<LogLevel, std::string>> gLogQueue;
/// Bound the queue: a wedged scheduler must not let workers grow it forever.
constexpr size_t TS_LOG_QUEUE_LIMIT = 64;
} // namespace

void deferLog(LogLevel level, const char *format, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lock(gLogMutex);
    if (gLogQueue.size() >= TS_LOG_QUEUE_LIMIT)
        return; // drop the newest rather than grow without bound
    gLogQueue.emplace_back(level, buf);
}

void drainDeferredLogs()
{
    std::vector<std::pair<LogLevel, std::string>> batch;
    {
        std::lock_guard<std::mutex> lock(gLogMutex);
        if (gLogQueue.empty())
            return;
        batch.swap(gLogQueue);
    }

    for (const auto &entry : batch) {
        switch (entry.first) {
        case LogLevel::Info:
            LOG_INFO("%s", entry.second.c_str());
            break;
        case LogLevel::Warn:
            LOG_WARN("%s", entry.second.c_str());
            break;
        case LogLevel::Error:
            LOG_ERROR("%s", entry.second.c_str());
            break;
        }
    }
}

// Loop-task priority: at pthreads' default of 5 a busy tokio worker starved Meshtastic into the 90 s watchdog.
#ifdef ARCH_ESP32
constexpr size_t TS_THREAD_PRIORITY = 1; // esp_pthread_cfg_t::prio is a size_t
#endif

void logInternalHeap(const char *when)
{
#if defined(ARCH_ESP32) && defined(MESHTASTIC_TAILSCALE_DIAGNOSTICS)
    esp_rom_printf("Tailscale: heap %s: internal %u free (%u largest), psram %u free\n", when,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    (void)when;
#endif
}

void setThreadStack(size_t bytes, const char *name, StackMem where)
{
#ifdef ARCH_ESP32
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = bytes;
    cfg.thread_name = name;
    cfg.prio = TS_THREAD_PRIORITY;
    // MALLOC_CAP_8BIT is required either way: a task stack must be byte-accessible.
    cfg.stack_alloc_caps =
        (where == StackMem::Psram) ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    cfg.inherit_cfg = true;
    esp_pthread_set_cfg(&cfg);
#else
    (void)bytes;
    (void)name;
    (void)where;
#endif
}

void restoreThreadStackDefaults()
{
#ifdef ARCH_ESP32
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&cfg);
#endif
}

} // namespace tailscale_shim

#endif // MESHTASTIC_TAILSCALE
