#include <QUrl>
#include "report_export.hpp"
#include "er_canvas.hpp"
#include "navidog/database_tools.hpp"
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPageLayout>
#include <QPdfWriter>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace navidog::ui {
namespace {
[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toStdString());
}

void write(QIODevice& target,const QByteArray& bytes) {
    if (target.write(bytes) != bytes.size()) fail(target.errorString());
}

constexpr quint32 maxZipPart = 0xffffff00;
quint32 crc32(quint32 state,const QByteArray& bytes) {
    static const auto table = [] {
        std::array<quint32,256> values{};
        for (quint32 i = 0; i < values.size(); ++i) {
            quint32 crc = i;
            for (int j = 0; j < 8; ++j)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
            values[i] = crc;
        }
        return values;
    }();
    for (unsigned char byte : bytes) state = table[(state^byte)&255] ^ (state>>8);
    return state;
}

// Stored ZIP entries allow the Word parts to be streamed without keeping all
// generated PNGs or the document XML resident in memory.
class Zip {
public:
    explicit Zip(const QString& path) : file_(path), stream_(&file_) {
        if (!file_.open(QIODevice::WriteOnly)) fail(file_.errorString());
        stream_.setByteOrder(QDataStream::LittleEndian);
    }
    void add(const QByteArray& name,const QByteArray& bytes) {
        addHeader(name,quint32(bytes.size()),crc32(0xffffffffu,bytes)^0xffffffffu);
        write(file_,bytes);
    }
    void add(const QByteArray& name,QFile& source,const Cancellation& cancel) {
        if (source.size() >= maxZipPart || !source.seek(0)) fail("Word 部件超过文件大小上限");
        quint32 crc = 0xffffffffu;
        const auto consume = [&](const auto& visitor) {
            while (!source.atEnd()) {
                if (cancel->load()) fail("导出已停止");
                const auto bytes = source.read(256*1024);
                if (bytes.isEmpty()) fail(source.errorString());
                visitor(bytes);
            }
        };
        consume([&](const QByteArray& bytes) { crc = crc32(crc,bytes); });
        addHeader(name,quint32(source.size()),crc^0xffffffffu);
        if (!source.seek(0)) fail(source.errorString());
        consume([&](const QByteArray& bytes) { write(file_,bytes); });
    }
    void finish() {
        auto start = offset();
        for (const auto& entry : entries_) {
            stream_ << quint32(0x02014b50) << quint16(20) << quint16(20)
                    << quint16(0) << quint16(0) << quint16(0) << quint16(33)
                    << entry.crc << entry.bytes << entry.bytes
                    << quint16(entry.name.size()) << quint16(0) << quint16(0)
                    << quint16(0) << quint16(0) << quint32(0) << entry.position;
            write(file_,entry.name);
        }
        const auto length = offset()-start;
        stream_ << quint32(0x06054b50) << quint16(0) << quint16(0)
                << quint16(entries_.size()) << quint16(entries_.size()) << length << start << quint16(0);
        if (stream_.status() != QDataStream::Ok || !file_.commit()) fail("无法完成 Word 文档");
    }
private:
    struct Entry { QByteArray name; quint32 bytes,crc,position; };
    QSaveFile file_;
    QDataStream stream_;
    QVector<Entry> entries_;
    quint32 offset() {
        if (file_.pos() >= maxZipPart) fail("Word 文档超过 4 GB，请拆分模型后导出");
        return quint32(file_.pos());
    }
    void addHeader(const QByteArray& name,quint32 size,quint32 crc) {
        if (entries_.size() >= 65000 || quint64(offset())+size >= maxZipPart)
            fail("Word 文档超过文件大小或部件数量上限");
        entries_.push_back({name,size,crc,offset()});
        stream_ << quint32(0x04034b50) << quint16(20) << quint16(0)
                << quint16(0) << quint16(0) << quint16(33) << crc << size << size
                << quint16(name.size()) << quint16(0);
        write(file_,name);
    }
};

const QByteArray xmlHead = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
const QByteArray officeRelationships = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
const QByteArray packageRelationships = "http://schemas.openxmlformats.org/package/2006/relationships";
#ifdef Q_OS_MACOS
const QByteArray eastAsiaFont = "Arial Unicode MS";
#elif defined(Q_OS_WIN)
const QByteArray eastAsiaFont = "Microsoft YaHei";
#else
const QByteArray eastAsiaFont = "Noto Sans CJK SC";
#endif
QByteArray xml(QString text) {
    for (auto& c : text) {
        if (c.unicode() < 32 && c != '\n' && c != '\r' && c != '\t') c = QChar(0xfffd);
    }
    return text.toHtmlEscaped().toUtf8();
}

using FieldRow = std::array<QString,5>;
const FieldRow fieldHeading{"字段","数据类型","约束","默认值","注释"};
const FieldRow objectHeading{"名称","字段","定义或引用","规则","注释"};
const auto objectKinds = {TableObjectKind::Index, TableObjectKind::ForeignKey, TableObjectKind::Check, TableObjectKind::Trigger};
FieldRow reportObjectRow(const TableObject& object, bool comments) {
    const auto cells = dictionaryObjectCells(object);
    return {cells[0],cells[1],cells[2],cells[3],comments ? cells[4] : QString{}};
}
QByteArray hex(const QColor& color) { return color.name().mid(1).toLatin1().toUpper(); }
constexpr std::array<int,5> fieldWidths{22,18,14,18,28};
constexpr int documentWidth = 9506; // A4 width minus both side margins in twips.
const QColor bodyColor("#243544");
const QColor secondaryColor("#526578");
const QColor gridColor("#D4DFE6");
const QColor headingFill("#DCE9ED");
const QColor alternateFill("#F4F7F9");
const QColor contentsLinkColor("#235778");

struct ContentsEntry {
    QString title;
    bool group;
};

QByteArray bookmark(int id) { return "NavidogEr"+QByteArray::number(id); }

FieldRow fieldRow(const TableColumn& column) {
    QString constraints = column.nullable ? "可空" : "非空";
    if (column.primaryKey) constraints.prepend(QString("PK %1 · ").arg(column.primaryKey));
    return {column.name,column.type,constraints,column.defaultExpression,column.comment};
}

template<class Width>
std::array<Width,6> fieldBoundaries(Width width) {
    std::array<Width,6> stops{};
    int percent = 0;
    for (size_t i = 0; i < fieldWidths.size(); ++i) {
        percent += fieldWidths[i];
        stops[i+1] = width*percent/100;
    }
    return stops;
}

QByteArray wordRun(QString value) {
    value.replace("\r\n","\n");
    value.replace('\r','\n');
    QByteArray run;
    const auto lines = value.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (i) run += "<w:br/>";
        run += "<w:t xml:space=\"preserve\">"+xml(lines[i])+"</w:t>";
    }
    return run;
}

QByteArray wordText(const QString& value,const QByteArray& properties = {}) {
    return "<w:r><w:rPr><w:rFonts w:eastAsia=\""+eastAsiaFont+"\" w:hint=\"eastAsia\"/>"+properties+
           "<w:lang w:eastAsia=\"zh-CN\"/></w:rPr>"+wordRun(value)+"</w:r>";
}

QByteArray wordFont(const DictionaryFont& font) {
    const auto size = QByteArray::number(font.size*2);
    return "<w:sz w:val=\""+size+"\"/><w:szCs w:val=\""+size+"\"/>"+
        (font.bold ? "<w:b/>" : "<w:b w:val=\"0\"/>");
}

QByteArray styledWordText(const QString& value, const DictionaryFont& font, const QColor& color) {
    if (font.family.isEmpty()) return wordText(value, wordFont(font)+"<w:color w:val=\""+hex(color)+"\"/>");
    const auto family = xml(font.family);
    return "<w:r><w:rPr><w:rFonts w:ascii=\""+family+"\" w:hAnsi=\""+family+"\" w:eastAsia=\""+family+
        "\"/>"+wordFont(font)+"<w:color w:val=\""+hex(color)+"\"/></w:rPr>"+wordRun(value)+"</w:r>";
}

class Docx {
public:
    Docx(const QString& path, const QString& title, const DictionaryLayout* options = nullptr)
        : archive_(path), options_(options) {
        if (options_) {
            const auto layout = options_->pageLayout();
            const auto rect = layout.fullRect(QPageLayout::Point);
            pageWidth_ = qRound(rect.width()*20);
            pageHeight_ = qRound(rect.height()*20);
            const auto margins = layout.margins(QPageLayout::Point);
            margins_ = {qRound(margins.left()*20),qRound(margins.top()*20),
                        qRound(margins.right()*20),qRound(margins.bottom()*20)};
            width_ = pageWidth_ - margins_.left() - margins_.right();
        }
        if (!body_.open() || !rels_.open()) fail("无法创建 Word 临时部件");
        archive_.add("[Content_Types].xml",xmlHead+
            "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Default Extension=\"png\" ContentType=\"image/png\"/>"
            "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
            "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
             "<Override PartName=\"/word/footer.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml\"/>" +
            (options_ && options_->header ? QByteArray("<Override PartName=\"/word/header.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml\"/>") : QByteArray{}) +
            "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>"
            "</Types>");
        archive_.add("_rels/.rels",xmlHead+"<Relationships xmlns=\""+packageRelationships+
            "\"><Relationship Id=\"rId1\" Type=\""+officeRelationships+
            "officeDocument\" Target=\"word/document.xml\"/></Relationships>");
        write(body_,xmlHead+
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
            " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
            " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
            " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
            " xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><w:body>");
        write(rels_,xmlHead+"<Relationships xmlns=\""+packageRelationships+
            "\"><Relationship Id=\"rStyles\" Type=\""+officeRelationships+"styles\" Target=\"styles.xml\"/>"
            "<Relationship Id=\"rFooter\" Type=\""+officeRelationships+"footer\" Target=\"footer.xml\"/>"
             "<Relationship Id=\"rSettings\" Type=\""+officeRelationships+"settings\" Target=\"settings.xml\"/>");
        if (options_ && options_->header)
            write(rels_,"<Relationship Id=\"rHeader\" Type=\""+officeRelationships+"header\" Target=\"header.xml\"/>");
        archive_.add("word/settings.xml",xmlHead+
            "<w:settings xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
            "<w:updateFields w:val=\"true\"/></w:settings>");
        const QByteArray pageField = "<w:fldSimple w:instr=\"PAGE\"><w:r><w:t>1</w:t></w:r></w:fldSimple>";
        const QByteArray footerText = options_ && options_->footer
            ? styledWordText(options_->footerText,options_->footerFont,secondaryColor) : QByteArray{};
        const auto footerPage = !options_ || options_->pageNumbers ? "<w:r><w:tab/></w:r>"+pageField : QByteArray{};
        archive_.add("word/footer.xml",xmlHead+
            "<w:ftr xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
            "<w:p><w:pPr><w:tabs><w:tab w:val=\"right\" w:pos=\""+QByteArray::number(width_)+"\"/></w:tabs></w:pPr>"+
            footerText+footerPage+"</w:p></w:ftr>");
        if (options_ && options_->header)
            archive_.add("word/header.xml",xmlHead+
                "<w:hdr xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:p>"+
                styledWordText(options_->headerText,options_->headerFont,secondaryColor)+"</w:p></w:hdr>");
        archive_.add("word/styles.xml",xmlHead+
            "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
            "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\" w:eastAsia=\""+eastAsiaFont+"\"/>"
            "<w:color w:val=\"243544\"/><w:sz w:val=\"20\"/><w:lang w:val=\"en-US\" w:eastAsia=\"zh-CN\"/></w:rPr></w:rPrDefault>"
            "<w:pPrDefault><w:pPr><w:spacing w:after=\"80\" w:line=\"276\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
            "<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:pPr><w:keepNext/><w:spacing w:after=\"180\"/></w:pPr>"
            "<w:rPr><w:b/><w:color w:val=\"000000\"/><w:sz w:val=\"48\"/></w:rPr></w:style>"
            "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/>"
            "<w:pPr><w:keepNext/><w:spacing w:before=\"240\" w:after=\"140\"/><w:outlineLvl w:val=\"0\"/></w:pPr>"
            "<w:rPr><w:b/><w:color w:val=\"000000\"/><w:sz w:val=\"34\"/></w:rPr></w:style>"
            "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/>"
            "<w:pPr><w:keepNext/><w:spacing w:before=\"240\" w:after=\"85\"/><w:outlineLvl w:val=\"1\"/></w:pPr>"
            "<w:rPr><w:b/><w:color w:val=\"000000\"/><w:sz w:val=\"25\"/></w:rPr></w:style>"
            "<w:style w:type=\"paragraph\" w:styleId=\"ReportNote\"><w:name w:val=\"Report Note\"/>"
            "<w:rPr><w:color w:val=\"526578\"/><w:sz w:val=\"19\"/></w:rPr></w:style>"
            "<w:style w:type=\"paragraph\" w:styleId=\"ReportLabel\"><w:name w:val=\"Report Label\"/>"
            "<w:pPr><w:keepNext/><w:spacing w:before=\"120\" w:after=\"50\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"20\"/></w:rPr></w:style>"
            "<w:style w:type=\"paragraph\" w:styleId=\"ReportContents\"><w:name w:val=\"Report Contents\"/>"
            "<w:pPr><w:keepNext/><w:spacing w:before=\"320\" w:after=\"180\"/></w:pPr>"
            "<w:rPr><w:b/><w:color w:val=\"000000\"/><w:sz w:val=\"34\"/></w:rPr></w:style>"
            "</w:styles>");
        if (!title.isEmpty()) text(title,"Title");
    }
    void text(const QString& line,const QByteArray& style = {},bool pageBreak = false,bool keepNext = false,
              int anchor = 0, const DictionaryFont* font = nullptr, const QColor& color = bodyColor) {
        const auto p = style.isEmpty() ? QByteArray() : "<w:pPr><w:pStyle w:val=\""+style+"\"/>"+
            (keepNext ? "<w:keepNext/>" : "")+(pageBreak ? "<w:pageBreakBefore/>" : "")+"</w:pPr>";
        const auto id = QByteArray::number(anchor);
        write(body_,"<w:p>"+p+(anchor ? "<w:bookmarkStart w:id=\""+id+"\" w:name=\""+bookmark(anchor)+"\"/>" : QByteArray{})+
            (font ? styledWordText(line,*font,color) : wordText(line))+(anchor ? "<w:bookmarkEnd w:id=\""+id+"\"/>" : QByteArray{})+"</w:p>");
    }
    void contents(const QVector<ContentsEntry>& entries,const Cancellation& cancel, bool pageBreak = false) {
        text("目录","ReportContents",pageBreak);
        for (int i = 0; i < entries.size(); ++i) {
            if (cancel->load()) fail("导出已停止");
            const auto& entry = entries[i];
            const auto target = bookmark(i+1);
            const QByteArray group = entry.group ? "<w:keepNext/>" : "";
            const QByteArray emphasis = entry.group ? "<w:b/>" : "";
            write(body_,"<w:p><w:pPr>"+group+"<w:keepLines/><w:tabs>"
                "<w:tab w:val=\"right\" w:leader=\"dot\" w:pos=\""+QByteArray::number(width_)+"\"/>"
                "</w:tabs><w:spacing w:before=\""+(entry.group ? "120" : "0")+"\" w:after=\"30\"/>"
                "<w:ind w:left=\""+(entry.group ? "0" : "300")+"\"/></w:pPr><w:hyperlink w:anchor=\""+target+"\">"+
                wordText(entry.title,emphasis+"<w:color w:val=\"235778\"/>")+"</w:hyperlink><w:r><w:tab/></w:r>"
                "<w:fldSimple w:instr=\"PAGEREF "+target+" \\h\" w:dirty=\"true\"><w:r><w:t></w:t></w:r></w:fldSimple></w:p>");
        }
    }
    void fields(const QVector<TableColumn>& columns,const Cancellation& cancel) {
        grid(fieldHeading,columns.size(),[&](int i) {
            auto row = fieldRow(columns[i]);
            if (options_ && !options_->comments) row[4].clear();
            return row;
        },cancel);
    }
    void objects(const QVector<TableObject>& objects, const Cancellation& cancel) {
        if (!options_ || !options_->details) return;
        for (const auto kind : objectKinds) {
            QVector<const TableObject*> matching;
            for (const auto& object : objects) if (object.kind == kind) matching.push_back(&object);
            if (matching.isEmpty()) continue;
            text(tableObjectKindName(kind),"ReportLabel",false,true,0,&options_->objectFont);
            grid(objectHeading,matching.size(),[&](int i) { return reportObjectRow(*matching[i],options_->comments); },cancel);
        }
    }
    void grid(const FieldRow& headings, int count, const std::function<FieldRow(int)>& values, const Cancellation& cancel) {
        if (!count) return;
        const auto stops = fieldBoundaries(width_);
        const auto colors = options_ ? dictionaryColors(options_->theme) : DictionaryColors{contentsLinkColor,headingFill,alternateFill,gridColor};
        const auto border = hex(colors.grid);
        write(body_,"<w:tbl><w:tblPr><w:tblW w:w=\""+QByteArray::number(width_)+"\" w:type=\"dxa\"/>"
            "<w:jc w:val=\"center\"/><w:tblBorders>"
            "<w:top w:val=\"single\" w:sz=\"6\" w:color=\""+border+"\"/><w:left w:val=\"single\" w:sz=\"4\" w:color=\""+border+"\"/>"
            "<w:bottom w:val=\"single\" w:sz=\"6\" w:color=\""+border+"\"/><w:right w:val=\"single\" w:sz=\"4\" w:color=\""+border+"\"/>"
            "<w:insideH w:val=\"single\" w:sz=\"4\" w:color=\""+border+"\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:color=\""+border+"\"/>"
            "</w:tblBorders><w:tblLayout w:type=\"fixed\"/><w:tblCellMar><w:top w:w=\"62\" w:type=\"dxa\"/>"
            "<w:left w:w=\"115\" w:type=\"dxa\"/><w:bottom w:w=\"62\" w:type=\"dxa\"/><w:right w:w=\"115\" w:type=\"dxa\"/>"
            "</w:tblCellMar></w:tblPr><w:tblGrid>");
        for (size_t i = 0; i < fieldWidths.size(); ++i)
            write(body_,"<w:gridCol w:w=\""+QByteArray::number(stops[i+1]-stops[i])+"\"/>");
        write(body_,"</w:tblGrid>");
        const auto row = [&](const FieldRow& cells,bool header,bool tinted) {
            write(body_,header ? "<w:tr><w:trPr><w:cantSplit/><w:tblHeader/></w:trPr>" : "<w:tr><w:trPr><w:cantSplit/></w:trPr>");
            const QByteArray fill = header ? hex(colors.heading) : tinted ? hex(colors.stripe) : QByteArray("FFFFFF");
            for (size_t i = 0; i < cells.size(); ++i)
                write(body_,"<w:tc><w:tcPr><w:tcW w:w=\""+QByteArray::number(stops[i+1]-stops[i])+"\" w:type=\"dxa\"/>"
                    "<w:shd w:val=\"clear\" w:fill=\""+fill+"\"/><w:vAlign w:val=\"top\"/></w:tcPr><w:p><w:pPr>"+
                    (header ? "<w:keepNext/>" : "")+"<w:spacing w:before=\"0\" w:after=\"0\" w:line=\"264\" w:lineRule=\"auto\"/>"
                     "</w:pPr>"+cellText(cells[i],header)+"</w:p></w:tc>");
            write(body_,"</w:tr>");
        };
        row(headings,true,false);
        for (int i = 0; i < count; ++i) {
            if (cancel->load()) fail("导出已停止");
            row(values(i),false,i%2 != 0);
        }
        write(body_,"</w:tbl>");
    }
    void image(const QString& path,const QByteArray& name,const QSize& size,const QString& title,
               const Cancellation& cancel, int maxWidth = 0) {
        QFile png(path);
        if (!png.open(QIODevice::ReadOnly)) fail(png.errorString());
        archive_.add("word/media/"+name,png,cancel);
        const auto id = QByteArray::number(++images_);
        const auto rid = "rImg"+id;
        write(rels_,"<Relationship Id=\""+rid+"\" Type=\""+officeRelationships+
                    "image\" Target=\"media/"+name+"\"/>");
        const qreal scale = qMin((maxWidth ? qMin(width_,maxWidth) : width_)*635.0/size.width(),5900000.0/size.height());
        const auto width = QByteArray::number(qint64(size.width()*scale));
        const auto height = QByteArray::number(qint64(size.height()*scale));
        const auto extent = " cx=\""+width+"\" cy=\""+height+"\"";
        write(body_,"<w:p><w:pPr><w:keepLines/><w:spacing w:after=\"160\"/><w:jc w:val=\"center\"/></w:pPr><w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                    "<wp:extent"+extent+"/><wp:docPr id=\""+id+"\" name=\""+name+"\" descr=\""+xml(title)+"\"/>"
                    "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                    "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"0\" name=\""+name+"\"/><pic:cNvPicPr/></pic:nvPicPr>"
                    "<pic:blipFill><a:blip r:embed=\""+rid+"\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                    "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext"+extent+"/></a:xfrm>"
                    "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>"
                    "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>");
    }
    void picture(const QImage& art,const QByteArray& name,const Cancellation& cancel,int maxWidth = 0) {
        if (art.isNull()) return;
        QTemporaryFile png;
        if (!png.open() || !art.save(&png,"PNG") || !png.flush()) fail("无法生成封面图片");
        image(png.fileName(),name,art.size(),{},cancel,maxWidth);
    }
    void finish(const Cancellation& cancel) {
        const auto number = [](int n) { return QByteArray::number(n); };
        write(body_,"<w:sectPr>"+
            QByteArray(options_ && options_->header ? "<w:headerReference w:type=\"default\" r:id=\"rHeader\"/>" : "")+
            "<w:footerReference w:type=\"default\" r:id=\"rFooter\"/><w:pgSz w:w=\""+number(pageWidth_)+"\" w:h=\""+number(pageHeight_)+
            (options_ && options_->orientation == QPageLayout::Landscape ? "\" w:orient=\"landscape" : "")+"\"/>"
            "<w:pgMar w:top=\""+number(margins_.top())+"\" w:right=\""+number(margins_.right())+"\" w:bottom=\""+
            number(margins_.bottom())+"\" w:left=\""+number(margins_.left())+"\" w:header=\"270\" w:footer=\"270\" w:gutter=\"0\"/>"+
            (options_ && options_->cover ? "<w:titlePg/>" : "")+"</w:sectPr></w:body></w:document>");
        write(rels_,"</Relationships>");
        archive_.add("word/document.xml",body_,cancel);
        archive_.add("word/_rels/document.xml.rels",rels_,cancel);
        archive_.finish();
    }
private:
    QByteArray cellText(const QString& text, bool header) const {
        if (!options_) return wordText(text,(header ? QByteArray("<w:b/>") : QByteArray{})+"<w:sz w:val=\"19\"/>");
        auto font = options_->bodyFont;
        font.bold |= header;
        return styledWordText(text,font,bodyColor);
    }
    Zip archive_;
    const DictionaryLayout* options_;
    int width_ = documentWidth, pageWidth_ = 11906, pageHeight_ = 16838;
    QMargins margins_{1200,1100,1200,1100};
    QTemporaryFile body_, rels_;
    int images_ = 0;
};

class PdfReport {
public:
    PdfReport(const QString& path,const QString& title,bool measure = false,
              const DictionaryLayout* options = nullptr)
        : file_(path),writer_(std::make_unique<QPdfWriter>(&file_)),
          baseFont_(QGuiApplication::font()), options_(options),
          colors_(options ? dictionaryColors(options->theme) : DictionaryColors{contentsLinkColor,headingFill,alternateFill,gridColor}) {
        writer_->setTitle(title);
        writer_->setResolution(110);
        writer_->setPageLayout(options_ ? options_->pageLayout() : QPageLayout(QPageSize(QPageSize::A4),QPageLayout::Portrait,
                                            QMarginsF(16,16,16,16),QPageLayout::Millimeter));
        if (measure) return;
        if (!file_.open(QIODevice::WriteOnly)) fail(file_.errorString());
        if (!painter_.begin(writer_.get())) fail("无法开始 PDF 绘制");
    }
    int page() const { return page_; }
    int text(const QString& value,int size = 15,bool bold = false,bool secondary = false,qreal reserve = 0,int anchor = 0,
             const DictionaryFont* style = nullptr) {
        if (value.isEmpty()) return page_;
        auto layout = textLayout(value,style ? styledFont(*style) : font(size,bold),writer_->width(),anchor);
        const qreal spacing = bold ? 10 : 5;
        const auto height = layout->boundingRect().height();
        if (cursor_ > 0 && cursor_+height+spacing+reserve > contentBottom()) newPage();
        const auto firstPage = page_;
        for (int i = 0; i < layout->lineCount(); ++i) {
            const auto line = layout->lineAt(i);
            if (cursor_+line.height() > contentBottom()) newPage();
            if (painter_.isActive()) {
                painter_.setPen(secondary ? secondaryColor : bold ? (options_ ? colors_.accent : QColor(Qt::black)) : bodyColor);
                line.draw(&painter_,QPointF(0,cursor_-line.y()));
            }
            cursor_ += line.height();
        }
        cursor_ += spacing;
        return firstPage;
    }
    void beginBody() {
        body_ = true;
        runningHeader();
    }
    void nextPage() { newPage(); }
    void artwork(const QImage& image, qreal widthFraction, qreal heightFraction) {
        if (image.isNull()) return;
        auto size = image.size();
        size.scale(qRound(writer_->width()*widthFraction),qRound(contentBottom()*heightFraction),Qt::KeepAspectRatio);
        if (cursor_+size.height()+8 > contentBottom()) newPage();
        if (painter_.isActive()) {
            painter_.setRenderHint(QPainter::SmoothPixmapTransform);
            painter_.drawImage(QRectF(0,cursor_,size.width(),size.height()),image);
        }
        cursor_ += size.height()+8;
    }
    void contents(const QVector<ContentsEntry>& entries,const QVector<int>& bodyPages,int offset,const Cancellation& cancel) {
        cursor_ += 16;
        text("目录",25,true,false,72);
        for (int i = 0; i < entries.size(); ++i) {
            if (cancel->load()) fail("导出已停止");
            const auto& entry = entries[i];
            const auto style = font(15,entry.group);
            const qreal left = entry.group ? 0 : 22;
            const qreal right = writer_->width();
            const auto target = i+1;
            auto label = textLayout(entry.title,style,right-left-64,-target);
            const qreal gap = entry.group ? 9 : 2;
            if (cursor_+gap+label->boundingRect().height()+5+(entry.group ? 23 : 0) > contentBottom())
                continueContents();
            cursor_ += gap;
            for (int j = 0; j < label->lineCount(); ++j) {
                const auto line = label->lineAt(j);
                if (cursor_+line.height()+5 > contentBottom()) continueContents();
                if (painter_.isActive()) {
                    painter_.setPen(colors_.accent);
                    line.draw(&painter_,QPointF(left,cursor_-line.y()));
                    if (j+1 == label->lineCount()) {
                        const auto number = QString::number(bodyPages[i]+offset);
                        const auto width = QFontMetricsF(style,writer_.get()).horizontalAdvance(number);
                        const auto numberLeft = right-width;
                        const auto leaderStart = left+line.naturalTextWidth()+9;
                        const auto lineY = cursor_+line.ascent()-2;
                        if (leaderStart < numberLeft-9) {
                            painter_.setPen(QPen(colors_.grid,1,Qt::DotLine));
                            painter_.drawLine(QPointF(leaderStart,lineY),QPointF(numberLeft-9,lineY));
                        }
                        painter_.setPen(colors_.accent);
                        textLayout(number,style,width+2,-target)->draw(&painter_,QPointF(numberLeft,cursor_));
                    }
                }
                cursor_ += line.height();
            }
            cursor_ += 5;
        }
        newPage();
    }
    int group(const QString& name,const QImage& image,bool separate,int anchor) {
        if (separate) newPage();
        else cursor_ += 14;
        auto size = image.size();
        size.scale(writer_->width(),int(contentBottom()*.55),Qt::KeepAspectRatio);
        const auto headingPage = text(name,25,true,false,size.height()+16,anchor);
        const QRectF area((writer_->width()-size.width())/2.,cursor_,size.width(),size.height());
        if (painter_.isActive()) {
            painter_.setRenderHint(QPainter::SmoothPixmapTransform);
            painter_.drawImage(area,image);
        }
        cursor_ += size.height()+14;
        return headingPage;
    }
    int tableTitle(const QString& name,bool hasFields,int anchor) {
        cursor_ += 12;
        return text(name,19,true,false,hasFields ? 70 : 0,anchor,options_ ? &options_->objectFont : nullptr);
    }
    void objectTitle() {
        cursor_ += 8;
        text("表对象",15,true,false,25);
    }
    void fields(const QVector<TableColumn>& columns,const Cancellation& cancel) {
        grid(fieldHeading,columns.size(),[&](int i) {
            auto row = fieldRow(columns[i]);
            if (options_ && !options_->comments) row[4].clear();
            return row;
        },cancel);
    }
    void objects(const QVector<TableObject>& objects,const Cancellation& cancel) {
        if (!options_ || !options_->details) return;
        for (const auto kind : objectKinds) {
            QVector<const TableObject*> matching;
            for (const auto& object : objects) if (object.kind == kind) matching.push_back(&object);
            if (matching.isEmpty()) continue;
            text(tableObjectKindName(kind),15,true,false,50,0,&options_->objectFont);
            grid(objectHeading,matching.size(),[&](int i) { return reportObjectRow(*matching[i],options_->comments); },cancel);
        }
    }
    void grid(const FieldRow& headings, int count, const std::function<FieldRow(int)>& values, const Cancellation& cancel) {
        if (!count) return;
        const auto stops = fieldBoundaries(qreal(writer_->width()));
        auto font = options_ ? styledFont(options_->bodyFont) : this->font(15,true);
        font.setBold(true);
        const auto header = layoutRow(headings,font,stops);
        font.setBold(options_ && options_->bodyFont.bold);
        auto firstRow = layoutRow(values(0),font,stops);
        const auto fullPage = contentBottom()-rowHeight(header);
        const auto firstHeight = rowHeight(firstRow) <= fullPage ? rowHeight(firstRow) : 2*cellMarginY+firstRow.stride;
        if (cursor_ + rowHeight(header) + firstHeight > contentBottom()) newPage();
        drawRow(header,stops,0,header.lines,colors_.heading);
        for (int i = 0; i < count; ++i) {
            if (cancel->load()) fail("导出已停止");
            const auto row = i == 0 ? std::move(firstRow) : layoutRow(values(i),font,stops);
            const auto color = i%2 == 0 ? QColor(Qt::white) : colors_.stripe;
            if (rowHeight(row) <= fullPage && cursor_ > rowHeight(header) &&
                cursor_ + rowHeight(row) > contentBottom()) {
                newPage();
                drawRow(header,stops,0,header.lines,colors_.heading);
            }
            for (int first = 0; first < row.lines;) {
                int available = int(std::floor((contentBottom()-cursor_-2*cellMarginY)/row.stride));
                if (available <= 0) {
                    newPage();
                    drawRow(header,stops,0,header.lines,colors_.heading);
                    available = int(std::floor((contentBottom()-cursor_-2*cellMarginY)/row.stride));
                    if (available <= 0) fail("PDF 页面无法容纳字段行");
                }
                const int lines = std::min(available,row.lines-first);
                drawRow(row,stops,first,lines,color);
                first += lines;
            }
        }
    }
    void finish() {
        footer();
        if (!painter_.end()) fail("无法完成 PDF 绘制");
        writer_.reset();
        if (!file_.commit()) fail(file_.errorString());
    }
private:
    QSaveFile file_;
    std::unique_ptr<QPdfWriter> writer_;
    QPainter painter_;
    QFont baseFont_;
    const DictionaryLayout* options_;
    DictionaryColors colors_;
    bool body_ = false;
    qreal cursor_ = 0;
    int page_ = 1;
    static constexpr qreal cellMarginX = 9;
    static constexpr qreal cellMarginY = 5;
    using Stops = std::array<qreal,6>;
    struct RowLayout {
        std::array<std::unique_ptr<QTextLayout>,5> cells;
        int lines = 1;
        qreal stride = 0;
    };
    qreal contentBottom() const {
        if (!options_) return writer_->height()-35;
        return writer_->height() - (body_ && (options_->footer || options_->pageNumbers)
            ? qMax(26, QFontMetrics(styledFont(options_->footerFont),writer_.get()).height()+10) : 0);
    }
    static qreal rowHeight(const RowLayout& row) { return 2*cellMarginY+row.stride*row.lines; }
    void continueContents() {
        newPage();
        text("目录（续）",19,true);
    }

    QFont font(int size,bool bold) const {
        auto value = baseFont_;
        value.setPixelSize(size);
        value.setBold(bold);
        return value;
    }
    QFont styledFont(const DictionaryFont& style) const {
        auto value = font(qRound(style.size*110./72.),style.bold);
        if (!style.family.isEmpty()) value.setFamily(style.family);
        return value;
    }
    std::unique_ptr<QTextLayout> textLayout(QString value,const QFont& style,qreal width,int anchor) const {
        value.replace("\r\n","\n");
        value.replace('\r','\n');
        value.replace('\n',QChar(QChar::LineSeparator));
        auto layout = std::make_unique<QTextLayout>(value,style,writer_.get());
        QTextOption wrap;
        wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout->setTextOption(wrap);
#if QT_VERSION >= QT_VERSION_CHECK(6,8,0)
        if (anchor && !value.isEmpty()) {
            QTextCharFormat format;
            format.setAnchor(true);
            const auto target = QString::fromLatin1(bookmark(std::abs(anchor)));
            if (anchor > 0) format.setAnchorNames({target});
            else format.setAnchorHref("#"+target);
            const auto length = anchor > 0 ? int(QTextBoundaryFinder(QTextBoundaryFinder::Grapheme,value).toNextBoundary()) : int(value.size());
            layout->setFormats({{0,length,format}});
        }
#else
        Q_UNUSED(anchor);
#endif
        layout->beginLayout();
        qreal y = 0;
        for (auto line = layout->createLine(); line.isValid(); line = layout->createLine()) {
            line.setLineWidth(width);
            line.setPosition({0,y});
            y += line.height();
        }
        layout->endLayout();
        return layout;
    }

    RowLayout layoutRow(const FieldRow& values,const QFont& font,const Stops& stops) const {
        RowLayout row;
        row.stride = std::ceil(QFontMetricsF(font,writer_.get()).lineSpacing())+2;
        for (size_t i = 0; i < values.size(); ++i) {
            if (values[i].isEmpty()) continue;
            auto cell = textLayout(values[i],font,stops[i+1]-stops[i]-2*cellMarginX,0);
            for (int j = 0; j < cell->lineCount(); ++j)
                row.stride = std::max(row.stride,std::ceil(cell->lineAt(j).height())+2);
            row.lines = std::max(row.lines,cell->lineCount());
            row.cells[i] = std::move(cell);
        }
        return row;
    }
    void drawRow(const RowLayout& row,const Stops& stops,int first,int count,const QColor& fill) {
        const qreal height = 2*cellMarginY+row.stride*count;
        if (!painter_.isActive()) { cursor_ += height; return; }
        painter_.fillRect(QRectF(0,cursor_,stops.back(),height),fill);
        painter_.setPen(QPen(colors_.grid,.8));
        for (auto stop : stops) painter_.drawLine(QPointF(stop,cursor_),QPointF(stop,cursor_+height));
        painter_.drawLine(QPointF(0,cursor_+height),QPointF(stops.back(),cursor_+height));
        if (fill == colors_.heading) painter_.drawLine(QPointF(0,cursor_),QPointF(stops.back(),cursor_));
        painter_.setPen(bodyColor);
        for (size_t i = 0; i < row.cells.size(); ++i) {
            const auto& cell = row.cells[i];
            if (!cell) continue;
            for (int j = first; j < std::min(first+count,cell->lineCount()); ++j) {
                const auto line = cell->lineAt(j);
                line.draw(&painter_,QPointF(stops[i]+cellMarginX,cursor_+cellMarginY+(j-first)*row.stride-line.y()));
            }
        }
        cursor_ += height;
    }
    void runningHeader() {
        if (!body_ || !options_ || !options_->header || options_->headerText.isEmpty()) return;
        auto style = styledFont(options_->headerFont);
        const auto height = QFontMetricsF(style,writer_.get()).height() + 5;
        if (painter_.isActive()) {
            painter_.setPen(secondaryColor);
            painter_.setFont(style);
            const auto text = QFontMetricsF(style,writer_.get()).elidedText(options_->headerText,Qt::ElideRight,writer_->width());
            painter_.drawText(QRectF(0,cursor_,writer_->width(),height),Qt::AlignTop,text);
        }
        cursor_ += height + 10;
    }
    void footer() {
        const bool pageNumber = !options_ || (body_ && options_->pageNumbers);
        const bool left = options_ && body_ && options_->footer && !options_->footerText.isEmpty();
        if (!pageNumber && !left) return;
        const auto style = options_ ? styledFont(options_->footerFont) : font(13,false);
        painter_.setFont(style);
        painter_.setPen(secondaryColor);
        const auto y = contentBottom()+5;
        const auto height = writer_->height()-y;
        if (left) {
            const auto width = writer_->width() - (pageNumber ? 50 : 0);
            const auto text = QFontMetricsF(style,writer_.get()).elidedText(options_->footerText,Qt::ElideRight,width);
            painter_.drawText(QRectF(0,y,width,height),Qt::AlignLeft,text);
        }
        if (pageNumber)
            painter_.drawText(QRectF(0,y,writer_->width(),height),Qt::AlignRight,QString::number(page_));
    }
    void newPage() {
        if (painter_.isActive()) {
            footer();
            if (!writer_->newPage()) fail("无法添加 PDF 页面");
        }
        ++page_;
        cursor_ = 0;
        runningHeader();
    }
};
}

void exportDatabaseDictionary(const DatabaseDictionary& dictionary, const QString& path,
                              ReportDocumentFormat format, const Cancellation& cancel) {
    DictionaryLayout layout;
    layout.cover = layout.introduction = layout.header = false;
    layout.title = "数据库字典";
    exportDatabaseDictionary(dictionary, layout, path, format, cancel);
}

void exportDatabaseDictionary(const DatabaseDictionary& dictionary, const DictionaryLayout& settings,
                              const QString& path, ReportDocumentFormat format, const Cancellation& cancel) {
    auto layout = settings;
    const auto title = dictionaryTitle(dictionary, layout);
    const auto scope = QString("%1  ·  %2 个对象").arg(dictionary.scope).arg(dictionary.tables.size());
    const auto accent = dictionaryColors(layout.theme).accent;
    layout.headerText = layout.headerText.isEmpty() ? title : dictionaryText(layout.headerText,dictionary);
    layout.footerText = dictionaryText(layout.footerText,dictionary);
    struct Section {
        QString title, description;
        const DatabaseDictionaryTable* table = nullptr;
    };
    QVector<Section> sections;
    if (layout.introduction) sections.push_back({"引言",dictionaryText(layout.introductionText,dictionary),nullptr});
    int heading = 0;
    for (int i = 0; i <= dictionary.tables.size(); ++i) {
        while (heading < dictionary.sections.size() && dictionary.sections[heading].firstTable == i) {
            const auto& section = dictionary.sections[heading++];
            sections.push_back({section.title,section.description,nullptr});
        }
        if (i < dictionary.tables.size())
            sections.push_back({dictionary.tables[i].name,dictionary.tables[i].comment,&dictionary.tables[i]});
    }
    QVector<ContentsEntry> contents;
    if (layout.contents)
        for (const auto& section : sections) contents.push_back({section.title,section.table == nullptr});
    const auto artwork = layout.cover ? dictionaryArtwork(layout) : QImage{};
    const auto contentsEnabled = !contents.isEmpty();
    const auto notes = dictionaryText(layout.notes,dictionary);
    if (format == ReportDocumentFormat::Word) {
        Docx report(path, {}, &layout);
        if (layout.cover) {
            report.picture(layout.logo,"cover-logo.png",cancel,1400);
            report.text(dictionaryText(layout.coverHeader,dictionary),"ReportNote",false,false,0,&layout.coverHeaderFont);
        }
        report.text(title,"Title",false,false,0,&layout.titleFont,accent);
        if (layout.cover)
            report.text(dictionaryText(layout.subtitle,dictionary),"ReportNote",false,false,0,&layout.subtitleFont);
        report.text(scope,"ReportNote",false,false,0,&layout.notesFont);
        if (layout.cover) {
            report.text(notes,"ReportNote",false,false,0,&layout.notesFont);
            report.picture(artwork,"cover-art.png",cancel);
            report.text(dictionaryText(layout.coverFooter,dictionary),"ReportNote",false,false,0,&layout.coverFooterFont);
        }
        if (contentsEnabled) report.contents(contents,cancel,layout.cover);
        int anchor = 0;
        for (const auto& section : sections) {
            if (cancel->load()) fail("导出已停止");
            const bool newPage = anchor == 0 && layout.cover;
            const auto* table = section.table;
            report.text(section.title,table ? "Heading2" : "Heading1",newPage,false,++anchor,
                        table ? &layout.objectFont : &layout.headingFont,accent);
            if (!section.description.isEmpty() && (!table || layout.comments))
                report.text(section.description,"ReportNote",false,table && !table->columns.isEmpty(),0,&layout.bodyFont);
            if (table) {
                report.fields(table->columns,cancel);
                report.objects(table->objects,cancel);
            }
        }
        report.finish(cancel);
        return;
    }
    const auto initialText = [&](PdfReport& report) {
        report.text(title,36,true,false,0,0,&layout.titleFont);
        report.text(scope,15,false,true,0,0,&layout.notesFont);
    };
    const auto printBody = [&](PdfReport& report) {
        if (!layout.cover && !contentsEnabled) initialText(report);
        QVector<int> pages;
        pages.reserve(sections.size());
        for (const auto& section : sections) {
            if (cancel->load()) fail("导出已停止");
            const auto* table = section.table;
            const auto anchor = int(pages.size())+1;
            pages.push_back(table ? report.tableTitle(section.title,!table->columns.isEmpty(),anchor)
                                  : report.text(section.title,25,true,false,55,anchor,&layout.headingFont));
            if (!section.description.isEmpty() && (!table || layout.comments))
                report.text(section.description,15,false,true,table && !table->columns.isEmpty() ? 56 : 0,0,&layout.bodyFont);
            if (table) {
                report.fields(table->columns,cancel);
                report.objects(table->objects,cancel);
            }
        }
        return pages;
    };
    PdfReport bodyLayout({},title,true,&layout);
    bodyLayout.beginBody();
    const auto pages = printBody(bodyLayout);
    const auto frontMatter = [&](PdfReport& report, int offset) {
        if (layout.cover) {
            report.artwork(layout.logo,0.2,0.12);
            report.text(dictionaryText(layout.coverHeader,dictionary),15,false,true,0,0,&layout.coverHeaderFont);
            report.text(title,36,true,false,0,0,&layout.titleFont);
            report.text(dictionaryText(layout.subtitle,dictionary),22,false,false,0,0,&layout.subtitleFont);
            report.text(scope,15,false,true,0,0,&layout.notesFont);
            report.text(notes,15,false,true,0,0,&layout.notesFont);
            report.artwork(artwork,1.,0.35);
            report.text(dictionaryText(layout.coverFooter,dictionary),15,false,true,0,0,&layout.coverFooterFont);
            if (!sections.isEmpty()) report.nextPage();
        }
        report.beginBody();
        if (contentsEnabled) {
            if (!layout.cover) initialText(report);
            report.contents(contents,pages,offset,cancel);
        }
    };
    PdfReport frontLayout({},title,true,&layout);
    frontMatter(frontLayout,0);
    const int offset = frontLayout.page()-1;
    PdfReport report(path,title,false,&layout);
    frontMatter(report,offset);
    const auto printed = printBody(report);
    for (int i = 0; i < pages.size(); ++i)
        if (printed[i] != pages[i]+offset) fail("PDF 目录与正文分页不一致");
    if (cancel->load()) fail("导出已停止");
    report.finish();
}

namespace er {
namespace {
namespace m = navidog::er;

QString objectDescription(const TableObject& object) {
    auto text = tableObjectKindName(object.kind) + " " + object.name;
    if (!object.columns.isEmpty()) text += " ("+object.columns.join(", ")+")";
    if (object.kind == TableObjectKind::ForeignKey)
        text += " → "+(object.referenceSchema.isEmpty() ? QString{} : object.referenceSchema+".")
             + object.referenceTable+" ("+object.referenceColumns.join(", ")+")";
    if (!object.expression.isEmpty()) text += " · " + object.expression;
    return text;
}

void pdfGroup(PdfReport& report,const m::Project& project,const m::Group& group,const QImage& image,
              const QVector<ContentsEntry>& entries,QVector<int>& pages,const Cancellation& cancel) {
    if (cancel->load()) fail("导出已停止");
    const auto id = int(pages.size());
    pages.push_back(report.group(entries[id].title,image,id>0,id+1));
    for (int i : group.members) {
        if (cancel->load()) fail("导出已停止");
        const auto& table = project.entities[i].table;
        const auto id = int(pages.size());
        pages.push_back(report.tableTitle(entries[id].title,!table.columns.isEmpty(),id+1));
        if (!table.comment.isEmpty()) report.text(table.comment,15,false,true,table.columns.isEmpty() ? 0 : 56);
        report.fields(table.columns,cancel);
        if (!table.objects.isEmpty()) report.objectTitle();
        for (const auto& object : table.objects) {
            if (cancel->load()) fail("导出已停止");
            report.text(objectDescription(object),14,false,true);
        }
    }
}

QString groupImageName(int id) { return QString("group-%1.png").arg(id,4,10,QChar('0')); }

QString safeMarkdown(QString text) {
    text.replace('&',"&amp;");
    text.replace('<',"&lt;");
    text.replace('>',"&gt;");
    text.replace('\\',"\\\\");
    for (QChar c : QString("*_[]#|`")) text.replace(c,QString("\\")+c);
    text.replace('\n'," ");
    text.replace('\r'," ");
    return text;
}
}

QString exportReport(const m::Project& project,const QString& destination,int formats,
                     const Cancellation& cancel,const std::function<void(int,int)>& progress,
                     const ExportContext& context) {
    const auto graph = m::makeGraph(project);
    const auto scene = m::arrange(project,graph);
    if (scene.groups.isEmpty()) fail("请先添加或导入数据库对象");
    if (!(formats & (Markdown|Pdf|Word))) fail("请选择导出格式");
    QDir output(destination);
    QTemporaryDir stage(output.filePath(".er-export-XXXXXX"));
    if (!stage.isValid()) fail("无法创建导出目录");
    QDir working(stage.path());
    if (!working.mkdir("images")) fail("无法创建 PNG 图目录");

    const auto timestamp = QDateTime::currentDateTime();
    const auto stem = exportStem(context,"ER文档",timestamp);
    const auto imageName = [&](int number) {
        const auto purpose = QString("ER图-分组%1").arg(number,4,10,QChar('0'));
        return exportStem(context,purpose,timestamp) + ".png";
    };
    QFile markdown(working.filePath(stem + ".md"));
    if ((formats & Markdown) && !markdown.open(QIODevice::WriteOnly)) fail(markdown.errorString());
    std::unique_ptr<Docx> word;
    std::unique_ptr<PdfReport> pdfLayout;
    if (formats & Word) word = std::make_unique<Docx>(working.filePath(stem + ".docx"),project.title);
    if (formats & Pdf) pdfLayout = std::make_unique<PdfReport>(QString{},project.title,true);
    QVector<ContentsEntry> contents;
    if (word || pdfLayout) {
        contents.reserve(graph.active.size()+scene.groups.size());
        for (const auto& group : scene.groups) {
            if (cancel->load()) fail("导出已停止");
            contents.push_back({group.name.isEmpty() ? QString("未分组") : group.name,true});
            for (int i : group.members)
                contents.push_back({m::displayName(project.entities[i].table.node,project.driver),false});
        }
    }
    const auto md = [&](const QString& value) {
        if (markdown.isOpen()) write(markdown,value.toUtf8());
    };
    const auto describe = [&](const QString& line) { md("- " + safeMarkdown(line) + "\n"); };
    md("# " + safeMarkdown(project.title) + "\n\n");
    const auto scope = QString("%1  ·  %2 个对象  ·  %3 个分组  ·  %4 条关系").arg(databaseKindName(project.driver))
                       .arg(graph.active.size()).arg(scene.groups.size()).arg(graph.edges.size());
    describe(scope);
    if (word) {
        word->text(scope,"ReportNote");
        word->contents(contents,cancel);
    }
    QVector<int> pdfBodyPages;
    const int groups = int(scene.groups.size());
    const int progressTotal = pdfLayout ? groups*2 : groups;
    int groupIndex = 0;
    int wordBookmark = 0;
    m::SpatialIndex index;
    index.reset(scene);
    for (const auto& group : scene.groups) {
        if (cancel->load()) fail("导出已停止");
        const auto name = imageName(++groupIndex);
        const auto path = working.filePath("images/"+name);
        const auto png = groupImage(project,graph,scene,index,group);
        if (!png.save(path,"PNG")) fail("无法写入分组 PNG");
        md("\n## " + safeMarkdown(group.name) + "\n\n![ER 图](images/"+QString::fromLatin1(QUrl::toPercentEncoding(name))+")\n");
        if (word) {
            ++wordBookmark;
            word->text(contents[wordBookmark-1].title,"Heading1",true,false,wordBookmark);
            word->image(path,groupImageName(groupIndex).toUtf8(),png.size(),group.name,cancel);
        }
        if (pdfLayout) pdfGroup(*pdfLayout,project,group,png,contents,pdfBodyPages,cancel);
        if (word || markdown.isOpen()) for (int i : group.members) {
            if (cancel->load()) fail("导出已停止");
            const auto& table = project.entities[i].table;
            const auto label = m::displayName(table.node,project.driver);
            md("\n### " + safeMarkdown(label) + "\n\n");
            if (word) word->text(label,"Heading2",false,false,++wordBookmark);
            if (!table.comment.isEmpty()) {
                describe(table.comment);
                if (word) word->text(table.comment,"ReportNote",false,!table.columns.isEmpty());
            }
            if (word) word->fields(table.columns,cancel);
            if (markdown.isOpen()) for (const auto& c : table.columns) {
                if (cancel->load()) fail("导出已停止");
                auto line = c.name + "  ·  " + c.type;
                if (c.primaryKey) line += QString("  ·  PK %1").arg(c.primaryKey);
                if (!c.nullable) line += "  ·  NOT NULL";
                if (!c.defaultExpression.isEmpty()) line += "  ·  DEFAULT " + c.defaultExpression;
                if (!c.comment.isEmpty()) line += "  ·  " + c.comment;
                describe(line);
            }
            if (word && !table.objects.isEmpty()) word->text("表对象","ReportLabel");
            for (const auto& o : table.objects) {
                if (cancel->load()) fail("导出已停止");
                const auto text = objectDescription(o);
                describe(text);
                if (word) word->text(text,"ReportNote");
            }
        }
        if (progress) progress(groupIndex,progressTotal);
    }
    if (markdown.isOpen() && !markdown.flush()) fail(markdown.errorString());
    markdown.close();
    if (word) word->finish(cancel);
    word.reset();
    if (pdfLayout) {
        const auto frontMatter = [&](PdfReport& report,int offset) {
            report.text(project.title,36,true);
            report.text(scope,15,false,true);
            report.contents(contents,pdfBodyPages,offset,cancel);
        };
        PdfReport frontLayout({},project.title,true);
        frontMatter(frontLayout,0);
        const int offset = frontLayout.page()-1;
        PdfReport report(working.filePath(stem + ".pdf"),project.title);
        frontMatter(report,offset);
        QVector<int> printedPages;
        groupIndex = 0;
        for (const auto& group : scene.groups) {
            if (cancel->load()) fail("导出已停止");
            const QImage png(working.filePath("images/"+imageName(++groupIndex)));
            if (png.isNull()) fail("无法读取分组 PNG");
            pdfGroup(report,project,group,png,contents,printedPages,cancel);
            if (progress) progress(groups+groupIndex,progressTotal);
        }
        for (int i = 0; i < pdfBodyPages.size(); ++i)
            if (printedPages[i] != pdfBodyPages[i]+offset) fail("PDF 目录与正文分页不一致");
        report.finish();
    }
    if (cancel->load()) fail("导出已停止");
    auto folder = stem;
    for (int copy = 2; true; ++copy) {
        if (output.rename(stage.path(),output.filePath(folder))) break;
        if (!output.exists(folder)) fail("无法发布导出文件夹");
        folder = exportStem(context,QString("ER文档-%1").arg(copy),timestamp);
    }
    stage.setAutoRemove(false);
    return output.filePath(folder);
}
}
}
