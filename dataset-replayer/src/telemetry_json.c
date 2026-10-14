#define _POSIX_C_SOURCE 200809L
#include "telemetry_json.h"

#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Each 63-byte text expands to at most 378 bytes plus quotes. The two
 * escaped strings, keys, punctuation, states, integers and three scientific
 * double representations fit well within 2048 bytes on the Linux target.
 * Every formatting operation is checked even if these bounds change.
 */
#define JSON_CAPACITY 2048

static int escape_text(const char *text, size_t capacity, char *out)
{
    const unsigned char *s = (const unsigned char *)text;
    const unsigned char *end = memchr(text, '\0', capacity);
    static const char hex[] = "0123456789abcdef";
    char *p = out;

    if (end == NULL) {
        return -1;
    }
    *p++ = '"';
    while (s < end) {
        unsigned char c = *s;
        if (c >= 0x80) {
            size_t n;
            if (c >= 0xc2 && c <= 0xdf) {
                n = 2;
            } else if (c >= 0xe0 && c <= 0xef) {
                n = 3;
            } else if (c >= 0xf0 && c <= 0xf4) {
                n = 4;
            } else {
                return -1;
            }
            if ((size_t)(end - s) < n) {
                return -1;
            }
            for (size_t i = 1; i < n; ++i) {
                if (s[i] < 0x80 || s[i] > 0xbf) {
                    return -1;
                }
            }
            /* Exclude overlong encodings, surrogates and values > U+10FFFF. */
            if ((c == 0xe0 && s[1] < 0xa0) ||
                (c == 0xed && s[1] >= 0xa0) ||
                (c == 0xf0 && s[1] < 0x90) ||
                (c == 0xf4 && s[1] >= 0x90)) {
                return -1;
            }
            memcpy(p, s, n);
            p += n;
            s += n;
            continue;
        }
        if (c == '"' || c == '\\') {
            *p++ = '\\';
            *p++ = (char)c;
        } else if (c < 0x20) {
            *p++ = '\\';
            *p++ = 'u';
            *p++ = '0';
            *p++ = '0';
            *p++ = hex[c >> 4];
            *p++ = hex[c & 15];
        } else {
            *p++ = (char)c;
        }
        ++s;
    }
    *p++ = '"';
    *p = '\0';
    return 0;
}

static int format_number(double value, char *buffer, size_t capacity)
{
    int length = isfinite(value)
        ? snprintf(buffer, capacity, "%.*g", DBL_DECIMAL_DIG, value)
        : snprintf(buffer, capacity, "null");
    return length < 0 || (size_t)length >= capacity ? -1 : 0;
}

int telemetry_to_json(const Telemetry *telemetry, char *buffer, size_t capacity)
{
    char machine_type[6 * (sizeof telemetry->machine_type - 1) + 3];
    char timestamp[6 * (sizeof telemetry->timestamp - 1) + 3];
    char temperature[DBL_DECIMAL_DIG + 32];
    char current[DBL_DECIMAL_DIG + 32];
    char power[DBL_DECIMAL_DIG + 32];
    char json[JSON_CAPACITY];
    const char *state;
    locale_t numeric_locale;
    locale_t previous;
    int result;
    int length;

    if (telemetry == NULL || buffer == NULL || capacity == 0 ||
        (telemetry->failure_within_24h != 0 && telemetry->failure_within_24h != 1)) {
        return -1;
    }
    if (escape_text(telemetry->machine_type, sizeof telemetry->machine_type,
                    machine_type) != 0 ||
        escape_text(telemetry->timestamp, sizeof telemetry->timestamp, timestamp) != 0) {
        return -1;
    }
    switch (telemetry->state) {
    case TELEMETRY_STATE_IDLE: state = "IDLE"; break;
    case TELEMETRY_STATE_NORMAL: state = "NORMAL"; break;
    case TELEMETRY_STATE_PEAK: state = "PEAK"; break;
    default: state = "UNKNOWN"; break;
    }

    numeric_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (numeric_locale == (locale_t)0) {
        return -1;
    }
    previous = uselocale(numeric_locale);
    if (previous == (locale_t)0) {
        freelocale(numeric_locale);
        return -1;
    }
    result = format_number(telemetry->temperature, temperature, sizeof temperature);
    if (format_number(telemetry->current, current, sizeof current) != 0 ||
        format_number(telemetry->power, power, sizeof power) != 0) {
        result = -1;
    }
    if (uselocale(previous) == (locale_t)0) {
        /* Do not free a locale that might still be active. */
        return -1;
    }
    freelocale(numeric_locale);
    if (result != 0) {
        return -1;
    }
    length = snprintf(json, sizeof json,
        "{\"machine_id\":%d,\"machine_type\":%s,\"timestamp\":%s,"
        "\"temperature\":%s,\"current\":%s,\"power\":%s,"
        "\"state\":\"%s\",\"failure_within_24h\":%d}",
        telemetry->machine_id, machine_type, timestamp, temperature, current,
        power, state, telemetry->failure_within_24h);
    if (length < 0 || (size_t)length >= sizeof json || (size_t)length >= capacity) {
        return -1;
    }
    memcpy(buffer, json, (size_t)length + 1);
    return 0;
}
