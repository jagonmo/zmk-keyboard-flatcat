/*
 * Advertising-data parsing for the BLE bridge.
 *
 * Only <stdint.h>/<string.h>, so it can be unit-tested on the host.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BRIDGE_AD_UUID16_SOME 0x02
#define BRIDGE_AD_UUID16_ALL  0x03
#define BRIDGE_AD_NAME_SHORT  0x08
#define BRIDGE_AD_NAME_FULL   0x09

#define BRIDGE_UUID_HIDS 0x1812

struct bridge_ad_info {
    bool hid;      /* lists the HID service (0x1812) */
    bool name_ok;  /* name starts with the wanted prefix (or no prefix wanted) */
};

/* Walk the AD structures of one advertisement. Malformed data stops the walk. */
static inline struct bridge_ad_info bridge_ad_scan(const uint8_t *ad, size_t len,
                                                   const char *name_prefix) {
    struct bridge_ad_info info = {.hid = false, .name_ok = false};
    size_t plen = name_prefix ? strlen(name_prefix) : 0;

    if (plen == 0) {
        info.name_ok = true;
    }

    size_t i = 0;
    while (i + 1 < len) {
        size_t l = ad[i];
        if (l == 0 || i + 1 + l > len) {
            break;
        }
        uint8_t type = ad[i + 1];
        const uint8_t *data = &ad[i + 2];
        size_t dlen = l - 1;

        switch (type) {
        case BRIDGE_AD_UUID16_SOME:
        case BRIDGE_AD_UUID16_ALL:
            for (size_t j = 0; j + 1 < dlen; j += 2) {
                uint16_t uuid = (uint16_t)data[j] | ((uint16_t)data[j + 1] << 8);
                if (uuid == BRIDGE_UUID_HIDS) {
                    info.hid = true;
                }
            }
            break;
        case BRIDGE_AD_NAME_SHORT:
        case BRIDGE_AD_NAME_FULL:
            if (plen > 0 && dlen >= plen && memcmp(data, name_prefix, plen) == 0) {
                info.name_ok = true;
            }
            break;
        default:
            break;
        }

        i += 1 + l;
    }

    return info;
}
