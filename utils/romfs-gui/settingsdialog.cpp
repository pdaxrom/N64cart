#include "settingsdialog.h"

#include "ui_settingsdialog.h"

#include <algorithm>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QVector>
#include <QString>

namespace
{
constexpr bool kDefaultFixRomEnabled = false;

struct LanguageOption
{
    QString label;
    QString code;
};
}

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
    , ui_(std::make_unique<Ui::SettingsDialog>())
{
    ui_->setupUi(this);
    ui_->languageComboBox->addItem(tr("System default"), QStringLiteral("system"));

    QVector<LanguageOption> languages = {
        {tr("English"), QStringLiteral("en")},
        {tr("Russian"), QStringLiteral("ru")},
        {tr("Armenian"), QStringLiteral("hy")},
        {tr("Persian (Farsi)"), QStringLiteral("fa")},
        {tr("Portuguese (Brazil)"), QStringLiteral("pt_BR")},
        {tr("German"), QStringLiteral("de")},
        {tr("Spanish"), QStringLiteral("es")},
    };
    std::sort(languages.begin(), languages.end(),
              [](const LanguageOption &lhs, const LanguageOption &rhs) {
                  return QString::localeAwareCompare(lhs.label, rhs.label) < 0;
              });
    for (const LanguageOption &language : languages) {
        ui_->languageComboBox->addItem(language.label, language.code);
    }

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
