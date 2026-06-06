#include "settingsdialog.h"

#include "ui_settingsdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QString>

namespace
{
constexpr bool kDefaultFixRomEnabled = false;
}

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
    , ui_(std::make_unique<Ui::SettingsDialog>())
{
    ui_->setupUi(this);
    ui_->languageComboBox->addItem(tr("System default"), QStringLiteral("system"));
    ui_->languageComboBox->addItem(tr("English"), QStringLiteral("en"));
    ui_->languageComboBox->addItem(tr("Russian"), QStringLiteral("ru"));
    ui_->languageComboBox->addItem(tr("Armenian"), QStringLiteral("hy"));
    ui_->languageComboBox->addItem(tr("Persian (Farsi)"), QStringLiteral("fa"));

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

void SettingsDialog::setLanguageCode(const QString &languageCode)
{
    const QString normalizedCode = languageCode.isEmpty()
                                   ? QStringLiteral("system")
                                   : languageCode;
    const int index = ui_->languageComboBox->findData(normalizedCode);
    ui_->languageComboBox->setCurrentIndex(index >= 0 ? index : 0);
}

QString SettingsDialog::languageCode() const
{
    const QVariant data = ui_->languageComboBox->currentData();
    if (!data.isValid() || data.toString().isEmpty()) {
        return QStringLiteral("system");
    }
    return data.toString();
}

bool SettingsDialog::resetRequested() const
{
    return resetRequested_;
}

void SettingsDialog::handleResetClicked()
{
    resetRequested_ = true;
    ui_->fixRomCheckBox->setChecked(kDefaultFixRomEnabled);
    setLanguageCode(QStringLiteral("system"));
}
