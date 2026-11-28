#pragma once
#include "navidog/catalog.hpp"
#include "navidog/metadata_cache.hpp"
namespace navidog {
CatalogResult loadExtendedCatalog(Database&, const ConnectionProfile&, const CatalogNode&, const Cancellation&);
MetadataSnapshot collectExtendedMetadata(Database&, const ConnectionProfile&, const QString&, const Cancellation&);
QString extendedPreview(const ConnectionProfile&, const CatalogNode&);
QString extendedDefinition(const ConnectionProfile&, const CatalogNode&);
}
