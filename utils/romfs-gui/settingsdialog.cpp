#include "settingsdialog.h"

#include "ui_settingsdialog.h"

#include <QDialogButtonBox>
#include <QPushButton>

namespace
{
constexpr bool kDefaultFixRomEnabled = false;
}

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
    , ui_(std::make_unique<Ui::SettingsDialog>())
{
    ui_->setupUi(this);
    connect(ui_->buttonBox, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(ui_->buttonBox, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    connect(ui_->resetButton, &QPushButton::clicked, this, &SettingsDialog::handleResetClicked);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::setFixRomEnabled(bool enabled)
{
    ui_->fixRomCheckBox->setChecked(enabled);
}

bool SettingsDialog::fixRomEnabled() const
{
    return ui_->fixRomCheckBox->isChecked();
}

bool SettingsDialog::resetRequested() const
{
    return resetRequested_;
}

void SettingsDialog::handleResetClicked()
{
    resetRequested_ = true;
    ui_->fixRomCheckBox->setChecked(kDefaultFixRomEnabled);
}
