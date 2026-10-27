// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <QString>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class SnapGoQrWidget;
namespace SnapGoUi {
// Reads only the independent Go UI settings; an unconfigured install stays OFF.
void Initialize();
QString SettingsPath();
void SetGamePaused(bool paused);
}

class SnapGoDialog final : public QDialog {
public:
    explicit SnapGoDialog(QWidget* parent);
    ~SnapGoDialog() override;
private:
    void RefreshAddresses();
    void RefreshStatus();
    bool ApplyEnabled(bool enabled);
    void ShowFailure(const QString& fallback);
    void SavePreferences();
    void ClearQr();
    QCheckBox* enabled{};
    QCheckBox* compact{};
    QComboBox* address{};
    QPushButton* pair{};
    QLabel* status{};
    QLabel* qr_hint{};
    SnapGoQrWidget* qr{};
    QTimer status_timer;
    QTimer qr_timer;
};
