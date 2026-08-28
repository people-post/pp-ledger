#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "dht.h"

int dht_sendto(int sockfd, const void *buf, int len, int flags,
               const struct sockaddr *to, int tolen)
{
    return (int)sendto(sockfd, buf, (size_t)len, flags, to, (socklen_t)tolen);
}
