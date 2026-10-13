/* SPDX-License-Identifier: Apache-2.0 */
#include "auth.h"
#include <string.h>
#include "mbedtls/md.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

/* PBKDF2 block 1, SHA-256, 32-byte key. Matches Python pbkdf2_hmac, 200000 iterations. */
bool tg_password_hash(const char *password, const unsigned char *salt, unsigned char *out) {
    unsigned char seed[20], u[32] = {0}; memcpy(seed, salt, 16); seed[16] = seed[17] = seed[18] = 0; seed[19] = 1;
    mbedtls_md_context_t context; mbedtls_md_init(&context);
    int rc = mbedtls_md_setup(&context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    if (!rc) rc = mbedtls_md_hmac_starts(&context, (const unsigned char *)password, strlen(password));
    if (!rc) rc = mbedtls_md_hmac_update(&context, seed, sizeof(seed));
    if (!rc) rc = mbedtls_md_hmac_finish(&context, u);
    memcpy(out, u, 32);
    for (unsigned i = 1; !rc && i < 200000; i++) {
        rc = mbedtls_md_hmac_reset(&context);
        if (!rc) rc = mbedtls_md_hmac_update(&context, u, sizeof(u));
        if (!rc) rc = mbedtls_md_hmac_finish(&context, u);
        for (int j = 0; j < 32; j++) out[j] ^= u[j];
#ifdef ESP_PLATFORM
        if (!(i % 512)) vTaskDelay(1);
#endif
    }
    memset(u, 0, sizeof(u)); mbedtls_md_free(&context); return rc == 0;
}
