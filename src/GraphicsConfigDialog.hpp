#pragma once
#include "../runtime/gx/graphics_settings.h"
#include <QDialog>
#include <QSize>
#include <QString>
#include <functional>

struct GraphicsOptions
{
    GekkoAOT::GX::GraphicsSettings image;
    QString backend = "Vulkan", aspect = "4:3", preset = "balanced";
    int fps = 120;
    bool fullscreen = false, hud = true;
    std::uint32_t hudItems = GekkoAOT::GX::HudDefaultItems;
    QSize outputExtent = QSize(1920, 1080);
};

// Changes are previewed in the renderer; Cancel restores the opening snapshot.
int editGraphicsOptions(QWidget *parent, GraphicsOptions &options, bool running,
                        std::function<void(const GraphicsOptions &)> preview);
