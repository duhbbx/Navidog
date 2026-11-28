#include "navidog/ssh_tunnel.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstring>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QElapsedTimer>
#include <mutex>
#include <thread>
#include <vector>
#ifdef NAVIDOG_HAS_SSH
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif
#include <libssh2.h>
#endif
namespace navidog {
SshHostKeyError::SshHostKeyError(QString key,QString hash,bool changed):std::runtime_error(changed?"SSH 主机密钥与已保存的密钥不一致，请核实服务器身份":"SSH 主机密钥尚未信任，请核对指纹"),publicKey(std::move(key)),fingerprint(std::move(hash)),mismatch(changed){}
namespace {
[[noreturn]] void fail(const QString& error){throw std::runtime_error(error.toStdString());}
#ifdef NAVIDOG_HAS_SSH
using Socket=libssh2_socket_t;
#ifdef Q_OS_WIN
constexpr Socket invalid=INVALID_SOCKET;
void closeSocket(Socket s){if(s!=invalid)closesocket(s);}int socketError(){return WSAGetLastError();}bool again(int error){return error==WSAEWOULDBLOCK || error==WSAEINPROGRESS;}void nonblocking(Socket s){u_long yes=1;if(ioctlsocket(s,FIONBIO,&yes))fail("无法设置 SSH 非阻塞套接字");}
void shutdownSocket(Socket s){if(s!=invalid)shutdown(s,SD_BOTH);}
#else
constexpr Socket invalid=-1;
void closeSocket(Socket s){if(s!=invalid)::close(s);}int socketError(){return errno;}bool again(int error){return error==EAGAIN || error==EWOULDBLOCK || error==EINPROGRESS;}void nonblocking(Socket s){
#ifdef SO_NOSIGPIPE
int yes=1;setsockopt(s,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
#endif
auto flags=fcntl(s,F_GETFL,0);if(flags<0 || fcntl(s,F_SETFL,flags|O_NONBLOCK))fail("无法设置 SSH 非阻塞套接字");}
void shutdownSocket(Socket s){if(s!=invalid)shutdown(s,SHUT_RDWR);}
#endif
bool selectable(Socket s){
#ifdef Q_OS_WIN
    return s!=invalid;
#else
    return s>=0 && s<FD_SETSIZE;
#endif
}
struct OwnedSocket {Socket value=invalid;~OwnedSocket(){closeSocket(value);}OwnedSocket()=default;explicit OwnedSocket(Socket fd):value(fd){}OwnedSocket(const OwnedSocket&)=delete;OwnedSocket& operator=(const OwnedSocket&)=delete;Socket release(){auto s=value;value=invalid;return s;}};
QString lastError(LIBSSH2_SESSION* session){char* text=nullptr;int length=0;libssh2_session_last_error(session,&text,&length,0);return text?QString::fromUtf8(text,length):QString("SSH 请求失败");}
Socket connectSocket(const QString& name,int port,int seconds){QList<QHostAddress> addresses;QHostAddress literal;if(literal.setAddress(name))addresses<<literal;else addresses=QHostInfo::fromName(name).addresses();if(addresses.isEmpty())fail("无法解析 SSH 主机");QElapsedTimer elapsed;elapsed.start();
    for(auto address:addresses){auto family=address.protocol()==QAbstractSocket::IPv6Protocol?AF_INET6:AF_INET;OwnedSocket socket(::socket(family,SOCK_STREAM,0));if(!selectable(socket.value))continue;nonblocking(socket.value);sockaddr_storage target{};int size=0;if(family==AF_INET6){auto* value=reinterpret_cast<sockaddr_in6*>(&target);value->sin6_family=AF_INET6;value->sin6_port=htons(port);auto bytes=address.toIPv6Address();memcpy(&value->sin6_addr,bytes.c,16);if(!address.scopeId().isEmpty())value->sin6_scope_id=QNetworkInterface::interfaceIndexFromName(address.scopeId());size=sizeof(sockaddr_in6);}else{auto* value=reinterpret_cast<sockaddr_in*>(&target);value->sin_family=AF_INET;value->sin_port=htons(port);value->sin_addr.s_addr=htonl(address.toIPv4Address());size=sizeof(sockaddr_in);}
        int rc=::connect(socket.value,reinterpret_cast<sockaddr*>(&target),size);if(rc && !again(socketError()))continue;if(rc){int remaining=seconds*1000-int(elapsed.elapsed());if(remaining<=0)break;fd_set writable;FD_ZERO(&writable);FD_SET(socket.value,&writable);timeval timeout{remaining/1000,(remaining%1000)*1000};if(select(int(socket.value)+1,nullptr,&writable,nullptr,&timeout)<=0)continue;int error=0;
#ifdef Q_OS_WIN
            int len=sizeof(error);getsockopt(socket.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&len);
#else
            socklen_t len=sizeof(error);getsockopt(socket.value,SOL_SOCKET,SO_ERROR,&error,&len);
#endif
            if(error)continue;}
        return socket.release();
    }fail("SSH TCP 连接失败或超时");
}
QString keyType(int type){switch(type){case LIBSSH2_HOSTKEY_TYPE_RSA:return "ssh-rsa";case LIBSSH2_HOSTKEY_TYPE_DSS:return "ssh-dss";case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:return "ecdsa-sha2-nistp256";case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:return "ecdsa-sha2-nistp384";case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:return "ecdsa-sha2-nistp521";case LIBSSH2_HOSTKEY_TYPE_ED25519:return "ssh-ed25519";default:fail("不支持该 SSH 主机密钥类型");}}
int knownKeyType(int type){switch(type){case LIBSSH2_HOSTKEY_TYPE_RSA:return LIBSSH2_KNOWNHOST_KEY_SSHRSA;case LIBSSH2_HOSTKEY_TYPE_DSS:return LIBSSH2_KNOWNHOST_KEY_SSHDSS;case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;default:return LIBSSH2_KNOWNHOST_KEY_ED25519;}}
class Tunnel final:public SshTunnel {
    Socket socket_=invalid,listener_=invalid;LIBSSH2_SESSION* session_=nullptr;std::thread io_;std::atomic_bool stop_{false};int port_=0;QByteArray remote_;int remotePort_;
    struct Stream {Socket socket=invalid;LIBSSH2_CHANNEL* channel=nullptr;QByteArray incoming,outgoing,writing;int written=0;bool clientEOF=false,remoteEOF=false,eofSent=false;};
    void verify(const ConnectionProfile& p){size_t length=0;int type=0;auto* key=libssh2_session_hostkey(session_,&length,&type);if(!key || !length)fail("SSH 服务器没有提供主机密钥");QByteArray raw(key,length);auto encoded=raw.toBase64();auto publicKey=keyType(type)+" "+QString::fromLatin1(encoded);auto fingerprint="SHA256:"+QString::fromLatin1(QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toBase64(QByteArray::OmitTrailingEquals));
        if(!p.ssh.hostKey.isEmpty()){if(p.ssh.hostKey!=publicKey)throw SshHostKeyError(publicKey,fingerprint,true);return;}
        auto* hosts=libssh2_knownhost_init(session_);if(!hosts)fail("无法创建 SSH 主机密钥校验器");struct Release{LIBSSH2_KNOWNHOSTS* hosts;~Release(){libssh2_knownhost_free(hosts);}}release{hosts};auto file=p.ssh.knownHostsFile.isEmpty()?QDir::home().filePath(".ssh/known_hosts"):p.ssh.knownHostsFile;if(QFileInfo::exists(file))libssh2_knownhost_readfile(hosts,QFile::encodeName(file).constData(),LIBSSH2_KNOWNHOST_FILE_OPENSSH);libssh2_knownhost* match=nullptr;auto name=p.ssh.host.toUtf8();int status=libssh2_knownhost_checkp(hosts,name.constData(),p.ssh.port,key,length,LIBSSH2_KNOWNHOST_TYPE_PLAIN|LIBSSH2_KNOWNHOST_KEYENC_RAW|knownKeyType(type),&match);if(status==LIBSSH2_KNOWNHOST_CHECK_MATCH)return;throw SshHostKeyError(publicKey,fingerprint,status==LIBSSH2_KNOWNHOST_CHECK_MISMATCH);
    }
    void authenticate(const ConnectionProfile& p){auto user=p.ssh.user.toUtf8();auto pass=p.ssh.password.toUtf8(),phrase=p.ssh.passphrase.toUtf8();auto mode=p.ssh.authentication;
        if(mode==SshAuthentication::PublicKey || mode==SshAuthentication::PasswordAndKey){auto file=QFile::encodeName(p.ssh.keyFile);int rc=libssh2_userauth_publickey_fromfile_ex(session_,user.constData(),user.size(),nullptr,file.constData(),phrase.isEmpty()?nullptr:phrase.constData());if(rc && (mode!=SshAuthentication::PasswordAndKey || libssh2_userauth_authenticated(session_)))fail("SSH 公钥验证失败："+lastError(session_));}
        if(mode==SshAuthentication::Password || mode==SshAuthentication::PasswordAndKey){if(!libssh2_userauth_authenticated(session_)){int rc=libssh2_userauth_password_ex(session_,user.constData(),user.size(),pass.constData(),pass.size(),nullptr);pass.fill('\0');if(rc)fail("SSH 密码验证失败："+lastError(session_));}}
        if(mode==SshAuthentication::Agent){auto* agent=libssh2_agent_init(session_);if(!agent)fail("无法打开 SSH Agent");struct Release{LIBSSH2_AGENT* value;~Release(){libssh2_agent_disconnect(value);libssh2_agent_free(value);}}release{agent};if(libssh2_agent_connect(agent) || libssh2_agent_list_identities(agent))fail("无法读取 SSH Agent 密钥");libssh2_agent_publickey* previous=nullptr;libssh2_agent_publickey* key=nullptr;while(libssh2_agent_get_identity(agent,&key,previous)==0){if(libssh2_agent_userauth(agent,user.constData(),key)==0)break;previous=key;}}
        phrase.fill('\0');pass.fill('\0');if(!libssh2_userauth_authenticated(session_))fail("SSH 身份验证没有完成");
    }
    void loop(){std::vector<Stream> streams;std::vector<LIBSSH2_CHANNEL*> closing;QElapsedTimer keepTimer;keepTimer.start();QByteArray scratch(64*1024,Qt::Uninitialized);
        while(!stop_){if(keepTimer.elapsed()>1000){int next=0;libssh2_keepalive_send(session_,&next);keepTimer.restart();}for(auto it=closing.begin();it!=closing.end();){if(libssh2_channel_free(*it)!=LIBSSH2_ERROR_EAGAIN)it=closing.erase(it);else ++it;}fd_set readable,writable;FD_ZERO(&readable);FD_ZERO(&writable);FD_SET(listener_,&readable);FD_SET(socket_,&readable);int maxfd=int(qMax(listener_,socket_));bool pending=false;
            for(const auto& s:streams){if(!s.clientEOF && s.incoming.size()+s.writing.size()<1024*1024)FD_SET(s.socket,&readable);if(!s.outgoing.isEmpty())FD_SET(s.socket,&writable);maxfd=qMax(maxfd,int(s.socket));pending|=!s.incoming.isEmpty() || !s.writing.isEmpty() || !s.channel;}
            timeval timeout{0,pending?2000:20000};int ready=select(maxfd+1,&readable,&writable,nullptr,&timeout);if(ready<0){if(stop_)break;continue;}
            if(FD_ISSET(listener_,&readable)){auto client=::accept(listener_,nullptr,nullptr);if(client!=invalid){if(streams.size()>=4 || !selectable(client))closeSocket(client);else{nonblocking(client);Stream s;s.socket=client;streams.push_back(std::move(s));}}}
            bool opening=false;for(auto& s:streams){if(!s.channel && !opening){s.channel=libssh2_channel_direct_tcpip_ex(session_,remote_.constData(),remotePort_,"127.0.0.1",0);if(!s.channel){if(libssh2_session_last_errno(session_)!=LIBSSH2_ERROR_EAGAIN){s.clientEOF=s.remoteEOF=true;}else opening=true;}}
                if(!s.channel)continue;
                if(!s.clientEOF && s.incoming.size()+s.writing.size()<1024*1024 && FD_ISSET(s.socket,&readable)){int n=::recv(s.socket,scratch.data(),scratch.size(),0);if(n>0)s.incoming.append(scratch.constData(),n);else if(n==0 || !again(socketError()))s.clientEOF=true;}
                if(s.writing.isEmpty() && !s.incoming.isEmpty()){s.writing=s.incoming.left(64*1024);s.incoming.remove(0,s.writing.size());s.written=0;}
                if(!s.writing.isEmpty()){auto n=libssh2_channel_write(s.channel,s.writing.constData()+s.written,s.writing.size()-s.written);if(n>0){s.written+=n;if(s.written==s.writing.size()){s.writing.clear();s.written=0;}}else if(n!=LIBSSH2_ERROR_EAGAIN){s.clientEOF=s.remoteEOF=true;s.incoming.clear();s.writing.clear();}}
                if(s.clientEOF && s.incoming.isEmpty() && s.writing.isEmpty() && !s.eofSent)s.eofSent=libssh2_channel_send_eof(s.channel)==0;
                for(int attempt=0;attempt<8 && s.outgoing.size()<1024*1024;++attempt){auto n=libssh2_channel_read(s.channel,scratch.data(),scratch.size());if(n>0)s.outgoing.append(scratch.constData(),n);else{if(n!=LIBSSH2_ERROR_EAGAIN && n<0)s.remoteEOF=true;break;}}
                s.remoteEOF|=libssh2_channel_eof(s.channel)!=0;
                if(!s.outgoing.isEmpty()){int n=::send(s.socket,s.outgoing.constData(),s.outgoing.size(),0);if(n>0)s.outgoing.remove(0,n);else if(n<0 && !again(socketError())){s.clientEOF=true;s.outgoing.clear();}}
                if(s.remoteEOF && s.outgoing.isEmpty())s.clientEOF=true;
            }
            for(auto it=streams.begin();it!=streams.end();){if(it->clientEOF && it->remoteEOF && it->incoming.isEmpty() && it->writing.isEmpty() && it->outgoing.isEmpty()){if(it->channel && libssh2_channel_free(it->channel)==LIBSSH2_ERROR_EAGAIN)closing.push_back(it->channel);closeSocket(it->socket);it=streams.erase(it);}else ++it;}
            if(libssh2_session_last_errno(session_)==LIBSSH2_ERROR_SOCKET_DISCONNECT)break;
        }
        for(auto* channel:closing)libssh2_channel_free(channel);
        for(auto& s:streams){if(s.channel)libssh2_channel_free(s.channel);shutdownSocket(s.socket);closeSocket(s.socket);}
    }
public:
    explicit Tunnel(const ConnectionProfile& p):remote_(p.host.toUtf8()),remotePort_(p.port){static std::once_flag init;std::call_once(init,[]{
#ifdef Q_OS_WIN
            WSADATA data;if(WSAStartup(MAKEWORD(2,2),&data))fail("无法初始化 Windows 网络");
#endif
            if(libssh2_init(0))fail("无法初始化 libssh2");});
        try{socket_=connectSocket(p.ssh.host,p.ssh.port,p.advanced.connectTimeout);session_=libssh2_session_init();if(!session_)fail("无法创建 SSH 会话");libssh2_session_set_timeout(session_,p.advanced.connectTimeout*1000L);libssh2_session_set_blocking(session_,1);if(libssh2_session_handshake(session_,socket_))fail("SSH 握手失败："+lastError(session_));verify(p);authenticate(p);libssh2_keepalive_config(session_,1,p.advanced.keepAlive?p.advanced.keepAliveSeconds:30);libssh2_session_set_blocking(session_,0);
            listener_=::socket(AF_INET,SOCK_STREAM,0);if(!selectable(listener_))fail("无法创建本地隧道监听");sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;if(::bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || ::listen(listener_,4))fail("无法打开本地隧道端口");
#ifdef Q_OS_WIN
            int length=sizeof(address);
#else
            socklen_t length=sizeof(address);
#endif
            if(getsockname(listener_,reinterpret_cast<sockaddr*>(&address),&length))fail("无法获取本地隧道端口");port_=ntohs(address.sin_port);nonblocking(listener_);io_=std::thread([this]{try{loop();}catch(...){shutdownSocket(socket_);}});
        }catch(...){if(session_)libssh2_session_free(session_);closeSocket(listener_);closeSocket(socket_);throw;}
    }
    ~Tunnel()override{stop_=true;shutdownSocket(socket_);if(io_.joinable())io_.join();if(session_)libssh2_session_free(session_);closeSocket(listener_);closeSocket(socket_);}
    int port()const override{return port_;}
};
#endif
}
bool sshAvailable(){
#ifdef NAVIDOG_HAS_SSH
    return true;
#else
    return false;
#endif
}
std::unique_ptr<SshTunnel> openSshTunnel(const ConnectionProfile& p){
#ifdef NAVIDOG_HAS_SSH
    return std::make_unique<Tunnel>(p);
#else
    Q_UNUSED(p);fail("此构建未包含 libssh2 隧道支持");
#endif
}
}
