#pragma once
#include "navidog/catalog.hpp"
#include <functional>
#include <optional>

class QWidget;
namespace navidog::ui {
void showDictionaryWizard(QWidget* parent, const QVector<ConnectionProfile>& profiles, const QString& profile,
                          const CatalogNode&, const QString& storePath,
                          std::function<std::optional<ConnectionProfile>(const QString&, const CatalogNode&)>,
                          std::function<QVector<CatalogNode>(const QString&)>);
}
