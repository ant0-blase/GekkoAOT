#include "GraphicsConfigDialog.hpp"
#include "../runtime/gx/graphics_presets.h"
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
                        std::function<void(const GraphicsOptions &)> preview, std::function<QString()> appliedStatus)
{
    const GraphicsOptions original = options;
    GraphicsOptions draft = options;
    if(!GekkoAOT::GX::SceneMatchesPreset(draft.image,draft.preset.toStdString()))draft.preset="custom";
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
    update.setInterval(16);
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
    auto* liveStatus=new QLabel(running?"Settings update live; new textures prepare in the background.":"Settings apply at the next launch.");
    liveStatus->setObjectName("graphicsAppliedStatus");liveStatus->setWordWrap(true);outer->addWidget(liveStatus);
    QTimer statusTimer;
    if(running && appliedStatus){statusTimer.setInterval(100);QObject::connect(&statusTimer,&QTimer::timeout,&dialog,[&]{liveStatus->setText(appliedStatus());});statusTimer.start();}

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
    auto *scene = page("Scene lighting");
    auto *materials = page("Remaster");
    auto *performance = page("Framerate");
    auto *advancedLighting = new QWidget;
    advancedLighting->setObjectName("advancedLighting");
    auto *sceneAdvanced = new QFormLayout(advancedLighting);
    sceneAdvanced->setContentsMargins(0,0,0,0);
    sceneAdvanced->setVerticalSpacing(13);
    auto *showAdvanced = new QCheckBox("Advanced lighting controls");
    showAdvanced->setObjectName("showAdvancedLighting");
    advancedLighting->setVisible(false);
    QObject::connect(showAdvanced,&QCheckBox::toggled,advancedLighting,&QWidget::setVisible);
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
        const bool sceneControl=form==sceneAdvanced || form==materials;
        QObject::connect(value, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog,
                         [&, bar, grading, sceneControl](double v)
                         {
                             target = float(v);
                             {
                                 const QSignalBlocker block(bar);
                                 bar->setValue(int(std::lround(v * 100)));
                             }
                             if ((grading || sceneControl) && !refreshing)
                                 draft.preset = "custom";
                             changed();
                         });
    };
    auto *backend = new QComboBox;
    backend->addItem("Vulkan", "Vulkan");
    backend->addItem("OpenGL", "OGL");
#ifdef Q_OS_MACOS
    backend->addItem("Metal", "Metal");
#endif
#ifdef Q_OS_WIN
    backend->addItem("Direct3D 12", "D3D12");
    backend->addItem("Direct3D 11", "D3D11");
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
    auto *enhancedEnable = check(scene, "Enhanced scene lighting", draft.image.enhanced != 0,
          [&](bool b) { draft.image.enhanced = b;if(!refreshing)draft.preset="custom"; });
    enhancedEnable->setObjectName("enhancedRenderer");
    auto *fragmentLighting = check(scene, "Per-pixel GX lighting", draft.image.fragment_lighting != 0,
          [&](bool b) { draft.image.fragment_lighting = b;if(!refreshing)draft.preset="custom"; });
    fragmentLighting->setObjectName("fragmentLighting");
    fragmentLighting->setToolTip("Evaluate the game's GX lights per pixel on eligible 3D materials with normals.");
    fragmentLighting->setEnabled(draft.image.enhanced != 0);
    QObject::connect(enhancedEnable, &QCheckBox::toggled, fragmentLighting, &QWidget::setEnabled);
    auto *textureScale = new QComboBox;
    textureScale->setObjectName("textureReconstruction");
    textureScale->addItem("Original resolution", 1);
    textureScale->addItem("2× reconstruction", 2);
    textureScale->addItem("4× reconstruction", 4);
    textureScale->setCurrentIndex(std::max(0,textureScale->findData(int(draft.image.texture_scale))));
    materials->addRow("Original texture reconstruction", textureScale);
    textureScale->setToolTip("Reconstruct original textures throughout the frame: world, weapons, skyboxes, menus and HUD. Font/palette art uses edge-rule reconstruction (non-AI) to preserve shapes and alpha; textured RGB uses the selected neural model. Animated menus and videos use real-time edge reconstruction on the GPU (non-AI), without caching obsolete frames. Packs keep priority; originals display immediately.");
    QObject::connect(textureScale, &QComboBox::currentIndexChanged, &dialog,
                     [&](int) { draft.image.texture_scale = textureScale->currentData().toUInt(); changed(); });
    auto *textureBackend = new QComboBox;
    textureBackend->setObjectName("textureReconstructionBackend");
    textureBackend->addItems({"Non-AI — internal texture examples", "ESPCN — small native neural model", "Real-ESRGAN compact (GPU)"});
    textureBackend->setCurrentIndex(int(draft.image.texture_backend));
    textureBackend->setToolTip("ESPCN is the fast 21k/25k parameter model. Real-ESRGAN compact reconstructs RGB detail with 1.2M parameters (4.9 MB weights), and takes longer to prepare. The compact model uses native GPU compute when available, with an explicit native CPU fallback. Work is asynchronous and cached; no Python, PyTorch or TensorFlow. Protected textures use the explicitly non-AI fallback.");
    materials->addRow("Texture reconstruction model", textureBackend);
    QObject::connect(textureBackend, &QComboBox::currentIndexChanged, &dialog,
                     [&](int v) { draft.image.texture_backend=std::uint32_t(v);changed(); });
    auto *textureQuality = new QComboBox;
    textureQuality->setObjectName("textureReconstructionQuality");
    textureQuality->addItems({"Balanced", "High — coherent HD mipmaps & detail"});
    textureQuality->setToolTip("Balanced preserves authored mips exactly. High keeps reconstructed RGB visible through ordinary mip levels, preserves authored alpha and unusual authored mip artwork, and improves the ESPCN inference. Fonts/palette art retain authored mip levels.");
    textureQuality->setCurrentIndex(int(draft.image.texture_quality));
    materials->addRow("Reconstruction quality", textureQuality);
    QObject::connect(textureQuality, &QComboBox::currentIndexChanged, &dialog,
                     [&](int v) { draft.image.texture_quality = std::uint32_t(v); changed(); });
    auto *meshQuality = new QComboBox;
    meshQuality->setObjectName("geometryRefinement");
    meshQuality->addItems({"Off", "Balanced — up to 4× triangles", "High — up to 16× triangles"});
    meshQuality->setCurrentIndex(int(draft.image.geometry_refinement));
    materials->addRow("Curved mesh refinement", meshQuality);
    meshQuality->setToolTip("Asynchronously refine validated meshes with reliable normals. Preserves UV seams and hard edges. Original geometry stays available while meshes are prepared. Guest collision is unchanged.");
    QObject::connect(meshQuality, &QComboBox::currentIndexChanged, &dialog,
                     [&](int v) { draft.image.geometry_refinement = std::uint32_t(v);if(!refreshing)draft.preset="custom"; changed(); });
    auto *materialEnable = check(materials, "Enhance original surface materials", draft.image.material_enhancement != 0,
          [&](bool b) { draft.image.material_enhancement = b;if(!refreshing)draft.preset="custom"; });
    materialEnable->setObjectName("materialEnhancement");
    materialEnable->setToolTip("Estimate surface normals and roughness for eligible opaque 3D textures. Uses original disc textures; no texture pack is needed.");
    auto *materialCompare = check(materials, "Compare materials and geometry (original on left)", draft.image.material_compare != 0,
          [&](bool b) { draft.image.material_compare = b; });
    materialCompare->setObjectName("materialCompare");
    materialCompare->setToolTip("Compare the same frame and lighting. Only the right half receives mesh refinement, generated normals and POM; other enhanced effects are unchanged.");
    auto *pom = new QComboBox;
    pom->setObjectName("pomQuality");
    pom->addItems({"Off", "Balanced (up to 12 steps)", "High (up to 24 steps)"});
    pom->setCurrentIndex(int(draft.image.pom_quality));
    materials->addRow("Parallax Occlusion Mapping (POM)", pom);
    pom->setToolTip("Apparent depth on strongly repeated surface structures. Does not modify geometry or silhouettes.");
    QObject::connect(pom, &QComboBox::currentIndexChanged, &dialog,
                     [&](int v) { draft.image.pom_quality = std::uint32_t(v);if(!refreshing)draft.preset="custom"; changed(); });
    auto* displacement = check(materials,"Geometric displacement — recognised surface heights", draft.image.displacement != 0,
          [&](bool b) {draft.image.displacement=b;if(b)materialEnable->setChecked(true);if(!refreshing)draft.preset="custom";});
    displacement->setObjectName("geometricDisplacement");
    displacement->setToolTip("Actually moves subdivided vertices on recognised opaque surfaces with structured height. Up to 64× triangles, generated asynchronously. Pins patch edges to avoid cracks; original collision and outer silhouette boundaries stay unchanged. Unknown heights are not invented.");
    slider(materials, "Surface normal strength", draft.image.material_normal_strength, 0, 2, "materialNormalStrength", false);
    slider(materials, "Relief depth (POM / displacement)", draft.image.material_relief, 0, .05f, "materialRelief", false);
    slider(materials, "Surface micro-detail", draft.image.surface_detail, 0, 1, "surfaceDetail", false);
    slider(materials, "Surface highlights", draft.image.surface_specular, 0, 1, "surfaceSpecular", false);
    const auto refreshMaterials = [&,meshQuality,materialEnable,materialCompare,pom,textureScale,textureBackend,textureQuality,displacement] {
      const QSignalBlocker b0(meshQuality),b1(materialEnable),b2(materialCompare),b3(pom),b4(textureScale),b5(textureQuality),b6(textureBackend),b7(displacement);
      displacement->setChecked(draft.image.displacement!=0);
      displacement->setEnabled(draft.image.enhanced!=0);
      meshQuality->setCurrentIndex(int(draft.image.geometry_refinement));
      materialEnable->setChecked(draft.image.material_enhancement!=0);
      materialCompare->setChecked(draft.image.material_compare!=0);
      pom->setCurrentIndex(int(draft.image.pom_quality));
      textureScale->setCurrentIndex(textureScale->findData(int(draft.image.texture_scale)));
      textureQuality->setCurrentIndex(int(draft.image.texture_quality));
      textureBackend->setCurrentIndex(int(draft.image.texture_backend));
      meshQuality->setEnabled(draft.image.enhanced != 0);
      materialEnable->setEnabled(draft.image.enhanced != 0);
      materialCompare->setEnabled(draft.image.enhanced && (draft.image.material_enhancement || draft.image.geometry_refinement));
      pom->setEnabled(draft.image.enhanced && draft.image.material_enhancement);
      textureScale->setEnabled(true);
      textureBackend->setEnabled(draft.image.texture_scale > 1);
      textureQuality->setEnabled(draft.image.texture_scale > 1);
    };
    QObject::connect(enhancedEnable, &QCheckBox::toggled, &dialog, refreshMaterials);
    QObject::connect(meshQuality, &QComboBox::currentIndexChanged, &dialog, refreshMaterials);
    QObject::connect(materialEnable, &QCheckBox::toggled, &dialog, refreshMaterials);
    QObject::connect(textureScale, &QComboBox::currentIndexChanged, &dialog, refreshMaterials);
    QObject::connect(textureBackend, &QComboBox::currentIndexChanged, &dialog, refreshMaterials);
    refresh.push_back(refreshMaterials);refreshMaterials();
    auto *ao = new QComboBox;
    ao->setObjectName("aoQuality");
    ao->addItems({"Off", "Low", "Medium", "High"});
    ao->setCurrentIndex(int(draft.image.ao_quality));
    scene->addRow("Ambient occlusion", ao);
    QObject::connect(ao, &QComboBox::currentIndexChanged, &dialog,
                     [&](int v) { draft.image.ao_quality = std::uint32_t(v);if(!refreshing)draft.preset="custom"; changed(); });
    slider(sceneAdvanced, "Occlusion strength", draft.image.ao_strength, 0, 2, "aoStrength", false);
    slider(sceneAdvanced, "Relative occlusion radius", draft.image.ao_radius, .01f, 1000, "aoRadius", false);
    check(scene, "Contact shadows", draft.image.contact_shadows != 0,
          [&](bool b) { draft.image.contact_shadows = b;if(!refreshing)draft.preset="custom"; })->setObjectName("contactShadows");
    slider(sceneAdvanced, "Shadow strength", draft.image.shadow_strength, 0, 1, "shadowStrength", false);
    slider(sceneAdvanced, "Relative shadow distance", draft.image.shadow_distance, .01f, 10000, "shadowDistance", false);
    slider(sceneAdvanced, "Relative shadow thickness", draft.image.shadow_thickness, .001f, 100, "shadowThickness", false);
    slider(sceneAdvanced, "Shadow softness", draft.image.shadow_softness, 0, 1, "shadowSoftness", false);
    auto *lightInfo = new QLabel("Direction towards the light, relative to the camera. Positive Z points towards the viewer.");
    lightInfo->setWordWrap(true);sceneAdvanced->addRow(lightInfo);
    slider(sceneAdvanced, "Light X", draft.image.light_x, -1, 1, "lightX", false);
    slider(sceneAdvanced, "Light Y", draft.image.light_y, -1, 1, "lightY", false);
    slider(sceneAdvanced, "Light Z", draft.image.light_z, -1, 1, "lightZ", false);
    scene->addRow(showAdvanced);scene->addRow(advancedLighting);
    // Environment lighting: a sun fixed in the game world plus sky light,
    // applied to the enhanced renderer's scene receivers.
    auto *environmentMode = new QComboBox;
    environmentMode->setObjectName("environmentLighting");
    environmentMode->addItems({"Off", "Auto (game sun only)", "Custom sun"});
    environmentMode->setCurrentIndex(int(std::min<std::uint32_t>(draft.image.environment, 2)));
    environmentMode->setToolTip("Auto uses the game's own directional light as the sun and never adds one in scenes without it. "
                                "Custom places a sun in the recovered world frame.");
    environmentMode->setEnabled(draft.image.enhanced != 0);
    QObject::connect(enhancedEnable, &QCheckBox::toggled, environmentMode, &QWidget::setEnabled);
    scene->addRow("Environment lighting", environmentMode);
    QObject::connect(environmentMode, &QComboBox::currentIndexChanged, &dialog,
                     [&](int v) { draft.image.environment = std::uint32_t(v);if(!refreshing)draft.preset="custom"; changed(); });
    auto *sunShadows = new QComboBox;
    sunShadows->setObjectName("sunShadows");
    sunShadows->addItems({"Off", "Cascaded 1024", "Cascaded 2048"});
    sunShadows->setCurrentIndex(int(std::min<std::uint32_t>(draft.image.sun_shadows, 2)));
    sunShadows->setToolTip("Shadow maps of the game's real geometry cast by the environment sun.");
    scene->addRow("Sun shadows", sunShadows);
    QObject::connect(sunShadows, &QComboBox::currentIndexChanged, &dialog,
                     [&](int v) { draft.image.sun_shadows = std::uint32_t(v);if(!refreshing)draft.preset="custom"; changed(); });
    auto *giEnable = check(scene, "Global illumination (one bounce)", draft.image.gi != 0,
          [&](bool b) { draft.image.gi = b;if(!refreshing)draft.preset="custom"; });
    giEnable->setObjectName("gi");
    giEnable->setToolTip("Light bounced from nearby visible surfaces tints the scene.");
    slider(scene, "Bounce light strength", draft.image.gi_strength, 0, 2, "giStrength", false);
    auto *ssrEnable = check(scene, "Screen-space reflections", draft.image.ssr != 0,
          [&](bool b) { draft.image.ssr = b;if(!refreshing)draft.preset="custom"; });
    ssrEnable->setObjectName("ssr");
    ssrEnable->setToolTip("Fresnel reflections of the visible scene, strongest at grazing angles.");
    slider(scene, "Reflection strength", draft.image.ssr_strength, 0, 4, "ssrStrength", false);
    auto *particleLights = new QComboBox;
    particleLights->setObjectName("particleLights");
    particleLights->addItems({"Off", "Balanced (4 visible sources)", "High (8 visible sources)"});
    particleLights->setCurrentIndex(int(std::min<std::uint32_t>(draft.image.particle_lights,2)));
    particleLights->setToolTip("Local light from 3D flames, shots and explosions, using decoded texture and GX material colours when available. Visible-depth contact shadows; emitters not submitted by the game cannot be recovered.");
    scene->addRow("Particle lighting",particleLights);
    QObject::connect(particleLights,&QComboBox::currentIndexChanged,&dialog,[&](int v){draft.image.particle_lights=std::uint32_t(v);if(!refreshing)draft.preset="custom";changed();});
    slider(scene,"Particle light strength",draft.image.particle_light_strength,0,4,"particleLightStrength",false);
    slider(scene,"Particle light reach",draft.image.particle_light_radius,.5,6,"particleLightRadius",false);
    auto *dofQuality = new QComboBox;
    dofQuality->setObjectName("dofQuality");
    dofQuality->addItems({"Off", "Balanced (12 samples)", "High (24 samples)"});
    dofQuality->setCurrentIndex(int(std::min<std::uint32_t>(draft.image.dof_quality,2)));
    dofQuality->setToolTip("Depth-aware far-field blur. A fixed focus plane avoids focus hunting. Foreground weapons, HUD and text stay sharp. Requires Enhanced Renderer.");
    scene->addRow("Depth of field",dofQuality);
    QObject::connect(dofQuality,&QComboBox::currentIndexChanged,&dialog,[&](int v){draft.image.dof_quality=std::uint32_t(v);if(!refreshing)draft.preset="custom";changed();});
    slider(scene,"Depth of field strength",draft.image.dof_strength,0,1,"dofStrength",false);
    slider(scene,"Focus distance (lens units)",draft.image.dof_focus,1,500,"dofFocus",false);
    auto *sunPreset = new QComboBox;
    sunPreset->setObjectName("sunPreset");
    sunPreset->addItems({"Neutral daylight", "Warm daylight", "Overcast", "Golden hour"});
    sunPreset->setCurrentIndex(int(std::min<std::uint32_t>(draft.image.sun_preset, 3)));
    scene->addRow("Sun and sky", sunPreset);
    slider(scene, "Environment strength", draft.image.environment_strength, 0, 1, "environmentStrength", false);
    slider(scene, "Sun intensity", draft.image.sun_intensity, 0, 2, "sunIntensity", false);
    slider(scene, "Sky intensity", draft.image.sky_intensity, 0, 2, "skyIntensity", false);
    slider(scene, "Sun temperature", draft.image.sun_temperature, 1500, 12000, "sunTemperature", false);
    slider(scene, "Sun azimuth (custom)", draft.image.sun_azimuth, -360, 360, "sunAzimuth", false);
    slider(scene, "Sun elevation (custom)", draft.image.sun_elevation, 1, 90, "sunElevation", false);
    dialog.findChild<QDoubleSpinBox*>("sunTemperature")->setSuffix(" K");
    dialog.findChild<QDoubleSpinBox*>("sunTemperature")->setDecimals(0);
    dialog.findChild<QDoubleSpinBox*>("sunAzimuth")->setSuffix(QString::fromUtf8("°"));
    dialog.findChild<QDoubleSpinBox*>("sunElevation")->setSuffix(QString::fromUtf8("°"));
    QObject::connect(sunPreset, &QComboBox::currentIndexChanged, &dialog,
                     [&](int v)
                     {
                         GekkoAOT::GX::ParseGraphicsSetting(draft.image, "sun_preset", std::to_string(v));
                         refreshing = true;
                         for (auto &action : refresh)
                             action();
                         refreshing = false;
                         changed();
                     });
    dialog.findChild<QDoubleSpinBox*>("shadowThickness")->setDecimals(3);
    auto *shaderInfo = new QLabel("Known shaders compile before the game starts. New shaders compile in the background; drawing waits when a required shader is still compiling.");
    shaderInfo->setWordWrap(true);performance->addRow(shaderInfo);
    float fov = draft.image.fov_y > 0 ? draft.image.fov_y : 75.f;
    auto *fovOverride = check(display, "Override game field of view", draft.image.fov_y > 0,
          [&](bool b) { draft.image.fov_y = b ? fov : 0.f; });
    fovOverride->setObjectName("fovOverride");
    fovOverride->setToolTip("Off preserves the game's projection and zoom. On overrides the vertical field of view of 3D scene cameras.");
    slider(display, "Vertical field of view", fov, 20, 150, "fovY", false);
    auto *fovAmount = dialog.findChild<QDoubleSpinBox*>("fovY");
    fovAmount->setSuffix(QString::fromUtf8("°"));
    fovAmount->setDecimals(1);fovAmount->setSingleStep(1);
    fovAmount->parentWidget()->setEnabled(draft.image.fov_y > 0);
    QObject::connect(fovOverride,&QCheckBox::toggled,fovAmount->parentWidget(),&QWidget::setEnabled);
    QObject::connect(fovAmount,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,
                     [&, fovOverride](double v) { if(fovOverride->isChecked())draft.image.fov_y=float(v);changed(); });
    check(display, "Fullscreen", draft.fullscreen, [&](bool b) { draft.fullscreen = b; });
    auto *unlock = check(performance, "Unlock frame rate", draft.image.interpolation != 0,
                         [&](bool b) { draft.image.interpolation = b; });
    unlock->setObjectName("frameUnlock");
    unlock->setToolTip("Renders the game's real 3D scene again between the game's own frames, up to the "
                       "output frame limit (and the display refresh rate with VSync). Game speed and "
                       "animation timing are unchanged; no image is generated.");
    auto *unlockMode = new QComboBox;
    unlockMode->setObjectName("frameUnlockMode");
    unlockMode->addItem("Smooth (interpolate, +1 game frame latency)", 0);
    unlockMode->addItem("Instant (predict, no added latency)", 1);
    unlockMode->setCurrentIndex(draft.image.unlock_predict ? 1 : 0);
    unlockMode->setEnabled(draft.image.interpolation != 0);
    unlockMode->setToolTip("Smooth shows positions between the last two game frames, including meshes the "
                           "game animates on the CPU. Instant predicts camera and object motion ahead of the "
                           "newest frame; it can overshoot on sudden direction changes and keeps CPU-animated "
                           "meshes at the game's rate.");
    performance->addRow("Unlock mode", unlockMode);
    QObject::connect(unlock, &QCheckBox::toggled, unlockMode, &QWidget::setEnabled);
    QObject::connect(unlockMode, &QComboBox::currentIndexChanged, &dialog,
                     [&, unlockMode]
                     {
                         draft.image.unlock_predict = unlockMode->currentData().toUInt();
                         changed();
                     });
    auto *vsync = check(performance, "VSync", draft.image.vsync != 0,
                        [&](bool b) { draft.image.vsync = b; changed(); });
    vsync->setObjectName("vsync");
    vsync->setToolTip("Waits for the display's refresh before showing a frame: no tearing, but the frame "
                      "rate is capped at the refresh rate. Off: frames are shown as soon as they are ready, "
                      "up to the output frame limit.");
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
    preset->setObjectName("graphicsPreset");
    preset->addItem("Original / neutral", "neutral");
    preset->addItem("Balanced", "balanced");
    preset->addItem("Cinematic", "cinematic");
    preset->addItem("Crisp", "crisp");
    preset->addItem("Remastered — Natural", "remastered");
    preset->addItem("Remastered — Vivid", "remastered_vivid");
    preset->addItem("Remastered — Cinematic", "remastered_cinematic");
    preset->addItem("Remastered — Performance", "remastered_performance");
    preset->addItem("Remake — AI textures", "remastered_remake");
    preset->addItem("Custom", "custom");
    preset->setCurrentIndex(std::max(0, preset->findData(draft.preset)));
    auto *presetRow = new QWidget;
    auto *presetLayout = new QHBoxLayout(presetRow);
    presetLayout->setContentsMargins(0,0,0,0);
    presetLayout->addWidget(new QLabel("Graphics preset"));
    presetLayout->addWidget(preset);
    presetLayout->addStretch(1); // left-aligned: TV navigation goes down to the first tab
    outer->insertWidget(0,presetRow);
    preset->setToolTip("Sets scene lighting, color and post-processing together. Display, FOV and frame rate remain as configured.");
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
                         GekkoAOT::GX::ApplyGraphicsPreset(draft.image,name.toStdString());
                         {const QSignalBlocker a(enable);enable->setChecked(draft.image.post!=0);}
                         auto *enhanced=dialog.findChild<QCheckBox*>("enhancedRenderer");
                         auto *contact=dialog.findChild<QCheckBox*>("contactShadows");
                         {const QSignalBlocker a(enhanced),b(contact),c(ao),d(fragmentLighting);
                          enhanced->setChecked(draft.image.enhanced!=0);
                          fragmentLighting->setChecked(draft.image.fragment_lighting!=0);
                          fragmentLighting->setEnabled(draft.image.enhanced!=0);
                          dialog.findChild<QComboBox*>("environmentLighting")->setEnabled(draft.image.enhanced!=0);
                          {auto* e=dialog.findChild<QComboBox*>("environmentLighting");auto* sh=dialog.findChild<QComboBox*>("sunShadows");
                           auto* g=dialog.findChild<QCheckBox*>("gi");auto* r=dialog.findChild<QCheckBox*>("ssr");
                           auto* sp=dialog.findChild<QComboBox*>("sunPreset");
                           const QSignalBlocker b1(e),b2(sh),b3(g),b4(r),b5(sp);
                           e->setCurrentIndex(int(draft.image.environment));sh->setCurrentIndex(int(draft.image.sun_shadows));
                           sp->setCurrentIndex(int(draft.image.sun_preset));
                           g->setChecked(draft.image.gi!=0);r->setChecked(draft.image.ssr!=0);}
                          contact->setChecked(draft.image.contact_shadows!=0);
                          ao->setCurrentIndex(int(draft.image.ao_quality));}
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
