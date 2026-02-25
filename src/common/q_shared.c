/*
 * q_shared.c - Shared utility functions
 *
 * Based on Quake II q_shared.c (id Software GPL).
 * String utilities, vector math, info string parsing, random numbers.
 *
 * SoF exports: VectorNormalize, VectorLength, Com_sprintf, flrand, irand
 */

#include "q_shared.h"

/* ==========================================================================
   Vector Math
   ========================================================================== */

vec_t VectorLength(vec3_t v)
{
    float length = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    return (vec_t)sqrt(length);
}

vec_t VectorNormalize(vec3_t v)
{
    float length = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    length = (float)sqrt(length);

    if (length) {
        float ilength = 1.0f / length;
        v[0] *= ilength;
        v[1] *= ilength;
        v[2] *= ilength;
    }

    return length;
}

void CrossProduct(vec3_t v1, vec3_t v2, vec3_t cross)
{
    cross[0] = v1[1] * v2[2] - v1[2] * v2[1];
    cross[1] = v1[2] * v2[0] - v1[0] * v2[2];
    cross[2] = v1[0] * v2[1] - v1[1] * v2[0];
}

/* ==========================================================================
   Random Numbers (SoF-specific exports)
   ========================================================================== */

float flrand(float min, float max)
{
    return min + (max - min) * ((float)rand() / (float)RAND_MAX);
}

int irand(int min, int max)
{
    if (min == max)
        return min;
    return min + (rand() % (max - min + 1));
}

/* ==========================================================================
   String Utilities
   ========================================================================== */

int Q_stricmp(const char *s1, const char *s2)
{
    int c1, c2;

    do {
        c1 = *s1++;
        c2 = *s2++;

        if (c1 != c2) {
            if (c1 >= 'a' && c1 <= 'z') c1 -= ('a' - 'A');
            if (c2 >= 'a' && c2 <= 'z') c2 -= ('a' - 'A');
            if (c1 != c2)
                return c1 < c2 ? -1 : 1;
        }
    } while (c1);

    return 0;
}

int Q_strncasecmp(const char *s1, const char *s2, int n)
{
    int c1, c2;

    do {
        c1 = *s1++;
        c2 = *s2++;

        if (!n--)
            return 0;

        if (c1 != c2) {
            if (c1 >= 'a' && c1 <= 'z') c1 -= ('a' - 'A');
            if (c2 >= 'a' && c2 <= 'z') c2 -= ('a' - 'A');
            if (c1 != c2)
                return c1 < c2 ? -1 : 1;
        }
    } while (c1);

    return 0;
}

void Q_strncpyz(char *dest, const char *src, int destsize)
{
    if (!dest || !src || destsize < 1)
        return;
    strncpy(dest, src, destsize - 1);
    dest[destsize - 1] = 0;
}

void Com_sprintf(char *dest, int size, const char *fmt, ...)
{
    va_list argptr;

    va_start(argptr, fmt);
    vsnprintf(dest, size, fmt, argptr);
    va_end(argptr);

    dest[size - 1] = 0;
}

/*
 * va() - Returns a static buffer with formatted string.
 * NOT thread-safe. Uses rotating buffers to allow nested calls.
 */
#define VA_BUFFERS  4
#define VA_BUFSIZE  2048

char *va(const char *format, ...)
{
    static char strings[VA_BUFFERS][VA_BUFSIZE];
    static int  index = 0;
    char        *buf;
    va_list     argptr;

    buf = strings[index];
    index = (index + 1) % VA_BUFFERS;

    va_start(argptr, format);
    vsnprintf(buf, VA_BUFSIZE, format, argptr);
    va_end(argptr);

    return buf;
}

/* ==========================================================================
   Info String Functions
   ========================================================================== */

/*
 * Info strings are key-value pairs separated by backslashes:
 *   "\name\player\skin\male/grunt\team\blue"
 */

char *Info_ValueForKey(char *s, char *key)
{
    static char value[2][MAX_INFO_VALUE];
    static int  valueindex;
    char        pkey[MAX_INFO_KEY];
    char        *o;

    valueindex ^= 1;

    if (*s == '\\')
        s++;

    while (1) {
        o = pkey;
        while (*s != '\\') {
            if (!*s)
                return "";
            *o++ = *s++;
        }
        *o = 0;
        s++;

        o = value[valueindex];
        while (*s != '\\' && *s) {
            *o++ = *s++;
        }
        *o = 0;

        if (!Q_stricmp(key, pkey))
            return value[valueindex];

        if (!*s)
            return "";
        s++;
    }
}

void Info_RemoveKey(char *s, char *key)
{
    char    pkey[MAX_INFO_KEY];
    char    value[MAX_INFO_VALUE];
    char    *start;
    char    *o;

    if (strchr(key, '\\'))
        return;

    while (1) {
        start = s;
        if (*s == '\\')
            s++;
        o = pkey;
        while (*s != '\\') {
            if (!*s)
                return;
            *o++ = *s++;
        }
        *o = 0;
        s++;

        o = value;
        while (*s != '\\' && *s) {
            *o++ = *s++;
        }
        *o = 0;

        if (!strcmp(key, pkey)) {
            memmove(start, s, strlen(s) + 1);
            return;
        }

        if (!*s)
            return;
    }
}

void Info_SetValueForKey(char *s, char *key, char *value)
{
    char newi[MAX_INFO_STRING];

    if (strchr(key, '\\') || strchr(value, '\\')) {
        return;
    }

    Info_RemoveKey(s, key);
    if (!value || !strlen(value))
        return;

    Com_sprintf(newi, sizeof(newi), "\\%s\\%s", key, value);

    if (strlen(newi) + strlen(s) > MAX_INFO_STRING - 1) {
        return;
    }

    strcat(s, newi);
}

/* ==========================================================================
   Byte Order
   ========================================================================== */

/* SoF is x86-only, so little-endian is native.
 * These are provided for Q2 API compatibility. */

short BigShort(short l)
{
    byte b1 = l & 255;
    byte b2 = (l >> 8) & 255;
    return (short)((b1 << 8) + b2);
}

short LittleShort(short l) { return l; }
int BigLong(int l)
{
    byte b1 = l & 255;
    byte b2 = (l >> 8) & 255;
    byte b3 = (l >> 16) & 255;
    byte b4 = (l >> 24) & 255;
    return ((int)b1 << 24) + ((int)b2 << 16) + ((int)b3 << 8) + b4;
}
int LittleLong(int l) { return l; }
float BigFloat(float l)
{
    union { float f; int i; } u;
    u.f = l;
    u.i = BigLong(u.i);
    return u.f;
}
float LittleFloat(float l) { return l; }
