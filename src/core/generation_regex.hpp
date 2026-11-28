#pragma once

#include <QString>
#include <memory>
#include <random>

namespace navidog::generation {

class Regex {
public:
    explicit Regex(const QString& pattern);
    QString generate(std::mt19937_64&, int maximumCharacters) const;
    int minimumCharacters() const;

private:
    struct Node;
    struct Parser;
    std::shared_ptr<const Node> root_;
};

}
