#include "editor.hpp"
#include "components.hpp"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QShortcut>
#include <QHideEvent>
#include <QTextBlock>

namespace {
constexpr int maxDocumentBytes=4*1024*1024;
constexpr int maxReplacements=100000;
bool wordCharacter(QChar c){return c.isLetterOrNumber() || c=='_' || c=='$' || c.category()==QChar::Mark_NonSpacing || c.category()==QChar::Mark_SpacingCombining;}
bool wordBoundary(const QString& text,int position,int length){
    return (position==0 || !wordCharacter(text.at(position-1))) &&
           (position+length==text.size() || !wordCharacter(text.at(position+length)));
}
int locate(const QString& text,const QString& needle,int from,bool backward,Qt::CaseSensitivity cs,bool wholeWord){
    if(needle.isEmpty() || from<0)return -1;
    int at=backward?text.lastIndexOf(needle,from,cs):text.indexOf(needle,from,cs);
    while(at>=0 && wholeWord && !wordBoundary(text,at,needle.size())){
        if(backward){if(at==0)return -1;at=text.lastIndexOf(needle,at-1,cs);}
        else at=text.indexOf(needle,at+1,cs);
    }
    return at;
}
int locateInBlocks(const QTextDocument &document, const QString &needle, int from,
                   bool backward, Qt::CaseSensitivity cs, bool wholeWord) {
    if (from < 0)
        return -1;
    auto block = document.findBlock(from);
    if (!block.isValid() && backward)
        block = document.lastBlock();
    for (; block.isValid(); block = backward ? block.previous() : block.next()) {
        const auto text = block.text();
        int start = backward ? qMin(from - block.position(), int(text.size()))
                             : qMax(0, from - block.position());
        const int at = locate(text, needle, start, backward, cs, wholeWord);
        if (at >= 0)
            return block.position() + at;
    }
    return -1;
}

}

void SqlEditor::showFind(bool replace){
    if(!findDialog_){
        findDialog_=new navidog::ui::Dialog(this);findDialog_->setObjectName("sqlFindDialog");
        findDialog_->setWindowTitle("查找 SQL");findDialog_->setModal(false);findDialog_->resize(560,210);
        auto* layout=new QVBoxLayout(findDialog_);layout->setSpacing(10);auto* form=new QFormLayout;form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setLabelAlignment(Qt::AlignLeft);
        findText_=new navidog::ui::TextInput;findText_->setObjectName("sqlFindText");findText_->setMaxLength(4096);form->addRow("查找：",findText_);layout->addLayout(form);
        replaceRow_=new QWidget;auto* replaceLayout=new QFormLayout(replaceRow_);replaceLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);replaceLayout->setLabelAlignment(Qt::AlignLeft);replaceLayout->setContentsMargins(0,0,0,0);
        replaceText_=new navidog::ui::TextInput;replaceText_->setObjectName("sqlReplaceText");replaceText_->setMaxLength(65536);replaceLayout->addRow("替换为：",replaceText_);layout->addWidget(replaceRow_);
        auto* flags=new QHBoxLayout;findCase_=new navidog::ui::CheckBox("区分大小写");findCase_->setObjectName("sqlFindCase");findWord_=new navidog::ui::CheckBox("整词匹配");findWord_->setObjectName("sqlFindWord");flags->addWidget(findCase_);flags->addWidget(findWord_);flags->addStretch();layout->addLayout(flags);
        auto* buttons=new QHBoxLayout;auto add=[&](const QString& text,const QString& name,auto callback){auto* button=new navidog::ui::PushButton(text);button->setObjectName(name);connect(button,&QPushButton::clicked,this,callback);buttons->addWidget(button);return button;};
        add("上一个","sqlFindPrevious",[this]{findNext(true);});auto* next=add("下一个","sqlFindNext",[this]{findNext();});next->setDefault(true);
        auto* one=add("替换","sqlReplaceOne",[this]{replaceCurrent();});auto* all=add("全部替换","sqlReplaceAll",[this]{replaceAll();});
        one->setProperty("replaceAction",true);all->setProperty("replaceAction",true);
        add("关闭","sqlFindClose",[this]{findDialog_->hide();setFocus();});layout->addLayout(buttons);
        findStatus_=new QLabel;findStatus_->setObjectName("sqlFindStatus");findStatus_->setWordWrap(true);findStatus_->setTextFormat(Qt::PlainText);layout->addWidget(findStatus_);
        connect(findText_,&QLineEdit::textChanged,findStatus_,&QLabel::clear);
        auto* previous=new QShortcut(QKeySequence("Shift+Return"),findDialog_);connect(previous,&QShortcut::activated,this,[this]{findNext(true);});
        auto* nextMatch=new QShortcut(QKeySequence::FindNext,findDialog_);connect(nextMatch,&QShortcut::activated,this,[this]{findNext();});
        auto* previousMatch=new QShortcut(QKeySequence::FindPrevious,findDialog_);connect(previousMatch,&QShortcut::activated,this,[this]{findNext(true);});
    }
    const auto cursor = textCursor();
    const auto selected = cursor.selectionEnd() - cursor.selectionStart() <= 4096
        ? cursor.selectedText() : QString{};
    if(!selected.isEmpty() && selected.size()<=4096 && !selected.contains(QChar::ParagraphSeparator) && !selected.contains(QChar::LineSeparator))findText_->setText(selected);
    replaceRow_->setVisible(replace);
    for(auto* button:findDialog_->findChildren<QPushButton*>())if(button->property("replaceAction").toBool())button->setVisible(replace);
    findDialog_->setWindowTitle(replace?"查找与替换 SQL":"查找 SQL");
    findDialog_->show();findDialog_->raise();findDialog_->activateWindow();findText_->setFocus();findText_->selectAll();
}

bool SqlEditor::findNext(bool backward){
    if(!findDialog_){showFind();return false;}
    const auto needle=findText_->text();if(needle.isEmpty()){findStatus_->setText("请输入要查找的文本。");return false;}
    const auto cursor = textCursor();
    const auto cs = findCase_->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const bool wholeWord = findWord_->isChecked();
    const auto text = largeDocument() ? QString{} : toPlainText();
    const auto search = [&](int from) {
        return largeDocument() ? locateInBlocks(*document(), needle, from, backward, cs, wholeWord)
                               : locate(text, needle, from, backward, cs, wholeWord);
    };
    int at = search(backward ? cursor.selectionStart() - 1 : cursor.selectionEnd());
    bool wrapped = false;
    if (at < 0) {
        at = search(backward ? document()->characterCount() - 1 : 0);
        wrapped = at >= 0;
    }
    if(at<0){findStatus_->setText("未找到匹配文本。");return false;}
    QTextCursor match(document());match.setPosition(at);match.setPosition(at+needle.size(),QTextCursor::KeepAnchor);setTextCursor(match);ensureCursorVisible();
    findStatus_->setText(wrapped?(backward?"已从文末继续查找。":"已从文首继续查找。"):"已找到匹配文本。");return true;
}

void SqlEditor::replaceCurrent(){
    if(!findDialog_){showFind(true);return;}
    if(isReadOnly()){findStatus_->setText("当前编辑器为只读。");return;}
    const auto needle=findText_->text();if(needle.isEmpty()){findStatus_->setText("请输入要查找的文本。");return;}
    auto cursor = textCursor();
    const auto cs = findCase_->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const auto begin = cursor.selectionStart(), end = cursor.selectionEnd();
    const bool boundary = (begin == 0 || !wordCharacter(document()->characterAt(begin - 1))) &&
        (end == document()->characterCount() - 1 || !wordCharacter(document()->characterAt(end)));
    if (end - begin != needle.size() || cursor.selectedText().compare(needle, cs) != 0 ||
        (findWord_->isChecked() && !boundary)) {
        findNext();
        return;
    }
    const auto replacement = replaceText_->text();
    if (!largeDocument()) {
        auto changed = toPlainText();
        changed.replace(begin, end - begin, replacement);
        if (changed.size() > maxDocumentBytes || changed.toUtf8().size() > maxDocumentBytes) {
            findStatus_->setText("替换后的 SQL 超过 4 MiB，未修改。");
            return;
        }
    }
    cursor.beginEditBlock();cursor.insertText(replacement);cursor.endEditBlock();setTextCursor(cursor);findNext();
    findStatus_->setText("已替换 1 处。");
}

void SqlEditor::replaceAll(){
    if(!findDialog_){showFind(true);return;}
    if(isReadOnly()){findStatus_->setText("当前编辑器为只读。");return;}
    const auto needle=findText_->text();if(needle.isEmpty()){findStatus_->setText("请输入要查找的文本。");return;}
    if (largeDocument()) {
        findStatus_->setText("大文件模式暂停全部替换，可逐处查找和替换。");
        return;
    }
    const auto text=toPlainText();if(text.size()>maxDocumentBytes || text.toUtf8().size()>maxDocumentBytes){findStatus_->setText("当前 SQL 超过 4 MiB，未修改。");return;}
    const auto replacement=replaceText_->text();const auto cs=findCase_->isChecked()?Qt::CaseSensitive:Qt::CaseInsensitive;
    QString changed;changed.reserve(text.size());int from=0,count=0;
    for(int at=locate(text,needle,from,false,cs,findWord_->isChecked());at>=0;at=locate(text,needle,from,false,cs,findWord_->isChecked())){
        if(++count>maxReplacements){findStatus_->setText("匹配超过 100000 处，未修改；请缩小查找范围。");return;}
        if(qint64(changed.size())+at-from+replacement.size()>maxDocumentBytes){findStatus_->setText("替换后的 SQL 超过 4 MiB，未修改。");return;}
        changed+=text.mid(from,at-from);changed+=replacement;from=at+needle.size();
    }
    if(count==0){findStatus_->setText("未找到匹配文本。");return;}
    if(qint64(changed.size())+text.size()-from>maxDocumentBytes){findStatus_->setText("替换后的 SQL 超过 4 MiB，未修改。");return;}
    changed+=text.mid(from);if(changed.toUtf8().size()>maxDocumentBytes){findStatus_->setText("替换后的 SQL 超过 4 MiB，未修改。");return;}
    const auto oldPosition=textCursor().position();auto cursor=textCursor();cursor.beginEditBlock();cursor.select(QTextCursor::Document);cursor.insertText(changed);cursor.endEditBlock();cursor.setPosition(qMin(oldPosition,int(changed.size())));setTextCursor(cursor);
    findStatus_->setText(QString("已替换 %1 处，可一次撤销。").arg(count));
}

void SqlEditor::hideEvent(QHideEvent* event){dismissCompletion();if(findDialog_)findDialog_->hide();QPlainTextEdit::hideEvent(event);}
