#include "editor.hpp"
#include <QInputDialog>
#include <QMessageBox>
#include <QTextBlock>

QString SqlEditor::selectedSqlOrAll() const {
    const auto cursor = textCursor();
    return cursor.hasSelection()
        ? cursor.selectedText().replace(QChar::ParagraphSeparator, '\n')
        : toPlainText();
}

void SqlEditor::indentLines(bool remove){transformLines(remove?1:0);}
void SqlEditor::toggleLineComment(){transformLines(2);}

void SqlEditor::transformLines(int operation){
    if(isReadOnly())return;
    auto cursor = textCursor();
    const int low = cursor.selectionStart(), high = cursor.selectionEnd();
    const auto first = document()->findBlock(low);
    auto last = document()->findBlock(high);
    if (high > low && high == last.position())
        last = last.previous();
    const int start = first.position(), end = last.position() + last.length() - 1;
    if (end - start > 4 * 1024 * 1024) {
        QMessageBox::information(this, "SQL 编辑", "请缩小需要调整的行范围。");
        return;
    }
    QTextCursor range(first);
    range.setPosition(end, QTextCursor::KeepAnchor);
    const auto original = range.selectedText().replace(QChar::ParagraphSeparator, '\n');
    auto lines = original.split('\n', Qt::KeepEmptyParts);
    bool uncomment=operation==2,hasText=false;
    if(operation==2)for(const auto& line:lines){int first=0;while(first<line.size() && line.at(first).isSpace())++first;if(first==line.size())continue;hasText=true;if(line.mid(first,2)!="--")uncomment=false;}
    if(operation==2 && !hasText)return;
    int mappedPosition=cursor.position(),offset=start;
    for(auto& line:lines){
        const int oldLength=line.size();int at=0,removed=0;QString added;
        if(operation==0)added=QString(indentWidth_,' ');
        else if(operation==1){if(line.startsWith('\t'))removed=1;else while(removed<indentWidth_ && removed<line.size() && line.at(removed)==' ')++removed;}
        else {
            while(at<line.size() && line.at(at).isSpace())++at;
            if(at<line.size()){
                if(uncomment){removed=2;if(at+2<line.size() && line.at(at+2)==' ')++removed;}
                else added="-- ";
            }
        }
        if(cursor.position()>=offset+at){
            const int distance=cursor.position()-(offset+at);
            mappedPosition+=added.size()-qMin(removed,distance);
        }
        line.replace(at,removed,added);offset+=oldLength+1;
    }
    const auto replacement = lines.join('\n');
    if (replacement == original)
        return;
    if (replacement.toUtf8().size() > 4 * 1024 * 1024) {
        QMessageBox::information(this, "SQL 编辑", "请缩小需要调整的行范围。");
        return;
    }
    const bool selection=cursor.hasSelection(),backward=cursor.position()<cursor.anchor();
    cursor.beginEditBlock();cursor.setPosition(start);cursor.setPosition(end,QTextCursor::KeepAnchor);cursor.insertText(replacement);cursor.endEditBlock();
    if(selection){cursor.setPosition(backward?start+int(replacement.size()):start);cursor.setPosition(backward?start:start+int(replacement.size()),QTextCursor::KeepAnchor);}
    else cursor.setPosition(qBound(0,mappedPosition,document()->characterCount()-1));
    setTextCursor(cursor);
}

void SqlEditor::goToLine(){
    bool accepted=false;const int line=QInputDialog::getInt(this,"转到行","行号：",textCursor().blockNumber()+1,1,blockCount(),1,&accepted);
    if(!accepted)return;auto block=document()->findBlockByNumber(line-1);if(!block.isValid())return;setTextCursor(QTextCursor(block));ensureCursorVisible();setFocus();
}
