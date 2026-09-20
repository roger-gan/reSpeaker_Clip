/* App-image replacement for NCS's fixed-buffer TLS heap initializer. */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <mbedtls/platform.h>

static int clip_tls_heap_init(void)
{
    return mbedtls_platform_set_calloc_free(k_calloc, k_free);
}

/* Same ordering as the replaced SDK initializer, before crypto/network users. */
SYS_INIT(clip_tls_heap_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
