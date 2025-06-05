/*
 *  Random function wrapper
 *
 *  Copyright (c) 2008-2021 Alexander Chukov <sashz@pdaXrom.org>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */

#include <stdio.h>
#ifdef __linux__
#include <unistd.h>
#include <sys/syscall.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#include "getrandom.h"

/**
 * @brief Get random bytes to buffer
 * @param buf Buffer
 * @param buflen Buffer length
 * @param flags Flags
 * @return Status
 */
int simple_connection_get_random(void *buf, size_t buflen, unsigned int flags)
{
#if defined(_WIN32)
    int ret = -1;
    HCRYPTPROV hProvider = 0;
    if (!CryptAcquireContextW(&hProvider, 0, 0, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
	return -1;
    }
    if (CryptGenRandom(hProvider, buflen, buf)) {
	ret = buflen;
    }
    CryptReleaseContext(hProvider, 0);
    return ret;
#else
    int ret;
    FILE *inf;
#if defined(__linux__)

#ifndef SYS_getrandom
#warning Trying to define SYS_getrandom
#if defined(__x86_64__)
#define SYS_getrandom 318
#elif defined(__i386__)
#define SYS_getrandom 355
#elif defined(__aarch64__)
#define SYS_getrandom 278
#elif defined(__arm__)
#define SYS_getrandom 384
#elif defined(__mips__)
#define SYS_getrandom 278
#elif defined(__powerpc__)
#define SYS_getrandom 359
#else
#error Unsupported arch!
#endif
#endif

    ret = (int)syscall(SYS_getrandom, buf, buflen, flags);
    if (ret != -1) {
	return ret;
    }
#endif
    inf = fopen("/dev/urandom", "rb");
    if (!inf) {
	return -1;
    }
    ret = fread(buf, 1, buflen, inf);
    fclose(inf);
    if (ret != buflen) {
	ret = -1;
    }
    return ret;
#endif
}
