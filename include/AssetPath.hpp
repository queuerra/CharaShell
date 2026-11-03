#pragma once
#include <QCoreApplication>
#include <QDir>

// Resolve assets against the executable directory, never the process working directory.
inline QString resolveAssetPath(const QString& value) {
    const auto path = QDir::fromNativeSeparators(value.trimmed());
    if (path.isEmpty()) return {};
    return QDir::cleanPath(QDir::isAbsolutePath(path) ? path :
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(path));
}
inline QString portableAssetPath(const QString& value) {
    const auto absolute = resolveAssetPath(value);
    const auto base = QCoreApplication::applicationDirPath();
    return absolute.startsWith(base + '/', Qt::CaseInsensitive) ? QDir(base).relativeFilePath(absolute) : value;
}
