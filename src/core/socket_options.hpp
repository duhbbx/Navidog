#pragma once
#include <stdexcept>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <mstcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif
namespace navidog {
template<class Socket> void abortSocket(Socket socket){
#ifdef Q_OS_WIN
    shutdown(socket,SD_BOTH);
#else
    shutdown(socket,SHUT_RDWR);
#endif
}

template<class Socket> void configureKeepAlive(Socket socket,int seconds){
    int enabled=1;
#ifdef Q_OS_WIN
    if(setsockopt(socket,SOL_SOCKET,SO_KEEPALIVE,reinterpret_cast<const char*>(&enabled),sizeof(enabled)))throw std::runtime_error("无法设置 TCP 保持连接");tcp_keepalive options{};options.onoff=1;options.keepalivetime=seconds*1000;options.keepaliveinterval=10000;DWORD returned=0;if(WSAIoctl(socket,SIO_KEEPALIVE_VALS,&options,sizeof(options),nullptr,0,&returned,nullptr,nullptr))throw std::runtime_error("无法设置 TCP 保持连接间隔");
#else
    if(setsockopt(socket,SOL_SOCKET,SO_KEEPALIVE,&enabled,sizeof(enabled)))throw std::runtime_error("无法设置 TCP 保持连接");
#ifdef Q_OS_MACOS
    if(setsockopt(socket,IPPROTO_TCP,TCP_KEEPALIVE,&seconds,sizeof(seconds)))throw std::runtime_error("无法设置 TCP 保持连接间隔");
#elif defined(TCP_KEEPIDLE)
    if(setsockopt(socket,IPPROTO_TCP,TCP_KEEPIDLE,&seconds,sizeof(seconds)))throw std::runtime_error("无法设置 TCP 保持连接间隔");
#endif
#endif
}
}
