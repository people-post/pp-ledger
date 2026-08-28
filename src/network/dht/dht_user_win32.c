#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "dht.h"

int dht_sendto(int sockfd, const void *buf, int len, int flags,
               const struct sockaddr *to, int tolen)
{
    return (int)sendto((SOCKET)sockfd, (const char *)buf, len, flags, to, tolen);
}

int dht_gettimeofday(struct timeval *tv, struct timezone *tz)
{
    (void)tz;
    if (!tv) {
        return -1;
    }

    FILETIME fileTime;
    GetSystemTimeAsFileTime(&fileTime);

    ULARGE_INTEGER ui;
    ui.LowPart = fileTime.dwLowDateTime;
    ui.HighPart = fileTime.dwHighDateTime;

    /* FILETIME is 100-ns intervals since Jan 1 1601 UTC. */
    const unsigned long long epochOffset = 116444736000000000ULL;
    const unsigned long long hundredNs = ui.QuadPart - epochOffset;
    tv->tv_sec = (long)(hundredNs / 10000000ULL);
    tv->tv_usec = (long)((hundredNs % 10000000ULL) / 10ULL);
    return 0;
}
