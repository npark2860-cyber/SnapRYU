// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QLabel;

enum class SnapGoModePresentation { Hidden, Playing };

constexpr SnapGoModePresentation SelectSnapGoModePresentation(bool enabled, bool game_running,
                                                             bool compact, bool suspended,
                                                             bool fault) {
    if (!enabled || !game_running || suspended || fault) return SnapGoModePresentation::Hidden;
    return compact ? SnapGoModePresentation::Playing : SnapGoModePresentation::Hidden;
}

// Static Qt raster widgets only. This view does not own a timer or a game surface.
class SnapGoModeView final : public QWidget {
public:
    enum class Style { PlayingScreen, PairingIdle };
    explicit SnapGoModeView(Style style, QWidget* parent = nullptr);
    void SetWaitingConnected(bool connected);

private:
    QLabel* status{};
};
