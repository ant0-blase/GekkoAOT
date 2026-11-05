#pragma once
#include <QString>

// Keep compiler output in Details. The launcher shows stable, readable phases.
inline QString buildProgressLabel(const QString &detail) {
  const auto text = detail.toLower();
  if (text.contains("failed") || text.contains("cancel") ||
      text.contains("ready") || text.contains("closing"))
    return detail;
  if (text.contains("native gx") || text.contains("aurora") ||
      text.contains("graphics build"))
    return "Preparing graphics";
  if (text.contains("aot compiler") || text.contains("toolchain") ||
      text.contains("build tools"))
    return "Preparing compiler";
  if (text.contains("globbed") || text.contains("configur") ||
      text.contains("cmake"))
    return "Preparing build";
  if (text.contains("link"))
    return "Finishing build";
  if (text.contains("aot blocks") || text.contains("compil") ||
      text.contains("module"))
    return "Compiling game";
  if (text.contains("extract") || text.contains("reading") ||
      text.contains("inspect"))
    return "Reading game";
  if (text.contains("waiting") || text.contains("queue"))
    return "Waiting in queue";
  if (text.contains("runtime") || text.contains("starting") ||
      text.contains("launch"))
    return "Starting game";
  return detail;
}
