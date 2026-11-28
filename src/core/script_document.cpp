#include "navidog/script_document.hpp"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QTextCodec>
namespace navidog {
namespace {
QString limitLabel(int maxBytes){return QString::number(maxBytes / (1024 * 1024)) + " MiB";}
[[noreturn]] void fail(const QString& text){throw std::runtime_error(text.toStdString());}
QString absolutePath(const QString& path){QFileInfo info(path);if(info.exists())return info.canonicalFilePath();auto parent=QDir(info.absolutePath()).canonicalPath();return parent.isEmpty()?info.absoluteFilePath():QDir(parent).filePath(info.fileName());}
QByteArray contents(const QString &path, int maxBytes) {
    if (!QFileInfo(path).isFile())
        fail("SQL 路径不是普通文件");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail("无法读取 SQL 文件：" + file.errorString());
    const auto size = file.size();
    if (size > maxBytes)
        fail("SQL 文件超过 " + limitLabel(maxBytes));
    auto bytes = file.read(size + 1);
    if (!file.atEnd() && bytes.size() <= maxBytes)
        bytes += file.read(qint64(maxBytes) + 1 - bytes.size());
    if (file.error() != QFileDevice::NoError)
        fail(file.errorString());
    if (bytes.size() > maxBytes)
        fail("SQL 文件超过 " + limitLabel(maxBytes));
    return bytes;
}
QByteArray digest(const QByteArray& bytes){return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);}
QByteArray fileDigest(const QString &path, int maxBytes) {
    QFile file(path);
    if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly) || file.size() > maxBytes)
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 remaining = maxBytes;
    while (!file.atEnd()) {
        const auto chunk = file.read(qMin(remaining + 1, qint64(256 * 1024)));
        if (chunk.isEmpty() || chunk.size() > remaining)
            return {};
        remaining -= chunk.size();
        hash.addData(chunk);
    }
    return hash.result();
}
QByteArray codecName(ScriptEncoding encoding){return encoding==ScriptEncoding::Utf16LE?"UTF-16LE":encoding==ScriptEncoding::Utf16BE?"UTF-16BE":"UTF-8";}
int encodingHeader(const QByteArray &bytes, ScriptEncoding &encoding) {
    if (bytes.startsWith(QByteArray("\xef\xbb\xbf", 3))) {
        encoding = ScriptEncoding::Utf8Bom;
        return 3;
    }
    if (bytes.startsWith(QByteArray("\xff\xfe", 2))) {
        encoding = ScriptEncoding::Utf16LE;
        return 2;
    }
    if (bytes.startsWith(QByteArray("\xfe\xff", 2))) {
        encoding = ScriptEncoding::Utf16BE;
        return 2;
    }
    return 0;
}

}
ScriptDocument readScriptDocument(const QString& path, int maxBytes){
    ScriptDocument document;
    document.path = absolutePath(path);
    const auto bytes = contents(document.path, maxBytes);
    document.byteSize = bytes.size();
    document.fingerprint = digest(bytes);
    const int header = encodingHeader(bytes, document.encoding);
    QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);auto* codec=QTextCodec::codecForName(codecName(document.encoding));auto text=codec->toUnicode(bytes.constData()+header,bytes.size()-header,&state);
    if(state.invalidChars || state.remainingChars || text.contains(QChar::Null))fail("文件不是有效 UTF-8 / 带 BOM 的 UTF-16 SQL 文本，请先转换编码");
    for(int i=0;i<text.size();++i){if(text.at(i).isHighSurrogate()){if(i+1>=text.size() || !text.at(++i).isLowSurrogate())fail("UTF-16 文本包含不完整的字符，请先修复编码");}else if(text.at(i).isLowSurrogate())fail("UTF-16 文本包含不完整的字符，请先修复编码");}
    document.crlf=text.count("\r\n")>text.count('\n')/2;document.text=text.replace("\r\n","\n").replace('\r','\n');return document;
}
QString readScriptPrefix(const QString &path) {
    QFile file(path);
    if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly))
        return {};
    const auto bytes = file.read(32 * 1024);
    ScriptEncoding encoding = ScriptEncoding::Utf8;
    const int header = encodingHeader(bytes, encoding);
    QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
    const auto *codec = QTextCodec::codecForName(codecName(encoding));
    auto prefix = codec->toUnicode(bytes.constData() + header, bytes.size() - header, &state);
    if (state.invalidChars || prefix.contains(QChar::Null))
        return {};
    if (!prefix.isEmpty() && prefix.at(prefix.size() - 1).isHighSurrogate())
        prefix.chop(1);
    return prefix.replace("\r\n", "\n").replace('\r', '\n');
}

ScriptDocument writeScriptDocument(const ScriptDocument& original,const QString& path,const QString& text,bool allowChanged, int maxBytes){
    if(path.trimmed().isEmpty() || text.contains(QChar::Null) || text.size()>maxBytes)fail("SQL 文件名或文本内容无效，文本上限为 "+limitLabel(maxBytes));
    ScriptDocument document=original;document.path=absolutePath(path);auto normalized=text;normalized.replace("\r\n","\n").replace('\r','\n');document.text=normalized;if(document.crlf)normalized.replace("\n","\r\n");
    QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);auto* codec=QTextCodec::codecForName(codecName(document.encoding));auto bytes=codec->fromUnicode(normalized.constData(),normalized.size(),&state);if(state.invalidChars || bytes.size()>maxBytes)fail("SQL 文本无法编码或超过 "+limitLabel(maxBytes));
    if(document.encoding==ScriptEncoding::Utf8Bom)bytes.prepend(QByteArray::fromHex("efbbbf"));else if(document.encoding==ScriptEncoding::Utf16LE)bytes.prepend(QByteArray::fromHex("fffe"));else if(document.encoding==ScriptEncoding::Utf16BE)bytes.prepend(QByteArray::fromHex("feff"));
    if(bytes.size()>maxBytes)fail("SQL 文本超过 "+limitLabel(maxBytes));
    const bool exists=QFileInfo::exists(document.path);if(!allowChanged){if(original.path!=document.path){if(exists)throw ScriptConflict();}else if(!exists || original.fingerprint.isEmpty() || fileDigest(document.path,maxBytes)!=original.fingerprint)throw ScriptConflict();}
    QSaveFile file(document.path);if(!file.open(QIODevice::WriteOnly))fail("无法保存 SQL 文件："+file.errorString());if(!exists)file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);if(file.write(bytes)!=bytes.size() || !file.commit())fail("SQL 保存失败："+file.errorString());document.byteSize=bytes.size();document.fingerprint=digest(bytes);return document;
}
}
