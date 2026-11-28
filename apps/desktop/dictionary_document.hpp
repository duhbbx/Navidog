#pragma once

#include "navidog/database_tools.hpp"
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QPageLayout>

namespace navidog::ui {

enum class DictionaryTheme { Amber, Carmine, Ink, Lake, Jade };
enum class DictionaryBackground { Theme, Image, None };
enum class DictionaryImageFit { Fill, Fit, Tile };
enum class DictionaryPreviewSection { Cover, Contents, Body, Introduction, Full };

struct DictionaryFont {
    QString family;
    int size = 0;
    bool bold = false;
};

struct DictionaryLayout {
    DictionaryTheme theme = DictionaryTheme::Lake;
    bool cover = true, contents = true, introduction = true, header = true, footer = true,
         pageNumbers = true, comments = true, details = true;
    QString title, subtitle = "数据字典", coverHeader, notes, coverFooter = "Navidog";
    QString introductionText = "本文档记录所选数据库的对象结构、字段和约束。";
    QString headerText, footerText;
    DictionaryFont coverHeaderFont{{}, 11}, titleFont{{}, 30, true}, subtitleFont{{}, 17},
                   notesFont{{}, 10}, coverFooterFont{{}, 9}, headerFont{{}, 9}, footerFont{{}, 9},
                   headingFont{{}, 17, true}, objectFont{{}, 13, true}, bodyFont{{}, 10};
    QPageSize::PageSizeId paper = QPageSize::A4;
    QPageLayout::Orientation orientation = QPageLayout::Portrait;
    QMarginsF margins{19,19,19,19};
    DictionaryBackground background = DictionaryBackground::Theme;
    DictionaryImageFit imageFit = DictionaryImageFit::Fill;
    int opacity = 100;
    QImage logo, coverImage;
    QPageLayout pageLayout() const;
};

struct DictionaryColors { QColor accent, heading, stripe, grid; };
struct DictionaryScopeChoice {
    QString id, database, schema;
    bool info = true;
    QStringList order, omitted;
};
struct DictionaryConfiguration {
    DictionaryLayout layout;
    QVector<DictionaryScopeChoice> scopes;
    QMap<QString, bool> serverInfo;
};

QString dictionaryScopeKey(const DictionaryScopeChoice&);
QString dictionaryThemeName(DictionaryTheme);
DictionaryColors dictionaryColors(DictionaryTheme);
QString dictionaryTitle(const DatabaseDictionary&, const DictionaryLayout&);
QString dictionaryText(QString, const DatabaseDictionary&);
QImage dictionaryArtwork(const DictionaryLayout&);
QString dictionaryHtml(const DatabaseDictionary&, const DictionaryLayout&, DictionaryPreviewSection,
                       const Cancellation&);
QJsonObject dictionaryConfigurationJson(const DictionaryConfiguration&);
DictionaryConfiguration dictionaryConfigurationFromJson(const QJsonObject&);
QStringList dictionaryObjectCells(const TableObject&);

}
