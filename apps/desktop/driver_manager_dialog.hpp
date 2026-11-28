#pragma once
#include "navidog/database_types.hpp"
class QWidget;
void showDriverManager(QWidget*, navidog::DatabaseKind selected = navidog::DatabaseKind::Unknown);
