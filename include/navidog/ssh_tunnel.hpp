#pragma once
#include "types.hpp"
#include <stdexcept>
namespace navidog {
class SshHostKeyError final:public std::runtime_error {
public:
    QString publicKey,fingerprint;
    bool mismatch;
    SshHostKeyError(QString key,QString fingerprint,bool mismatch);
};
class SshTunnel {
public:
    virtual ~SshTunnel()=default;
    virtual int port()const=0;
};
bool sshAvailable();
std::unique_ptr<SshTunnel> openSshTunnel(const ConnectionProfile&);
}
