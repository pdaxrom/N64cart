#pragma once

#include <QDialog>
#include <memory>

namespace Ui
{
class SettingsDialog;
}

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog() override;

    void setFixRomEnabled(bool enabled);
    bool fixRomEnabled() const;
    bool resetRequested() const;

private slots:
    void handleResetClicked();

private:
    std::unique_ptr<Ui::SettingsDialog> ui_;
    bool resetRequested_ = false;
};
