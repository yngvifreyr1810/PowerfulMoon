// MIT. The bridge is a separate GPL executable, never linked into this host.
#include "controller_bootstrap.h"
#include "hle.h"
#include "paths.h"
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
void pm_start_bundled_bridge(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2),&wsa)!=0) return;
    SOCKET fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (fd==INVALID_SOCKET) { WSACleanup(); return; }
    struct sockaddr_in addr={0}; addr.sin_family=AF_INET; addr.sin_port=htons(26759); addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    u_long nonblock=1; ioctlsocket(fd,FIONBIO,&nonblock);
    connect(fd,(struct sockaddr *)&addr,sizeof addr);
    fd_set ready; FD_ZERO(&ready); FD_SET(fd,&ready);
    struct timeval timeout={0,300000};
    int pending=0,sz=sizeof pending;
    bool running=select(0,NULL,&ready,NULL,&timeout)>0 && getsockopt(fd,SOL_SOCKET,SO_ERROR,(char *)&pending,&sz)==0 && pending==0;
    closesocket(fd); WSACleanup();
    if (running) { LOG("[controller] ProCon2Bridge already running\n"); return; }
    char exe[1200],command[2500];
    if (!rt_resource("controller-bridge/ProCon2Bridge.exe",NULL,exe,sizeof exe)) {
        LOG("[controller] bundled bridge absent; standard SDL controllers remain available\n"); return;
    }
    snprintf(command,sizeof command,"\"%s\"",exe);
    STARTUPINFOA startup={.cb=sizeof startup}; PROCESS_INFORMATION process={0};
    if (CreateProcessA(exe,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process)) {
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        LOG("[controller] started bundled ProCon2Bridge setup/background app\n");
    } else LOG("[controller] bridge could not be started (%lu)\n",GetLastError());
}
#else
void pm_start_bundled_bridge(void) {}
#endif
