#include "query_parameter_dialog.hpp"
#include "components.hpp"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>

namespace navidog::ui {
namespace {
using Type = QueryParameterType;

QString example(Type type) {
    switch (type) {
    case Type::Integer: return "例如 42";
    case Type::ExactNumber: return "例如 12.345";
    case Type::Real: return "例如 1.5e3";
    case Type::Boolean: return "true / false";
    case Type::Date: return "YYYY-MM-DD";
    case Type::Time: return "HH:MM:SS";
    case Type::Timestamp: return "YYYY-MM-DDTHH:MM:SS";
    case Type::Binary: return "例如 00ABFF";
    default: return {};
    }
}
}

std::optional<QVector<QVariant>> promptQueryParameters(QWidget* owner, const QVector<QueryParameter>& parameters) {
    Dialog dialog(owner);
    dialog.setObjectName("queryParameterDialog");
    dialog.setWindowTitle("绑定 SQL 参数");
    dialog.resize(870, qMin(580, 177 + int(parameters.size()) * 42));
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel("输入参数值后执行。勾选 NULL 可传入空值，文本框留空会传入空字符串。", &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget(scroll);
    auto* grid = new QGridLayout(page);
    grid->setContentsMargins(0, 2, 4, 2);
    grid->setColumnStretch(3, 1);
    const QStringList headings{"参数", "位置", "类型", "值", "NULL"};
    for (int column = 0; column < headings.size(); ++column) {
        auto* heading = new QLabel(headings[column], page);
        auto font = heading->font();
        font.setBold(true);
        heading->setFont(font);
        grid->addWidget(heading, 0, column);
    }
    scroll->setWidget(page);
    layout->addWidget(scroll, 1);
    auto* error = new QLabel(&dialog);
    error->setObjectName("queryParameterInputError");
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    layout->addWidget(error);

    struct Input { Select* type; TextInput* value; CheckBox* isNull; };
    QVector<Input> inputs;
    inputs.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        const int row = int(inputs.size()) + 1;
        auto* name = new QLabel(parameter.name, page);
        name->setTextFormat(Qt::PlainText);
        name->setToolTip(parameter.name);
        name->setMaximumWidth(220);
        name->setWordWrap(true);
        auto* type = new Select(page);
        type->setObjectName("queryParameterType");
        type->setAccessibleName(parameter.name + " 类型");
        type->addItem("待指定", int(Type::Unknown));
        for (const auto item : {Type::Text, Type::Integer, Type::ExactNumber, Type::Real, Type::Boolean,
                                Type::Date, Type::Time, Type::Timestamp, Type::Binary})
            type->addItem(queryParameterTypeName(item), int(item));
        type->setCurrentIndex(type->findData(int(parameter.type)));
        type->setToolTip(parameter.evidence.isEmpty() ? "SQL 和本地元数据无法确定类型，请选择" : parameter.evidence);
        auto* value = new TextInput(page);
        value->setObjectName("queryParameterValue");
        value->setAccessibleName(parameter.name + " 值");
        value->setMaxLength(512 * 1024);
        value->setPlaceholderText(example(parameter.type));
        auto* isNull = new CheckBox(page);
        isNull->setObjectName("queryParameterNull");
        isNull->setAccessibleName(parameter.name + " 为 NULL");
        QObject::connect(type, qOverload<int>(&QComboBox::currentIndexChanged), value, [type, value] {
            value->setPlaceholderText(example(Type(type->currentData().toInt())));
        });
        QObject::connect(isNull, &QCheckBox::toggled, value, &QWidget::setDisabled);
        const auto locationText = [](const QueryParameterLocation& location) {
            return QString("第 %1 条 · %2:%3").arg(location.statement).arg(location.line).arg(location.byteColumn);
        };
        auto location = locationText(parameter.locations.front());
        if (parameter.locations.size() > 1) location += QString(" +%1").arg(parameter.locations.size() - 1);
        auto* places = new QLabel(location, page);
        QStringList references;
        for (const auto& reference : parameter.locations.mid(0, 20)) references << locationText(reference);
        places->setToolTip("行号:字节列，以本次执行的 SQL 为起点\n" + references.join('\n'));
        grid->addWidget(name, row, 0);
        grid->addWidget(places, row, 1);
        grid->addWidget(type, row, 2);
        grid->addWidget(value, row, 3);
        grid->addWidget(isNull, row, 4, Qt::AlignHCenter);
        inputs << Input{type, value, isNull};
    }
    grid->setRowStretch(int(inputs.size()) + 1, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* run = new PushButton("执行", buttons);
    run->setObjectName("confirmQueryParameterBindings");
    run->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    buttons->addButton(run, QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);
    QVector<QVariant> values;
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        values.clear();
        qint64 bytes = 0;
        for (int i = 0; i < inputs.size(); ++i) {
            const auto& input = inputs[i];
            const auto type = Type(input.type->currentData().toInt());
            const auto text = input.value->text();
            const auto parsed = parseQueryParameterValue(type, text, input.isNull->isChecked());
            if (!parsed) {
                error->setText(type == Type::Unknown
                    ? QString("请选择 %1 的类型").arg(parameters[i].name)
                    : QString("%1 的值不符合「%2」格式").arg(parameters[i].name, queryParameterTypeName(type)));
                auto* field = type == Type::Unknown ? static_cast<QWidget*>(input.type) : input.value;
                scroll->ensureWidgetVisible(field);
                field->setFocus();
                return;
            }
            if (!input.isNull->isChecked()) bytes += qint64(text.size()) * 2;
            if (bytes > 8 * 1024 * 1024) { error->setText("本批参数超过 8 MiB，请分批执行"); return; }
            values << *parsed;
        }
        dialog.accept();
    });
    if (!inputs.isEmpty()) {
        if (parameters.front().type == Type::Unknown) inputs.front().type->setFocus();
        else inputs.front().value->setFocus();
    }
    if (dialog.exec() != QDialog::Accepted) return {};
    return values;
}
}
