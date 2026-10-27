/*
 * hybris-ctype-fix: keep glibc's thread-local ctype tables usable in a
 * process that also hosts bionic (Android) libraries through libhybris.
 *
 * libhybris runs Android code with glibc's thread pointer.  bionic's
 * TLS_SLOT_OPENGL lives at offset 16 from that pointer, which is exactly
 * where glibc keeps the thread-local `__ctype_b` pointer.  Android's
 * /system/lib/libEGL.so writes its slot from a constructor, so glibc's
 * ctype table pointer is replaced by a bionic table pointer.  Any
 * isspace()/isalpha()/isdigit() call that goes through the PLT then reads
 * the wrong table; Xlib's locale-file parser walks past the end of its
 * buffer and crashes.
 *
 * This preload interposes the ctype locators and restores a valid C-locale
 * table whenever the slot no longer looks like a glibc ctype table.  It is
 * a workaround for the TLS ABI collision, not a general libhybris fix.
 *
 * Build:
 *   arm-none-linux-gnueabihf-gcc --sysroot=$SYSROOT -O2 -Wall -Wextra \
 *       -fPIC -shared -o libhybris-ctype-fix.so hybris-ctype-fix.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

/* glibc ctype bit layout: _ISbit(n) == (n < 8) ? (1 << n) << 8 : (1 << n) >> 8 */
#define C_UPPER  0x0100u
#define C_LOWER  0x0200u
#define C_ALPHA  0x0400u
#define C_DIGIT  0x0800u
#define C_XDIGIT 0x1000u
#define C_SPACE  0x2000u
#define C_PRINT  0x4000u
#define C_GRAPH  0x8000u
#define C_BLANK  0x0001u
#define C_CNTRL  0x0002u
#define C_PUNCT  0x0004u
#define C_ALNUM  0x0008u

static unsigned short c_class[384];
static int32_t c_tolower[384];
static int32_t c_toupper[384];
static int tables_ready;

static void build_tables(void)
{
    int c;

    for (c = 0; c < 256; c++) {
        const int upper = (c >= 'A' && c <= 'Z');
        const int lower = (c >= 'a' && c <= 'z');
        const int digit = (c >= '0' && c <= '9');
        const int alpha = upper || lower;
        const int alnum = alpha || digit;
        const int space = (c == ' ' || c == '\t' || c == '\n' ||
                           c == '\v' || c == '\f' || c == '\r');
        const int print = (c >= 0x20 && c <= 0x7e);
        const int graph = (c >= 0x21 && c <= 0x7e);
        unsigned short bits = 0;

        if (upper)
            bits |= C_UPPER;
        if (lower)
            bits |= C_LOWER;
        if (alpha)
            bits |= C_ALPHA;
        if (digit)
            bits |= C_DIGIT;
        if (digit || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
            bits |= C_XDIGIT;
        if (space)
            bits |= C_SPACE;
        if (print)
            bits |= C_PRINT;
        if (graph)
            bits |= C_GRAPH;
        if (c == ' ' || c == '\t')
            bits |= C_BLANK;
        if (c < 0x20 || c == 0x7f)
            bits |= C_CNTRL;
        if (graph && !alnum)
            bits |= C_PUNCT;
        if (alnum)
            bits |= C_ALNUM;

        c_class[128 + c] = bits;
        c_tolower[128 + c] = upper ? c - 'A' + 'a' : c;
        c_toupper[128 + c] = lower ? c - 'a' + 'A' : c;
    }

    /* glibc duplicates the 128..255 entries for negative (signed char) indices. */
    for (c = 0; c < 128; c++) {
        c_class[c] = c_class[c + 256];
        c_tolower[c] = c_tolower[c + 256];
        c_toupper[c] = c_toupper[c + 256];
    }

    tables_ready = 1;
}

static int class_looks_valid(const unsigned short *t)
{
    if (!t)
        return 0;
    return (t[' '] & C_SPACE) != 0 &&
           (t['A'] & C_UPPER) != 0 &&
           (t['0'] & C_DIGIT) != 0 &&
           (t[0] & C_SPACE) == 0;
}

static int lower_looks_valid(const int32_t *t)
{
    return t && t['A'] == 'a' && t['0'] == '0';
}

static int upper_looks_valid(const int32_t *t)
{
    return t && t['a'] == 'A' && t['0'] == '0';
}

const unsigned short **__ctype_b_loc(void)
{
    static const unsigned short **(*real_loc)(void);
    const unsigned short **slot;

    if (!real_loc)
        real_loc = (const unsigned short **(*)(void))dlsym(RTLD_NEXT, "__ctype_b_loc");
    slot = real_loc();
    if (!class_looks_valid(*slot)) {
        if (!tables_ready)
            build_tables();
        *slot = c_class + 128;
    }
    return slot;
}

const int32_t **__ctype_tolower_loc(void)
{
    static const int32_t **(*real_loc)(void);
    const int32_t **slot;

    if (!real_loc)
        real_loc = (const int32_t **(*)(void))dlsym(RTLD_NEXT, "__ctype_tolower_loc");
    slot = real_loc();
    if (!lower_looks_valid(*slot)) {
        if (!tables_ready)
            build_tables();
        *slot = c_tolower + 128;
    }
    return slot;
}

const int32_t **__ctype_toupper_loc(void)
{
    static const int32_t **(*real_loc)(void);
    const int32_t **slot;

    if (!real_loc)
        real_loc = (const int32_t **(*)(void))dlsym(RTLD_NEXT, "__ctype_toupper_loc");
    slot = real_loc();
    if (!upper_looks_valid(*slot)) {
        if (!tables_ready)
            build_tables();
        *slot = c_toupper + 128;
    }
    return slot;
}
