// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <catch2/catch_test_macros.hpp>
#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "qt_common/config/qt_config.h"
#include "qt_common/config/uisettings.h"

TEST_CASE("Frame generation hotkeys migrate and survive reordered configuration saves",
          "[qt][hotkeys]") {
    int argc = 1;
    char name[] = "hotkey-config-test";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    // Keep fixtures within the build workspace (also valid in restricted Windows runs).
    QTemporaryDir temporary(QDir::currentPath() + QStringLiteral("/snapryu-hotkey-test-XXXXXX"));
    REQUIRE(temporary.isValid());
    const auto config_dir = std::filesystem::path{temporary.path().toStdWString()};
    INFO(Common::FS::PathToUTF8String(config_dir));
    REQUIRE(std::filesystem::is_directory(config_dir));
    const auto old_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
    Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, config_dir);
    SCOPE_EXIT { Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, old_dir); };

    const auto shortcut = [](const std::string& action) -> UISettings::ContextualShortcut& {
        auto& shortcuts = UISettings::values.shortcuts;
        auto found = std::ranges::find_if(shortcuts, [&](const auto& item) {
            return item.group == "Main Window" && item.name == action;
        });
        REQUIRE(found != shortcuts.end());
        return found->shortcut;
    };
    {
        QtConfig config("fresh");
        REQUIRE(shortcut("Toggle LSFG Frame Generation").keyseq == "F10");
        REQUIRE_FALSE(shortcut("Toggle LSFG Frame Generation").repeat);
        REQUIRE(shortcut("Toggle LSFG Frame Generation").context == Qt::ApplicationShortcut);
        REQUIRE(shortcut("Change Docked Mode").keyseq == "Shift+F10");
        REQUIRE(shortcut("Change Docked Mode").controller_keyseq == "Home+X");
        config.ReloadAllValues();
        REQUIRE(UISettings::values.shortcuts.size() == UISettings::default_hotkeys.size());
    }

    // Emulate a pre-FG configuration with the old dock key explicitly persisted.
    {
        QFile ini(temporary.filePath(QStringLiteral("migration.ini")));
        INFO(ini.errorString().toStdString());
        REQUIRE(ini.open(QIODevice::WriteOnly));
        REQUIRE(ini.write("[UI]\n"
               "Shortcuts\\Main%20Window\\Change%20Docked%20Mode\\KeySeq=F10\n"
               "Shortcuts\\Main%20Window\\Change%20Docked%20Mode\\KeySeq\\default=false\n"
               "Shortcuts\\Main%20Window\\Capture%20Screenshot\\KeySeq=Alt+P\n"
               "Shortcuts\\Main%20Window\\Capture%20Screenshot\\KeySeq\\default=false\n") > 0);
    }
    {
        QtConfig config("migration");
        INFO(config.GetConfigFilePath());
        REQUIRE(config.GetConfigFilePath() == Common::FS::PathToUTF8String(config_dir / "migration.ini"));
        REQUIRE(shortcut("Change Docked Mode").keyseq == "Shift+F10");
        REQUIRE(shortcut("Toggle LSFG Frame Generation").keyseq == "F10");
        REQUIRE(shortcut("Capture Screenshot").keyseq == "Alt+P");

        // The registry may reorder shortcuts. A custom key equal to a different
        // action's default must not be marked as this action's default on save.
        std::ranges::reverse(UISettings::values.shortcuts);
        REQUIRE(UISettings::values.shortcuts.front().name == "Toggle LSFG Frame Generation");
        shortcut("Toggle LSFG Frame Generation").keyseq = "Ctrl+M";
        config.SaveAllValues();
    }
    {
        QtConfig config("migration");
        REQUIRE(shortcut("Toggle LSFG Frame Generation").keyseq == "Ctrl+M");
        REQUIRE(shortcut("Change Docked Mode").keyseq == "Shift+F10");
        REQUIRE(shortcut("Capture Screenshot").keyseq == "Alt+P");
        REQUIRE(UISettings::values.shortcuts.size() == UISettings::default_hotkeys.size());
    }
}
