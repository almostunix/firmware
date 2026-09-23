// mbedTLS allocations PSRAM-first, as ESP-IDF's CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC would do. The prebuilt Arduino libs
// allocate internal-only, and HTTPS alone needs ~40 KB that WiFi + Tailscale can't spare. Wired in by
// -Wl,--wrap=esp_mbedtls_mem_calloc; frees still go through esp_mbedtls_mem_free, which handles any heap.
#include "configuration.h"

#if defined(ARCH_ESP32) && defined(MESHTASTIC_MBEDTLS_PSRAM)
#include <esp_heap_caps.h>

extern "C" __attribute__((used)) void *__wrap_esp_mbedtls_mem_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
#endif
