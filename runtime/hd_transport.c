// SPDX-License-Identifier: MIT
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Socket;
#define BAD_SOCKET INVALID_SOCKET
#define socket_close closesocket
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
typedef int Socket;
#define BAD_SOCKET (-1)
#define socket_close close
#endif
#include <string.h>
#include "hd_transport.h"
#include "hd_wire.h"
static Socket fd=BAD_SOCKET;
static struct sockaddr_in endpoint;
static uint64_t queried, acknowledged;
static uint32_t sequence;
static bool eligible_now;
#ifdef _WIN32
static bool winsock;
#endif
void pm_hd_close(void) {
    if (fd!=BAD_SOCKET) socket_close(fd);
    fd=BAD_SOCKET;acknowledged=queried=0;eligible_now=false;
#ifdef _WIN32
    if (winsock) { WSACleanup();winsock=false; }
#endif
}
bool pm_hd_active(uint64_t now) {
    return eligible_now && acknowledged && now>=acknowledged && now-acknowledged<2000000000ull;
}
void pm_hd_poll(uint64_t now, bool eligible) {
    if (!eligible) { pm_hd_close();return; }
    eligible_now=true;
    if (fd==BAD_SOCKET) {
#ifdef _WIN32
        WSADATA wsa;if (WSAStartup(MAKEWORD(2,2),&wsa)) return;winsock=true;
#endif
        fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if (fd==BAD_SOCKET) { pm_hd_close();return; }
#ifdef _WIN32
        u_long nonblock=1;ioctlsocket(fd,FIONBIO,&nonblock);
#else
        fcntl(fd,F_SETFL,O_NONBLOCK);
#endif
        memset(&endpoint,0,sizeof endpoint);endpoint.sin_family=AF_INET;
        endpoint.sin_port=htons(26761);endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    }
    // Consume only bounded replies from the expected local endpoint.
    for (int i=0;i<8;i++) {
        unsigned char reply[32];struct sockaddr_in from;
#ifdef _WIN32
        int length=sizeof from;
#else
        socklen_t length=sizeof from;
#endif
        int n=recvfrom(fd,(char*)reply,sizeof reply,0,(struct sockaddr*)&from,&length);
        if (n<0) break;
        if (n==16 && from.sin_addr.s_addr==endpoint.sin_addr.s_addr && from.sin_port==endpoint.sin_port
            && !memcmp(reply,"PC2A\1\0\0\0",8) && (reply[8]&3)==3) acknowledged=now;
    }
    if (!queried || now-queried>=1000000000ull) {
        const char query[8]={'P','C','2','Q',1,0,0,0};
        sendto(fd,query,sizeof query,0,(struct sockaddr*)&endpoint,sizeof endpoint);queried=now;
    }
}
void pm_hd_send(const PMVibration values[2],uint64_t now) {
    if (!pm_hd_active(now)) return;
    unsigned char packet[56];pm_hd_encode(packet,++sequence,now,values);
    sendto(fd,(const char*)packet,sizeof packet,0,(struct sockaddr*)&endpoint,sizeof endpoint);
}
