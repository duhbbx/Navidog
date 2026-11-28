#include "dictionary_document.hpp"
#include <QBuffer>
#include <QDateTime>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <array>
#include <cmath>
#include <stdexcept>

namespace navidog::ui {
namespace {
constexpr int imageBytes = 2 * 1024 * 1024;

struct TextSetting { const char* key; QString DictionaryLayout::*member; };
const TextSetting textSettings[] = {
    {"title", &DictionaryLayout::title}, {"subtitle", &DictionaryLayout::subtitle},
    {"coverHeader", &DictionaryLayout::coverHeader}, {"notes", &DictionaryLayout::notes},
    {"coverFooter", &DictionaryLayout::coverFooter}, {"introductionText", &DictionaryLayout::introductionText},
    {"headerText", &DictionaryLayout::headerText}, {"footerText", &DictionaryLayout::footerText}
};
struct SwitchSetting { const char* key; bool DictionaryLayout::*member; };
const SwitchSetting switchSettings[] = {
    {"cover", &DictionaryLayout::cover}, {"contents", &DictionaryLayout::contents},
    {"introduction", &DictionaryLayout::introduction}, {"header", &DictionaryLayout::header},
    {"footer", &DictionaryLayout::footer}, {"pageNumbers", &DictionaryLayout::pageNumbers},
    {"comments", &DictionaryLayout::comments}, {"details", &DictionaryLayout::details}
};
struct FontSetting { const char* key; DictionaryFont DictionaryLayout::*member; };
const FontSetting fontSettings[] = {
    {"coverHeaderFont", &DictionaryLayout::coverHeaderFont}, {"titleFont", &DictionaryLayout::titleFont},
    {"subtitleFont", &DictionaryLayout::subtitleFont}, {"notesFont", &DictionaryLayout::notesFont},
    {"coverFooterFont", &DictionaryLayout::coverFooterFont}, {"headerFont", &DictionaryLayout::headerFont},
    {"footerFont", &DictionaryLayout::footerFont}, {"headingFont", &DictionaryLayout::headingFont},
    {"objectFont", &DictionaryLayout::objectFont}, {"bodyFont", &DictionaryLayout::bodyFont}
};

QString imageData(const QImage& image) {
    if (image.isNull()) return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG") || bytes.size() > imageBytes)
        throw std::runtime_error("配置图片超过保存限制");
    return QString::fromLatin1(bytes.toBase64());
}

QImage decodeImage(const QJsonValue& value) {
    const auto encoded = value.toString();
    if (encoded.isEmpty()) return {};
    if (encoded.size() > imageBytes * 4 / 3 + 4) throw std::runtime_error("配置图片过大");
    QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "PNG");
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 1200 || size.height() > 1200)
        throw std::runtime_error("配置中的图片无效或尺寸过大");
    const auto image = reader.read();
    if (image.isNull()) throw std::runtime_error("无法读取配置中的图片");
    return image;
}

QString htmlText(QString text) {
    return text.replace("\r\n", "\n").replace('\r', '\n').toHtmlEscaped().replace('\n', "<br>");
}

QString fontCss(const DictionaryFont& font) {
    auto css = QString("font-size:%1pt;font-weight:%2;").arg(font.size).arg(font.bold ? 700 : 400);
    auto family = font.family;
    family.remove(QRegularExpression("[^\\p{L}\\p{N} ._()-]"));
    if (!family.isEmpty()) css += "font-family:'" + family.toHtmlEscaped() + "';";
    return css;
}

QString paragraph(const QString& tag, const QString& value, const DictionaryFont& font, const QString& css = {}) {
    return "<" + tag + " style=\"" + fontCss(font) + css + "\">" + htmlText(value) + "</" + tag + ">";
}
QString inlineImage(const QImage& image, const QString& width) {
    if (image.isNull()) return {};
    return "<img width=\"" + width + "\" src=\"data:image/png;base64," + imageData(image) + "\">";
}

void checkCancel(const Cancellation& token) {
    if (token->load()) throw std::runtime_error("生成数据字典已停止");
}
}

QPageLayout DictionaryLayout::pageLayout() const {
    return {QPageSize(paper), orientation, margins, QPageLayout::Millimeter};
}

QString dictionaryScopeKey(const DictionaryScopeChoice& scope) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{scope.id, scope.database, scope.schema}).toJson(QJsonDocument::Compact));
}

QString dictionaryThemeName(DictionaryTheme theme) {
    static const QStringList names{"琥珀橙", "朱砂红", "墨黑", "湖水蓝", "松石绿"};
    return names.value(int(theme), names[3]);
}

DictionaryColors dictionaryColors(DictionaryTheme theme) {
    static const std::array<DictionaryColors,5> palette{{
        {QColor("#995013"), QColor("#F4E0C7"), QColor("#FBF5ED"), QColor("#D9C4AD")},
        {QColor("#9B384D"), QColor("#F1DDE1"), QColor("#FAF2F4"), QColor("#D9BFC6")},
        {QColor("#24272D"), QColor("#EAEBED"), QColor("#F6F6F7"), QColor("#CED0D4")},
        {QColor("#245B82"), QColor("#DDEAF4"), QColor("#F2F7FB"), QColor("#BDD2E2")},
        {QColor("#186856"), QColor("#D8EEE7"), QColor("#F0F8F5"), QColor("#B8D8CD")}
    }};
    return palette.at(size_t(theme));
}

QString dictionaryTitle(const DatabaseDictionary& dictionary, const DictionaryLayout& layout) {
    return layout.title.trimmed().isEmpty() ? (dictionary.scope.isEmpty() ? QString("数据字典") : dictionary.scope)
                                           : dictionaryText(layout.title, dictionary);
}

QString dictionaryText(QString text, const DatabaseDictionary& dictionary) {
    text.replace("[SCOPE]", dictionary.scope);
    const auto time = QDateTime::currentDateTime();
    return text.replace("[DATE]", time.date().toString(Qt::ISODate)).replace("[TIME]", time.time().toString("HH:mm"));
}

QImage dictionaryArtwork(const DictionaryLayout& layout) {
    if (layout.background == DictionaryBackground::None ||
        (layout.background == DictionaryBackground::Image && layout.coverImage.isNull())) return {};
    const QSize size(960,300);
    QImage result(size, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::white);
    QPainter paint(&result);
    paint.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    paint.setOpacity(layout.opacity / 100.);
    if (layout.background == DictionaryBackground::Image) {
        if (layout.imageFit == DictionaryImageFit::Tile) {
            const auto tile = layout.coverImage.scaled(240,150,Qt::KeepAspectRatio,Qt::SmoothTransformation);
            for (int y = 0; y < size.height(); y += tile.height())
                for (int x = 0; x < size.width(); x += tile.width()) paint.drawImage(x,y,tile);
        } else {
            const auto fit = layout.imageFit == DictionaryImageFit::Fit ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding;
            const auto scaled = layout.coverImage.scaled(size,fit,Qt::SmoothTransformation);
            paint.drawImage((size.width()-scaled.width())/2,(size.height()-scaled.height())/2,scaled);
        }
    } else {
        const auto colors = dictionaryColors(layout.theme);
        paint.fillRect(result.rect(), colors.stripe);
        paint.setPen(Qt::NoPen);
        const auto variant = int(layout.theme);
        for (int i = 0; i < 6; ++i) {
            const int x = 14 + i * 167;
            const int height = 52 + ((i * (39 + variant * 9) + 61 * variant) % 192);
            paint.setBrush(i % 3 == 2 ? colors.accent : (i % 2 ? colors.grid : colors.heading));
            paint.drawRoundedRect(QRectF(x, 286-height, 143, height), 18, 18);
        }
    }
    paint.end();
    return result;
}

QStringList dictionaryObjectCells(const TableObject& object) {
    QString definition = object.expression.isEmpty() ? object.method : object.expression;
    QString rules;
    if (object.kind == TableObjectKind::ForeignKey) {
        definition = (object.referenceSchema.isEmpty() ? QString{} : object.referenceSchema + ".") +
            object.referenceTable + " (" + object.referenceColumns.join(", ") + ")";
        rules = "DELETE " + object.onDelete + "\nUPDATE " + object.onUpdate;
    } else if (object.kind == TableObjectKind::Index && object.unique) rules = "UNIQUE";
    return {object.name, object.columns.join(", "), definition, rules, object.comment};
}

QString dictionaryHtml(const DatabaseDictionary& dictionary, const DictionaryLayout& layout,
                       DictionaryPreviewSection section, const Cancellation& cancel) {
    const auto colors = dictionaryColors(layout.theme);
    const auto accent = colors.accent.name(), heading = colors.heading.name(), stripe = colors.stripe.name();
    const bool all = section == DictionaryPreviewSection::Full;
    const auto paper = layout.pageLayout().fullRect(QPageLayout::Millimeter);
    const QString margins = QString("%1mm %2mm %3mm %4mm").arg(layout.margins.top()).arg(layout.margins.right())
                                                               .arg(layout.margins.bottom()).arg(layout.margins.left());
    const auto title = dictionaryTitle(dictionary, layout);
    QString html = "<!doctype html><html lang='zh'><head><meta charset='utf-8'><title>" + htmlText(title) +
        "</title><style>@page{size:" + QString("%1mm %2mm").arg(paper.width()).arg(paper.height()) +
        ";margin:" + margins + "}body{color:#253548;font-family:system-ui,sans-serif;margin:0}table{width:100%;border-collapse:collapse}"
        "th,td{border:1px solid " + colors.grid.name() + ";padding:6px 7px;vertical-align:top}th{background-color:" + heading +
        ";text-align:left}tr:nth-child(odd){background-color:" + stripe + "}h2,h3,a{color:" + accent +
        "}h2,h3{break-after:avoid}.cover{page-break-after:always;padding-top:28px}.contents{page-break-after:always}"
        "</style></head><body>";
    const auto count = QString("%1 · %2 个对象").arg(dictionary.scope).arg(dictionary.tables.size());
    if (section == DictionaryPreviewSection::Cover || (all && layout.cover)) {
        if (!layout.cover) html += "<p>已隐藏封面</p>";
        else {
            html += "<div class='cover'>" + inlineImage(layout.logo, "90");
            html += paragraph("p", dictionaryText(layout.coverHeader, dictionary), layout.coverHeaderFont);
            html += paragraph("h1", title, layout.titleFont, "color:" + accent);
            html += paragraph("p", dictionaryText(layout.subtitle, dictionary), layout.subtitleFont);
            html += paragraph("p", count, layout.notesFont) + paragraph("p", dictionaryText(layout.notes, dictionary), layout.notesFont);
            html += inlineImage(dictionaryArtwork(layout),
                                QString::number(layout.pageLayout().paintRect(QPageLayout::Point).width()));
            html += paragraph("p", dictionaryText(layout.coverFooter, dictionary), layout.coverFooterFont) + "</div>";
        }
    } else if (all) html += paragraph("h1", title, layout.titleFont);
    if (section == DictionaryPreviewSection::Contents || (all && layout.contents)) {
        if (!layout.contents) html += "<p>已隐藏目录</p>";
        else {
            html += "<div class='contents'><h2>目录</h2><ol>";
            if (layout.introduction) html += "<li><a href='#introduction'>引言</a></li>";
            int entry = 0;
            for (int i = 0; i <= dictionary.tables.size(); ++i) {
                checkCancel(cancel);
                while (entry < dictionary.sections.size() && dictionary.sections[entry].firstTable == i) {
                    html += "<li><a href='#section-" + QString::number(entry) + "'><b>" +
                        htmlText(dictionary.sections[entry].title) + "</b></a></li>";
                    ++entry;
                }
                if (i < dictionary.tables.size())
                    html += "<li><a href='#object-" + QString::number(i) + "'>" + htmlText(dictionary.tables[i].name) + "</a></li>";
            }
            html += "</ol></div>";
        }
    }
    if (section == DictionaryPreviewSection::Introduction || (all && layout.introduction)) {
        if (!layout.introduction) html += "<p>已隐藏引言</p>";
        else html += "<a name='introduction'></a><h2>引言</h2>" +
            paragraph("p", dictionaryText(layout.introductionText, dictionary), layout.bodyFont);
    }
    if (all || section == DictionaryPreviewSection::Body) {
        if (layout.header) html += paragraph("p", layout.headerText.isEmpty() ? title : dictionaryText(layout.headerText,dictionary), layout.headerFont);
        const auto renderRow = [&](const QStringList& values, bool header) {
            const QString tag = header ? "th" : "td";
            html += "<tr>";
            for (const auto& value : values)
                html += "<" + tag + " style=\"" + fontCss(layout.bodyFont) + (header ? "font-weight:700;background-color:" + heading : QString{}) +
                    "\">" + htmlText(value) + "</" + tag + ">";
            html += "</tr>";
        };
        int entry = 0;
        for (int i = 0; i <= dictionary.tables.size(); ++i) {
            checkCancel(cancel);
            while (entry < dictionary.sections.size() && dictionary.sections[entry].firstTable == i) {
                const auto& item = dictionary.sections[entry];
                html += "<a name='section-" + QString::number(entry++) + "'></a>" + paragraph("h2", item.title,layout.headingFont) +
                    paragraph("p", item.description,layout.bodyFont);
            }
            if (i == dictionary.tables.size()) break;
            if (!all && i > 0) break;
            const auto& table = dictionary.tables[i];
            html += "<a name='object-" + QString::number(i) + "'></a>" + paragraph("h3",table.name,layout.objectFont);
            if (layout.comments && !table.comment.isEmpty()) html += paragraph("p",table.comment,layout.bodyFont);
            if (!table.columns.isEmpty()) {
                html += "<table cellspacing='0' cellpadding='5'><thead>";
                renderRow({"字段","数据类型","约束","默认值","注释"},true);
                html += "</thead><tbody>";
                for (const auto& item : table.columns)
                    renderRow({item.name,item.type,item.primaryKey ? QString("PK %1 · %2").arg(item.primaryKey).arg(item.nullable ? "可空" : "非空")
                        : item.nullable ? "可空" : "非空",item.defaultExpression,layout.comments ? item.comment : QString{}},false);
                html += "</tbody></table>";
            }
            if (layout.details) for (auto kind : {TableObjectKind::Index,TableObjectKind::ForeignKey,TableObjectKind::Check,TableObjectKind::Trigger}) {
                bool any = false;
                for (const auto& object : table.objects) if (object.kind == kind) {
                    if (!any) {
                        html += "<h4>" + tableObjectKindName(kind) + "</h4><table cellspacing='0' cellpadding='5'><thead>";
                        renderRow({"名称","字段","定义或引用","规则","注释"},true);
                        html += "</thead><tbody>";
                        any = true;
                    }
                    auto cells = dictionaryObjectCells(object);
                    if (!layout.comments) cells[4].clear();
                    renderRow(cells,false);
                }
                if (any) html += "</tbody></table>";
            }
            if (html.size() > 16 * 1024 * 1024) throw std::runtime_error("数据字典预览过大，请缩小范围");
        }
        if (layout.footer) html += paragraph("p",dictionaryText(layout.footerText, dictionary),layout.footerFont);
    }
    return html + "</body></html>";
}

QJsonObject dictionaryConfigurationJson(const DictionaryConfiguration& config) {
    const auto& layout = config.layout;
    QJsonObject values;
    for (const auto& field : textSettings) values[field.key] = layout.*field.member;
    for (const auto& field : switchSettings) values[field.key] = layout.*field.member;
    for (const auto& field : fontSettings) {
        const auto& font = layout.*field.member;
        values[field.key] = QJsonObject{{"family", font.family},{"size", font.size},{"bold", font.bold}};
    }
    values["theme"] = int(layout.theme);
    values["paper"] = int(layout.paper);
    values["landscape"] = layout.orientation == QPageLayout::Landscape;
    values["margins"] = QJsonArray{layout.margins.left(),layout.margins.top(),layout.margins.right(),layout.margins.bottom()};
    values["background"] = int(layout.background);
    values["fit"] = int(layout.imageFit);
    values["opacity"] = layout.opacity;
    values["logo"] = imageData(layout.logo);
    values["coverImage"] = imageData(layout.coverImage);
    QJsonArray scopes;
    for (const auto& scope : config.scopes)
        scopes.append(QJsonObject{{"connection",scope.id},{"database",scope.database},{"schema",scope.schema},{"info",scope.info},
                                  {"order",QJsonArray::fromStringList(scope.order)},{"omitted",QJsonArray::fromStringList(scope.omitted)}});
    QJsonObject servers;
    for (auto it = config.serverInfo.cbegin(); it != config.serverInfo.cend(); ++it) servers[it.key()] = it.value();
    return {{"format","navidog-dictionary"},{"version",1},{"layout",values},{"scopes",scopes},{"serverInfo",servers}};
}

DictionaryConfiguration dictionaryConfigurationFromJson(const QJsonObject& json) {
    if (json.value("format").toString() != "navidog-dictionary" || json.value("version").toInt() != 1 ||
        !json.value("layout").isObject() || !json.value("scopes").isArray())
        throw std::runtime_error("无法识别此数据字典配置文件");
    DictionaryConfiguration config;
    auto& layout = config.layout;
    const auto values = json.value("layout").toObject();
    for (const auto& field : textSettings) layout.*field.member = values.value(field.key).toString(layout.*field.member).left(12000);
    for (const auto& field : switchSettings) layout.*field.member = values.value(field.key).toBool(layout.*field.member);
    for (const auto& field : fontSettings) {
        auto& font = layout.*field.member;
        const auto value = values.value(field.key).toObject();
        font.family = value.value("family").toString().left(120);
        font.size = qBound(6,value.value("size").toInt(font.size),52);
        font.bold = value.value("bold").toBool(font.bold);
    }
    layout.theme = DictionaryTheme(qBound(0,values.value("theme").toInt(int(layout.theme)),4));
    const auto paper = QPageSize::PageSizeId(values.value("paper").toInt(int(layout.paper)));
    if (paper == QPageSize::A4 || paper == QPageSize::A3 || paper == QPageSize::Letter || paper == QPageSize::Legal) layout.paper = paper;
    if (values.value("landscape").toBool()) layout.orientation = QPageLayout::Landscape;
    const auto margins = values.value("margins").toArray();
    if (margins.size() == 4) {
        const auto at = [&](int i) { return qBound(6.,margins[i].toDouble(19),40.); };
        layout.margins = {at(0),at(1),at(2),at(3)};
    }
    layout.background = DictionaryBackground(qBound(0,values.value("background").toInt(),2));
    layout.imageFit = DictionaryImageFit(qBound(0,values.value("fit").toInt(),2));
    layout.opacity = qBound(0,values.value("opacity").toInt(100),100);
    layout.logo = decodeImage(values.value("logo"));
    layout.coverImage = decodeImage(values.value("coverImage"));
    const auto scopes = json.value("scopes").toArray();
    QSet<QString> seen;
    for (const auto& entry : scopes) {
        const auto object = entry.toObject();
        DictionaryScopeChoice scope{object.value("connection").toString().left(512),object.value("database").toString().left(512),
                                    object.value("schema").toString().left(512),object.value("info").toBool(true),{},{}};
        const auto key = dictionaryScopeKey(scope);
        if (scope.id.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        for (const auto& name : object.value("order").toArray()) scope.order << name.toString();
        for (const auto& name : object.value("omitted").toArray()) scope.omitted << name.toString();
        config.scopes.push_back(std::move(scope));
    }
    const auto servers = json.value("serverInfo").toObject();
    for (auto it = servers.begin(); it != servers.end(); ++it) config.serverInfo[it.key()] = it.value().toBool();
    return config;
}

}
