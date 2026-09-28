// mbedTLS allocations PSRAM-first (what CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC would do), via
// -Wl,--wrap=esp_mbedtls_mem_calloc: the prebuilt libs allocate internal-only, and HTTPS needs ~40 KB.
#include "configuration.h"

#if defined(ARCH_ESP32) && defined(MESHTASTIC_MBEDTLS_PSRAM)
#include <esp_heap_caps.h>

extern "C" __attribute__((used)) void *__wrap_esp_mbedtls_mem_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
#endif
