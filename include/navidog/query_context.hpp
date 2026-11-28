#pragma once
#include "database.hpp"
namespace navidog {
// Apply only to this worker-owned query connection, before a user transaction.
void applyQuerySchema(Database&,const ConnectionProfile&,const QString&,const Cancellation&);
}
