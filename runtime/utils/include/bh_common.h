#ifndef _BH_COMMON_H
#define _BH_COMMON_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define bh_memcpy_s(dest, dlen, src, slen)                            \
    do {                                                              \
        int _ret = slen == 0 ? 0 : b_memcpy_s(dest, dlen, src, slen); \
        (void)_ret;                                                   \
    } while (0)

#define bh_memcpy_wa(dest, dlen, src, slen)                            \
    do {                                                               \
        int _ret = slen == 0 ? 0 : b_memcpy_wa(dest, dlen, src, slen); \
        (void)_ret;                                                    \
    } while (0)                                                        

#define bh_memmove_s(dest, dlen, src, slen)                            \
    do {                                                               \
        int _ret = slen == 0 ? 0 : b_memmove_s(dest, dlen, src, slen); \
        (void)_ret;                                                    \
    } while (0)

#define bh_strcat_s(dest, dlen, src)            \
    do {                                        \
        int _ret = b_strcat_s(dest, dlen, src); \
        (void)_ret;                             \
    } while (0)

#define bh_strcpy_s(dest, dlen, src)            \
    do {                                        \
        int _ret = b_strcpy_s(dest, dlen, src); \
        (void)_ret;                             \
    } while (0)

int
b_memcpy_s(void *s1, unsigned int s1max, const void *s2, unsigned int n);
int
b_memcpy_wa(void *s1, unsigned int s1max, const void *s2, unsigned int n);
int
b_memmove_s(void *s1, unsigned int s1max, const void *s2, unsigned int n);
int
b_strcat_s(char *s1, unsigned int s1max, const char *s2);
int
b_strcpy_s(char *s1, unsigned int s1max, const char *s2);

/* strdup with string allocated by BH_MALLOC */
char *
bh_strdup(const char *s);

/* strdup with string allocated by malloc */
char *
wa_strdup(const char *s);

#ifdef __cplusplus
}
#endif

#endif
