#pragma once

#include "tags/tagcatalog.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;

namespace filemanager::ui {

class TagRulesDialog final : public QDialog
{
public:
    explicit TagRulesDialog(QWidget* parent = nullptr, QString storagePath = {});
    void done(int result) override;

private:
    void reload();
    void editRule(bool create);
    void removeRule();
    void save();
    void refresh();

    QString storagePath_;
    QList<tags::DestinationRule> baseline_;
    QList<tags::DestinationRule> draft_;
    QTableWidget* table_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* edit_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* save_ = nullptr;
};

} // namespace filemanager::ui
