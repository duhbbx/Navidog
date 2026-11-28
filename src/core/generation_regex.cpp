#include "generation_regex.hpp"
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace navidog::generation {
namespace {
constexpr int maximumLength = 8192;
constexpr int ceiling = maximumLength + 1;

[[noreturn]] void invalid(const QString& reason) {
    throw std::runtime_error(("正则生成规则：" + reason).toStdString());
}

int choose(std::mt19937_64& engine, int low, int high) {
    return std::uniform_int_distribution<int>(low, high)(engine);
}

QString letters() {
    return QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
}
QString digits() {
    return QStringLiteral("0123456789");
}
QString printable() {
    QString range;
    for (int i = 32; i < 127; ++i) range += QChar(i);
    return range;
}
}

struct Regex::Node {
    enum class Kind { Symbols, Sequence, Alternative, Repeat };
    Kind kind = Kind::Sequence;
    QStringList symbols;
    std::vector<Node> children;
    int minimum = 0;
    int low = 1, high = 1;

    void write(QString& result, std::mt19937_64& engine, int budget, int& steps) const {
        if (--steps < 0) invalid("正则展开超过单值计算上限");
        switch (kind) {
        case Kind::Symbols: {
            const auto& symbol = symbols.at(choose(engine, 0, int(symbols.size()) - 1));
            if (symbol.size() <= budget) result += symbol;
            else {
                QStringList eligible;
                for (const auto& item : symbols)
                    if (item.size() <= budget) eligible << item;
                result += eligible.at(choose(engine, 0, int(eligible.size()) - 1));
            }
            break;
        }
        case Kind::Sequence: {
            int needed = minimum;
            for (const auto& child : children) {
                needed -= child.minimum;
                const int before = int(result.size());
                child.write(result, engine, budget - needed, steps);
                budget -= int(result.size()) - before;
            }
            break;
        }
        case Kind::Alternative: {
            std::vector<const Node*> eligible;
            for (const auto& child : children)
                if (child.minimum <= budget) eligible.push_back(&child);
            eligible.at(choose(engine, 0, int(eligible.size()) - 1))->write(result, engine, budget, steps);
            break;
        }
        case Kind::Repeat: {
            const auto& child = children.front();
            const int cap = child.minimum ? std::min(high, budget / child.minimum) : high;
            const int count = choose(engine, low, cap);
            for (int i = 0; i < count; ++i) {
                const int before = int(result.size());
                child.write(result, engine, budget - (count - i - 1) * child.minimum, steps);
                budget -= int(result.size()) - before;
            }
            break;
        }
        }
    }
};

struct Regex::Parser {
    const QString& input;
    int at = 0;
    int effort = 0;

    QChar peek() const { return at < input.size() ? input[at] : QChar{}; }
    bool eat(QChar symbol) {
        if (peek() != symbol) return false;
        ++at;
        return true;
    }
    void require(QChar symbol) {
        if (!eat(symbol)) invalid(QString("需要字符 %1").arg(symbol));
    }
    QString scalar() {
        if (peek().isNull()) invalid("缺少字符");
        const auto first = input[at++];
        if (first.isHighSurrogate() && peek().isLowSurrogate())
            return QString(first) + input[at++];
        return QString(first);
    }
    QStringList symbols(const QString& characters) {
        QStringList out;
        for (auto character : characters) out << QString(character);
        return out;
    }
    QStringList escape() {
        const auto token = scalar();
        if (token == "d") return symbols(digits());
        if (token == "w") return symbols(letters() + digits() + "_");
        if (token == "s") return {" ", "\t"};
        if (token == "t") return {"\t"};
        if (token == "n") return {"\n"};
        if (token == "r") return {"\r"};
        if (QStringLiteral("\\.^$|?*+()[]{}-").contains(token) && token.size() == 1)
            return {token};
        invalid("支持字面量以及 \\d、\\w、\\s 等字符组。反向引用、Unicode 属性和断言暂不支持");
    }
    QStringList characterSet() {
        const bool negate = eat('^');
        QStringList values;
        while (!peek().isNull() && peek() != ']') {
            auto item = eat('\\') ? escape() : QStringList{scalar()};
            if (peek() == '-' && at + 1 < input.size() && input[at + 1] != ']') {
                ++at;
                const auto endpoint = eat('\\') ? escape() : QStringList{scalar()};
                if (item.size() != 1 || endpoint.size() != 1 || item.front().size() != 1 ||
                    endpoint.front().size() != 1 || item.front().front() > endpoint.front().front() ||
                    endpoint.front().front().unicode() - item.front().front().unicode() > 1024)
                    invalid("字符范围无效或过大");
                for (int code = item.front().front().unicode(); code <= endpoint.front().front().unicode(); ++code)
                    values << QString(QChar(code));
            } else values << item;
            if (values.size() > 4096) invalid("字符组过大");
        }
        require(']');
        values.removeDuplicates();
        if (negate) {
            QStringList complement;
            for (const auto& entry : symbols(printable()))
                if (!values.contains(entry)) complement << entry;
            values = std::move(complement);
        }
        if (values.isEmpty()) invalid("字符组没有可生成的字符");
        return values;
    }
    int count() {
        int value = 0;
        const int begin = at;
        while (peek() >= '0' && peek() <= '9') {
            value = value * 10 + (input[at++].unicode() - '0');
            if (value > maximumLength) invalid("重复次数超出 8192");
        }
        if (begin == at) invalid("缺少重复次数");
        return value;
    }
    Node atom(int depth) {
        if (++effort > 512 || depth > 32) invalid("表达式过于复杂");
        Node node;
        if (eat('(')) {
            if (eat('?')) require(':');
            node = alternative(depth + 1);
            require(')');
        } else {
            node.kind = Node::Kind::Symbols;
            if (eat('[')) node.symbols = characterSet();
            else if (eat('\\')) node.symbols = escape();
            else if (eat('.')) node.symbols = symbols(letters() + digits());
            else {
                if (QStringLiteral("^$*+?{}]").contains(peek())) invalid("不支持此处的锚点、断言或重复");
                node.symbols = {scalar()};
            }
            node.minimum = int(node.symbols.front().size());
            for (const auto& symbol : node.symbols)
                node.minimum = std::min(node.minimum, int(symbol.size()));
        }

        int low = 1, high = 1;
        if (eat('?')) { low = 0; high = 1; }
        else if (eat('*')) { low = 0; high = 8; }
        else if (eat('+')) { high = 9; }
        else if (eat('{')) {
            low = count();
            high = low;
            if (eat(',')) high = peek() == '}' ? std::min(maximumLength, low + 8) : count();
            require('}');
        }
        if (low > high) invalid("重复次数范围无效");
        if (low == 1 && high == 1) return node;
        if (node.minimum == 0 && high > 32)
            invalid("可生成空串的重复组最多重复 32 次");
        Node repeat;
        repeat.kind = Node::Kind::Repeat;
        repeat.low = low;
        repeat.high = high;
        repeat.minimum = std::min(ceiling, low * node.minimum);
        repeat.children.push_back(std::move(node));
        return repeat;
    }
    Node sequence(int depth) {
        Node out;
        while (!peek().isNull() && peek() != ')' && peek() != '|' && peek() != '$') {
            auto child = atom(depth);
            out.minimum = std::min(ceiling, out.minimum + child.minimum);
            out.children.push_back(std::move(child));
        }
        return out;
    }
    Node alternative(int depth) {
        auto first = sequence(depth);
        if (!eat('|')) return first;
        Node out;
        out.kind = Node::Kind::Alternative;
        out.minimum = first.minimum;
        out.children.push_back(std::move(first));
        do {
            auto child = sequence(depth);
            out.minimum = std::min(out.minimum, child.minimum);
            out.children.push_back(std::move(child));
        } while (eat('|'));
        return out;
    }
};

Regex::Regex(const QString& pattern) {
    if (pattern.isEmpty() || pattern.size() > 4096 || pattern.contains(QChar::Null))
        invalid("请输入 1 至 4096 个字符");
    const QRegularExpression expression(QRegularExpression::anchoredPattern(pattern));
    if (!expression.isValid()) invalid(expression.errorString());
    Parser parser{pattern};
    parser.eat('^');
    auto root = parser.alternative(0);
    parser.eat('$');
    if (parser.at != pattern.size()) invalid("锚点只可写在整个表达式的首尾");
    if (root.minimum > maximumLength) invalid("最短值超过 8192 个字符");
    root_ = std::make_shared<Node>(std::move(root));
}

int Regex::minimumCharacters() const { return root_->minimum; }
QString Regex::generate(std::mt19937_64& engine, int maximumCharacters) const {
    const int limit = std::min(maximumLength, maximumCharacters);
    if (minimumCharacters() > limit) invalid("最短匹配内容超过字段长度");
    QString result = QStringLiteral("");
    int steps = 100000;
    root_->write(result, engine, limit, steps);
    return result;
}

}
