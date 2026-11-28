#pragma once
#include "components.hpp"
#include "navidog/query_service.hpp"
class QLabel;
class SqlReadOnlyView;
class QTabWidget;
class NamespaceDialog final : public navidog::ui::Dialog {
  Q_OBJECT
public:
  NamespaceDialog(const navidog::ConnectionProfile &, navidog::NamespaceKind,
                  navidog::QueryService &, QWidget *parent = nullptr);
  navidog::NamespaceDraft draft() const;
  bool partialResult() const { return partial_; }
  void accept() override;
  void reject() override;

private:
  navidog::ConnectionProfile profile_;
  navidog::NamespaceKind kind_;
  navidog::QueryService &service_;
  navidog::NamespaceOptions options_;
  QString optionsId_, createId_;
  bool loading_ = false, applying_ = false, partial_ = false;
  QWidget *form_;
  navidog::ui::TextInput *name_, *path_;
  navidog::ui::Select *owner_, *charset_, *collation_;
  navidog::ui::TextInput *password_, *confirmPassword_;
  navidog::ui::Select *tablespace_, *temporaryTablespace_;
  navidog::ui::NumberInput *quota_;
  navidog::ui::CheckBox *grantSession_, *locked_;
  navidog::ui::PushButton *retry_;
  QDialogButtonBox *buttons_;
  SqlReadOnlyView *preview_;
  QLabel *message_;
  void updatePreview();
  void loadOptions();
  void updateCollations();
};

class NamespaceDropDialog final : public navidog::ui::Dialog {
public:
  NamespaceDropDialog(navidog::DatabaseKind, navidog::NamespaceKind,
                      const QString &name, const QString &sql, QWidget *parent);
  void done(int result) override;

private:
  const QString name_;
  navidog::ui::TextInput *confirmation_;
};
