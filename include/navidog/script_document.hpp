#pragma once
#include <QString>
#include <QByteArray>
#include <stdexcept>
namespace navidog {
inline constexpr int defaultScriptBytes = 4 * 1024 * 1024;
inline constexpr int editableScriptBytes = 128 * 1024 * 1024;

enum class ScriptEncoding { Utf8,Utf8Bom,Utf16LE,Utf16BE };
struct ScriptDocument {
    QString path,text;
    QByteArray fingerprint;
    ScriptEncoding encoding=ScriptEncoding::Utf8;
    bool crlf=false;
    qint64 byteSize = 0;
};
class ScriptConflict final:public std::runtime_error {
public:ScriptConflict():std::runtime_error("磁盘上的 SQL 文件已被修改或删除"){}
};
ScriptDocument readScriptDocument(const QString& path, int maxBytes = defaultScriptBytes);
// A short, non-editable prefix for the editor while the full file is loading.
QString readScriptPrefix(const QString &path);
// Atomic save; existing associated files require their original fingerprint.
// allowChanged is used only after a user chooses overwrite / Save As.
ScriptDocument writeScriptDocument(const ScriptDocument&,const QString& path,const QString& text,bool allowChanged=false, int maxBytes = defaultScriptBytes);
}
