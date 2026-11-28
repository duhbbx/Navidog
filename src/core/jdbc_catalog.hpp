#pragma once

#include "navidog/catalog.hpp"
#include "navidog/metadata_cache.hpp"
#include "navidog/table_design.hpp"

namespace navidog {
CatalogResult loadJdbcCatalog(Database&, const ConnectionProfile&, const CatalogNode&, const Cancellation&);
MetadataSnapshot collectJdbcMetadata(Database&, const ConnectionProfile&, const QString&, const Cancellation&, bool);
TableDefinition readJdbcTable(Database&, const CatalogNode&, const Cancellation&);
}
