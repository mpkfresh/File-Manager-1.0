#pragma once

#include "files/batchrename.h"

#include <QDialog>
#include <QHash>
#include <QStringList>
#include <memory>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QKeySequence;

namespace filemanager::ui {

class BatchRenameDialog final : public QDialog
{
public:
    explicit BatchRenameDialog(const QStringList& paths, QWidget* parent = nullptr);
    ~BatchRenameDialog() override;
    const filemanager::files::BatchRenamePlan& renamePlan() const { return plan_; }
    bool previewPending() const { return pending_; }
    void done(int result) override;

private:
    void rebuildPlan();
    void startPreparation();
    void renderNextRows();
    void moveCurrentRow(int offset);
    void updateOrderButtons();

    class Preparation;
    std::unique_ptr<Preparation> preparation_;
    bool pending_ = true;
    QStringList paths_;
    QHash<QString, filemanager::files::BatchRenameSource> originalSources_;
    filemanager::files::BatchRenamePlan plan_;
    QComboBox* mode_ = nullptr;
    QLineEdit* label_ = nullptr;
    QSpinBox* start_ = nullptr;
    QSpinBox* step_ = nullptr;
    QSpinBox* digits_ = nullptr;
    QPlainTextEdit* names_ = nullptr;
    QCheckBox* keepExtensions_ = nullptr;
    QLineEdit* find_ = nullptr;
    QLineEdit* replace_ = nullptr;
    QLineEdit* prefix_ = nullptr;
    QLineEdit* suffix_ = nullptr;
    QCheckBox* regex_ = nullptr;
    QCheckBox* caseSensitive_ = nullptr;
    QComboBox* caseConversion_ = nullptr;
    QCheckBox* sequence_ = nullptr;
    QSpinBox* ruleStart_ = nullptr;
    QSpinBox* ruleStep_ = nullptr;
    QSpinBox* ruleDigits_ = nullptr;
    QTableWidget* preview_ = nullptr;
    QLabel* validation_ = nullptr;
    QPushButton* up_ = nullptr;
    QPushButton* down_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

filemanager::files::BatchRenameResult runBatchRenameDialog(
    QWidget* parent, const QStringList& paths,
    filemanager::files::BatchRenamePlan* appliedPlan = nullptr);
void showBatchRenameHelp(QWidget* parent, const QKeySequence& shortcut);

} // namespace filemanager::ui
