/* USFX: minimal Adler-32 so the core does not need zlib just for this.
   Same result as zlib's adler32(). Public domain. */
#ifndef USF_ADLER32_H
#define USF_ADLER32_H

#include <stddef.h>

static unsigned long adler32(unsigned long adler, const unsigned char *buf, unsigned int len)
{
    unsigned long a = adler & 0xffff, b = (adler >> 16) & 0xffff;
    if (buf == NULL)
        return 1UL;
    while (len)
    {
        unsigned int n = len > 5552 ? 5552 : len;
        len -= n;
        while (n--)
        {
            a += *buf++;
            b += a;
        }
        a %= 65521UL;
        b %= 65521UL;
    }
    return (b << 16) | a;
}

#endif
