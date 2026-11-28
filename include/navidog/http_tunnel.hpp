#pragma once
#include "database.hpp"
namespace navidog {
std::unique_ptr<Database> connectHttpTunnel(const ConnectionProfile&);
QByteArray httpTunnelScript();
}
