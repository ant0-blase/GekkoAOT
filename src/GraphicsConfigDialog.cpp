#include "GraphicsConfigDialog.hpp"
#include "SettingsStyle.hpp"
#include "ShellStyle.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <vector>

int editGraphicsOptions(QWidget *parent, GraphicsOptions &options, bool running,
                        std::function<void(const GraphicsOptions &)> preview)
{
    const GraphicsOptions original = options;
    GraphicsOptions draft = options;
    if (draft.backend.compare("OpenGL", Qt::CaseInsensitive) == 0)
        draft.backend = "OGL";
    QDialog dialog(parent);
    dialog.setObjectName("graphicsSettings");
    dialog.setWindowTitle("Graphics Settings");
    dialog.resize(820, 690);
    dialog.setMinimumSize(680, 570);
    dialog.setStyleSheet(settingsStyleSheet());
    QTimer update;
    update.setSingleShot(true);
    update.setInterval(85);
    QObject::connect(&update, &QTimer::timeout, &dialog, [&] { preview(draft); });
    bool refreshing = false;
    const auto changed = [&]
    {
        if (!refreshing)
            update.start();
    };
    auto *outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(22, 20, 22, 18);
    outer->setSpacing(16);
    auto *tabs = new QTabWidget(&dialog);
    outer->addWidget(tabs, 1);
    const auto page = [&](const QString &title)
    {
        auto *content = new QWidget;
        auto *form = new QFormLayout(content);
        form->setContentsMargins(24, 22, 24, 22);
        form->setHorizontalSpacing(24);
        form->setVerticalSpacing(13);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        auto *scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(content);
        tabs->addTab(scroll, title);
        return form;
    };
    auto *display = page("Display");
    auto *color = page("Color");
    auto *effects = page("Effects");
    auto *performance = page("Framerate");
    std::vector<std::function<void()>> refresh;
    const auto slider = [&](QFormLayout *form, const QString &label, float &target, float low, float high,
                            const QString &name, bool grading)
    {
        auto *row = new QWidget;
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(16);
        auto *bar = new QSlider(Qt::Horizontal);
        bar->setObjectName(name + "Slider");
        bar->setRange(int(std::lround(low * 100)), int(std::lround(high * 100)));
        bar->setPageStep(10);
        bar->setSingleStep(1);
        auto *value = new QDoubleSpinBox;
        value->setObjectName(name);
        value->setRange(low, high);
        value->setDecimals(2);
        value->setSingleStep(.01);
        value->setFixedWidth(100);
        bar->setValue(int(std::lround(target * 100)));
        value->setValue(target);
        layout->addWidget(bar, 1);
        layout->addWidget(value);
        form->addRow(label, row);
        refresh.push_back(
            [bar, value, &target]
            {
                const QSignalBlocker a(bar), b(value);
                bar->setValue(int(std::lround(target * 100)));
                value->setValue(target);
            });
        QObject::connect(bar, &QSlider::valueChanged, &dialog, [value](int v) { value->setValue(v / 100.); });
        QObject::connect(value, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog,
                         [&, bar, grading](double v)
                         {
                             target = float(v);
                             {
                                 const QSignalBlocker block(bar);
                                 bar->setValue(int(std::lround(v * 100)));
                             }
                             if (grading && !refreshing)
                                 draft.preset = "custom";
                             changed();
                         });
    };
    auto *backend = new QComboBox;
    backend->addItem("Vulkan", "Vulkan");
    backend->addItem("OpenGL", "OGL");
#ifdef Q_OS_WIN
    backend->addItem("Direct3D 12", "D3D12");
#endif
    backend->setCurrentIndex(std::max(0, backend->findData(draft.backend)));
    backend->setToolTip("Changing the graphics API requires recreating the renderer on the next launch.");
    display->addRow(running ? "Graphics API (next launch)" : "Graphics API", backend);
    QObject::connect(backend, &QComboBox::currentIndexChanged, &dialog,
                     [&]
                     {
                         draft.backend = backend->currentData().toString();
                         changed();
                     });
    float render = float(draft.image.render_percent);
    slider(display, "3D resolution", render, 25, 400, "renderPercent", false);
    QObject::connect(dialog.findChild<QDoubleSpinBox *>("renderPercent"),
                     qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog,
                     [&](double v) { draft.image.render_percent = std::uint32_t(v); });
    auto *resolutionAmount = dialog.findChild<QDoubleSpinBox *>("renderPercent");
    resolutionAmount->setDecimals(0);
    resolutionAmount->setSingleStep(1);
    resolutionAmount->setSuffix(" %");
    dialog.findChild<QSlider *>("renderPercentSlider")->setSingleStep(100);
    auto *pixels = new QLabel;
    pixels->setObjectName("renderDimensions");
    pixels->setMinimumWidth(105);
    pixels->setProperty("muted", true);
    resolutionAmount->parentWidget()->layout()->addWidget(pixels);
    const auto updateDimensions = [&]
    {
        // The launcher's 100% reference is the current monitor's physical pixels.
        double width = std::max(1, draft.outputExtent.width());
        double height = std::max(1, draft.outputExtent.height());
        const double scale = std::min(render / 100., 8192. / std::max(width, height));
        pixels->setText(QString("%1 × %2").arg(int(width * scale)).arg(int(height * scale)));
    };
    QObject::connect(resolutionAmount, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog,
                     updateDimensions);
    updateDimensions();
    auto *aspect = new QComboBox;
    aspect->addItem("Original 4:3", "4:3");
    aspect->addItem("Fit window (widescreen)", "stretch");
    aspect->setCurrentIndex(std::max(0, aspect->findData(draft.aspect)));
    display->addRow("Aspect ratio", aspect);
    QObject::connect(aspect, &QComboBox::currentIndexChanged, &dialog,
                     [&]
                     {
                         draft.aspect = aspect->currentData().toString();
                         changed();
                     });
    auto *aa = new QComboBox;
    aa->addItems({"Off", "FXAA", "4× MSAA", "4× MSAA + FXAA"});
    aa->setCurrentIndex((draft.image.msaa == 4 ? 2 : 0) + (draft.image.fxaa ? 1 : 0));
    display->addRow("Anti-aliasing", aa);
    QObject::connect(aa, &QComboBox::currentIndexChanged, &dialog,
                     [&](int v)
                     {
                         draft.image.msaa = v >= 2 ? 4 : 1;
                         draft.image.fxaa = v % 2;
                         changed();
                     });
    auto *aniso = new QComboBox;
    for (int v : {1, 2, 4, 8, 16})
        aniso->addItem(v == 1 ? "Off" : QString::number(v) + "×", v);
    aniso->setCurrentIndex(aniso->findData(int(draft.image.anisotropy)));
    display->addRow("Texture filtering", aniso);
    QObject::connect(aniso, &QComboBox::currentIndexChanged, &dialog,
                     [&]
                     {
                         draft.image.anisotropy = aniso->currentData().toUInt();
                         changed();
                     });
    const auto check =
        [&](QFormLayout *form, const QString &text, bool initial, std::function<void(bool)> set)
    {
        auto *box = new QCheckBox(text);
        box->setChecked(initial);
        form->addRow(box);
        QObject::connect(box, &QCheckBox::toggled, &dialog,
                         [&, set](bool value)
                         {
                             set(value);
                             changed();
                         });
        return box;
    };
    check(display, "Fullscreen", draft.fullscreen, [&](bool b) { draft.fullscreen = b; });
    auto *unlock = check(performance, "Unlock frame rate", draft.image.interpolation != 0,
                         [&](bool b) { draft.image.interpolation = b; });
    unlock->setObjectName("frameUnlock");
    unlock->setToolTip("Renders the game's real 3D scene again between the game's own frames, up to the "
                       "display refresh rate and the output frame limit. Game speed and animation timing "
                       "are unchanged; no image is generated.");
    auto *unlockMode = new QComboBox;
    unlockMode->setObjectName("frameUnlockMode");
    unlockMode->addItem("Smooth (interpolate, +1 game frame latency)", 0);
    unlockMode->addItem("Instant (predict, no added latency)", 1);
    unlockMode->setCurrentIndex(draft.image.unlock_predict ? 1 : 0);
    unlockMode->setEnabled(draft.image.interpolation != 0);
    unlockMode->setToolTip("Smooth shows positions between the last two game frames. Instant predicts "
                           "positions ahead of the newest frame; it can overshoot on sudden direction changes.");
    performance->addRow("Unlock mode", unlockMode);
    QObject::connect(unlock, &QCheckBox::toggled, unlockMode, &QWidget::setEnabled);
    QObject::connect(unlockMode, &QComboBox::currentIndexChanged, &dialog,
                     [&, unlockMode]
                     {
                         draft.image.unlock_predict = unlockMode->currentData().toUInt();
                         changed();
                     });
    float cap = float(draft.fps);
    slider(performance, "Output frame limit (FPS)", cap, 30, 360, "fpsLimit", false);
    QObject::connect(dialog.findChild<QDoubleSpinBox *>("fpsLimit"),
                     qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog,
                     [&](double v) { draft.fps = int(v); });
    auto *fpsAmount = dialog.findChild<QDoubleSpinBox *>("fpsLimit");
    fpsAmount->setDecimals(0);
    fpsAmount->setSingleStep(1);
    dialog.findChild<QSlider *>("fpsLimitSlider")->setSingleStep(100);
    // In-game performance HUD: drawn into the game image, each row optional.
    auto *hud = check(performance, "Show performance HUD", draft.hud, [&](bool b) { draft.hud = b; });
    hud->setObjectName("hudEnabled");
    auto *hudItems = new QWidget;
    auto *hudLayout = new QVBoxLayout(hudItems);
    hudLayout->setContentsMargins(26, 0, 0, 0);
    hudLayout->setSpacing(8);
    const std::pair<const char *, std::uint32_t> hudRows[] = {
        {"Frames per second (on screen)", GekkoAOT::GX::HudFps},
        {"Frame time", GekkoAOT::GX::HudFrameTime},
        {"Frame time graph", GekkoAOT::GX::HudGraph},
        {"Game frame rate", GekkoAOT::GX::HudGameFps},
        {"VI rate (VPS)", GekkoAOT::GX::HudVps},
        {"Execution speed", GekkoAOT::GX::HudSpeed},
        {"CPU usage", GekkoAOT::GX::HudCpu},
        {"GPU usage", GekkoAOT::GX::HudGpu},
    };
    for (const auto &[text, bit] : hudRows)
    {
        auto *box = new QCheckBox(text);
        box->setObjectName(QString("hudItem%1").arg(bit));
        box->setChecked((draft.hudItems & bit) != 0);
        hudLayout->addWidget(box);
        QObject::connect(box, &QCheckBox::toggled, &dialog,
                         [&, bit](bool on)
                         {
                             draft.hudItems = on ? (draft.hudItems | bit) : (draft.hudItems & ~bit);
                             changed();
                         });
    }
    hudItems->setEnabled(draft.hud);
    QObject::connect(hud, &QCheckBox::toggled, hudItems, &QWidget::setEnabled);
    performance->addRow(hudItems);
    auto *enable = check(effects, "Enable post-processing", draft.image.post != 0,
                         [&](bool b) { draft.image.post = b; });
    enable->setObjectName("postEnabled");
    auto *preset = new QComboBox;
    preset->addItem("Original / neutral", "neutral");
    preset->addItem("Balanced", "balanced");
    preset->addItem("Cinematic", "cinematic");
    preset->addItem("Crisp", "crisp");
    preset->addItem("Custom", "custom");
    preset->setCurrentIndex(std::max(0, preset->findData(draft.preset)));
    effects->addRow("Preset", preset);
    slider(color, "Brightness", draft.image.brightness, 0, 2, "brightness", true);
    slider(color, "Contrast", draft.image.contrast, 0, 2, "contrast", true);
    slider(color, "Saturation", draft.image.saturation, 0, 2, "saturation", true);
    slider(color, "Gamma", draft.image.gamma, .5f, 2.5f, "gamma", true);
    slider(color, "Exposure (EV)", draft.image.exposure, -3, 3, "exposure", true);
    slider(color, "Temperature", draft.image.temperature, -1, 1, "temperature", true);
    slider(color, "Tint", draft.image.tint, -1, 1, "tint", true);
    slider(color, "Vibrance", draft.image.vibrance, 0, 1, "vibrance", true);
    slider(effects, "Sharpening", draft.image.sharpen, 0, 1, "sharpen", true);
    slider(effects, "Bloom", draft.image.bloom, 0, 1, "bloom", true);
    slider(effects, "Vignette", draft.image.vignette, 0, 1, "vignette", true);
    slider(effects, "Film grain", draft.image.grain, 0, .25f, "grain", true);
    slider(effects, "Filmic tone mapping", draft.image.tone, 0, 1, "tone", true);
    slider(effects, "Local contrast", draft.image.local_contrast, 0, 1, "localContrast", true);
    QObject::connect(preset, &QComboBox::currentIndexChanged, &dialog,
                     [&]
                     {
                         const QString name = preset->currentData().toString();
                         draft.preset = name;
                         if (name == "custom")
                             return;
                         refreshing = true;
                         auto &s = draft.image;
                         s.brightness = s.contrast = s.saturation = s.gamma = 1;
                         s.exposure = s.temperature = s.vibrance = s.tint = 0;
                         s.sharpen = s.bloom = s.vignette = s.grain = s.tone = s.local_contrast =
                             s.cinematic = 0;
                         if (name == "balanced")
                         {
                             s.sharpen = .28f;
                             s.bloom = .10f;
                             s.vignette = .06f;
                             s.tone = .15f;
                             s.local_contrast = .08f;
                         }
                         if (name == "cinematic")
                         {
                             s.sharpen = .22f;
                             s.bloom = .18f;
                             s.vignette = .10f;
                             s.grain = .012f;
                             s.tone = .32f;
                             s.local_contrast = .12f;
                             s.cinematic = .20f;
                         }
                         if (name == "crisp")
                         {
                             s.sharpen = .42f;
                             s.bloom = .06f;
                             s.vignette = .02f;
                             s.tone = .08f;
                             s.local_contrast = .14f;
                         }
                         for (auto &action : refresh)
                             action();
                         refreshShellValues(&dialog);
                         refreshing = false;
                         changed();
                     });
    // Keep the preset label in sync when a grading slider is edited.
    QObject::connect(&update, &QTimer::timeout, &dialog,
                     [&]
                     {
                         const QSignalBlocker block(preset);
                         preset->setCurrentIndex(preset->findData(draft.preset));
                     });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    presentShellDialog(dialog,"Graphics",shellTVMode(parent));
    const int result = execShellDialog(dialog);
    update.stop();
    options = result == QDialog::Accepted ? draft : original;
    preview(options);
    return result;
}
