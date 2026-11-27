#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

inline QString appImageExecutable() {
    const auto app_dir = qEnvironmentVariable("APPDIR");
    const QFileInfo image(qEnvironmentVariable("APPIMAGE"));
    if (app_dir.isEmpty() || !image.isAbsolute() || !image.isFile() || !image.isExecutable()) return {};
    const auto bundled_gui = QDir(app_dir).filePath(QStringLiteral("usr/local/libexec/p2premote/gui"));
    if (QCoreApplication::applicationDirPath() != QDir(bundled_gui).absolutePath()) return {};
    return image.absoluteFilePath();
}
