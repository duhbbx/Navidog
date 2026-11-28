#pragma once
#include <QTextCodec>
#include <QString>
#include <stdexcept>
namespace navidog {
class ClientCodec {
    QTextCodec* codec_;
public:
    explicit ClientCodec(QString name="UTF8") {
        auto lower=name.toLower();if(lower=="utf8mb4" || lower=="utf8mb3" || lower=="utf8")name="UTF-8";else if(lower=="latin1")name="Windows-1252";else if(lower=="sjis")name="Shift_JIS";else if(lower=="win1251")name="Windows-1251";else if(lower=="win1252")name="Windows-1252";
        codec_=QTextCodec::codecForName(name.toLatin1());if(!codec_)throw std::runtime_error(("不支持的客户端字符集："+name).toStdString());
    }
    QString decode(const char* data,int size=-1)const{return size<0?codec_->toUnicode(data):codec_->toUnicode(data,size);}
    QString decodeStrict(const char* data, int size) const {
        if (!size) return QStringLiteral("");
        QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
        auto text = codec_->toUnicode(data, size, &state);
        if (state.invalidChars || state.remainingChars)
            throw std::runtime_error("result contains text with invalid client encoding");
        return text;
    }
    QByteArray encode(const QString& text)const {
        QTextCodec::ConverterState state;auto bytes=codec_->fromUnicode(text.constData(),text.size(),&state);
        if(state.invalidChars)
            throw std::runtime_error("SQL 中的字符无法用所选客户端字符集表示");
        return bytes;
    }
};
}
