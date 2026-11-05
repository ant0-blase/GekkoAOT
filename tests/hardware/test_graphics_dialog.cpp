// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/GraphicsConfigDialog.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include "../../src/ShellStyle.hpp"
#include <QLabel>
#include <QTabWidget>
#include <QTimer>
#include <cstdlib>
#include <iostream>
#define CHECK(x)                                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        if (!(x))                                                                                            \
        {                                                                                                    \
            std::cerr << __LINE__ << ": " #x "\n";                                                           \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (false)
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    GraphicsOptions options;
    options.outputExtent = QSize(1920, 1200);
    GraphicsOptions last;
    unsigned previews = 0;
    QTimer timer;
    unsigned step = 0;
    QObject::connect(&timer, &QTimer::timeout,
                     [&]
                     {
                         auto *dialog = qobject_cast<QDialog *>(app.activeModalWidget());
                         if (!dialog)
                             return;
                         if (step++ == 0)
                         {
                             auto *tabs = dialog->findChild<QTabWidget *>();
                             CHECK(tabs && tabs->count() == 6);
                             CHECK(dialog->findChild<QWidget*>("advancedLighting")->isHidden());
                             auto *preset=dialog->findChild<QComboBox*>("graphicsPreset");
                             CHECK(preset);preset->setCurrentIndex(preset->findData("remastered"));
                             CHECK(dialog->findChild<QCheckBox*>("materialEnhancement")->isChecked());
                             CHECK(dialog->findChild<QComboBox*>("geometryRefinement")->currentIndex()==0);
                             CHECK(dialog->findChild<QComboBox*>("pomQuality")->currentIndex()==1);
                             preset->setCurrentIndex(preset->findData("balanced"));
                             CHECK(!dialog->findChild<QCheckBox*>("materialEnhancement")->isChecked());
                             CHECK(dialog->findChild<QComboBox*>("geometryRefinement")->currentIndex()==0);
                             CHECK(dialog->findChild<QCheckBox*>("enhancedRenderer")->isChecked());
                             CHECK(dialog->findChild<QCheckBox*>("contactShadows")->isChecked());
                             auto *lighting=dialog->findChild<QCheckBox*>("fragmentLighting");
                             CHECK(lighting && lighting->isEnabled() && lighting->isChecked());
                             lighting->setChecked(false);
                             CHECK(dialog->findChild<QComboBox*>("aoQuality")->currentIndex()==2);
                             dialog->findChild<QCheckBox*>("showAdvancedLighting")->setChecked(true);
                             dialog->findChild<QDoubleSpinBox *>("brightness")->setValue(1.75);
                             dialog->findChild<QDoubleSpinBox *>("saturation")->setValue(0);
                             dialog->findChild<QDoubleSpinBox *>("renderPercent")->setValue(200);
                             auto* particleReach=dialog->findChild<QDoubleSpinBox*>("particleLightRadius");
                             CHECK(particleReach);particleReach->setValue(3.5);
                             CHECK(dialog->findChild<QLabel *>("renderDimensions")->text() == "3840 × 2400");
                             dialog->findChild<QCheckBox*>("fovOverride")->setChecked(true);
                             dialog->findChild<QDoubleSpinBox*>("fovY")->setValue(110);
                             dialog->findChild<QCheckBox*>("enhancedRenderer")->setChecked(true);
                             dialog->findChild<QComboBox*>("aoQuality")->setCurrentIndex(2);
                             dialog->findChild<QDoubleSpinBox*>("shadowDistance")->setValue(12);
                             CHECK(dialog->findChild<QComboBox*>("environmentLighting")->isEnabled());
                             dialog->findChild<QComboBox*>("environmentLighting")->setCurrentIndex(2);
                             dialog->findChild<QComboBox*>("sunPreset")->setCurrentIndex(3);
                             CHECK(dialog->findChild<QDoubleSpinBox*>("sunTemperature")->value() == 3300);
                             dialog->findChild<QDoubleSpinBox*>("sunAzimuth")->setValue(120);
                             dialog->findChild<QComboBox*>("sunShadows")->setCurrentIndex(2);
                             auto* geometry=dialog->findChild<QComboBox*>("geometryRefinement");
                             CHECK(geometry && geometry->isEnabled());geometry->setCurrentIndex(2);
                             auto* material=dialog->findChild<QCheckBox*>("materialEnhancement");
                             auto* pom=dialog->findChild<QComboBox*>("pomQuality");
                             auto* reconstruction=dialog->findChild<QComboBox*>("textureReconstruction");
                             CHECK(material && material->isEnabled() && !material->isChecked());
                             CHECK(pom && !pom->isEnabled());
                             CHECK(reconstruction && reconstruction->currentData().toUInt()==1);
                             material->setChecked(true);CHECK(pom->isEnabled());pom->setCurrentIndex(2);
                             auto* compare=dialog->findChild<QCheckBox*>("materialCompare");
                             CHECK(compare && compare->isEnabled());compare->setChecked(true);
                             reconstruction->setCurrentIndex(1);
                             auto* model=dialog->findChild<QComboBox*>("textureReconstructionBackend");
                             CHECK(model && model->isEnabled() && model->currentIndex()==1);
                             auto* textureQuality=dialog->findChild<QComboBox*>("textureReconstructionQuality");
                             CHECK(textureQuality && textureQuality->isEnabled());
                             textureQuality->setCurrentIndex(1);
                             model->setCurrentIndex(2);CHECK(textureQuality->isEnabled());
                             auto* displacement=dialog->findChild<QCheckBox*>("geometricDisplacement");
                             CHECK(displacement && displacement->isEnabled());displacement->setChecked(true);
                             model->setCurrentIndex(0);
                             if (argc > 1)
                             {
                                 timer.stop();
                                 refreshShellValues(dialog);
                                 for (int i = 0; i < tabs->count(); ++i)
                                 {
                                     tabs->setCurrentIndex(i);
                                     QEventLoop settle;QTimer::singleShot(260,&settle,&QEventLoop::quit);settle.exec();
                                     dialog->grab().save(QString(argv[1]) + QString::number(i) + ".png");
                                 }
                                 timer.start();
                             }
                         }
                         else
                         {
                             CHECK(previews > 0 && last.image.brightness == 1.75f &&
                                   last.image.saturation == 0);
                             CHECK(last.image.render_percent == 200 && last.image.fov_y == 110);
                             CHECK(last.image.enhanced == 1 && last.image.ao_quality == 2 && last.image.shadow_distance == 12);
                             CHECK(last.image.fragment_lighting == 0);
                             CHECK(last.image.particle_light_radius == 3.5f);
                             CHECK(last.image.geometry_refinement == 2 && last.image.material_enhancement == 1 && last.image.material_compare == 1 && last.image.pom_quality == 2 && last.image.texture_scale == 2 && last.image.texture_backend == 0 && last.image.displacement == 1);
                             CHECK(last.image.environment == 2 && last.image.sun_preset == 3 &&
                                   last.image.sun_temperature == 3300 && last.image.sun_elevation == 12 &&
                                   last.image.sun_azimuth == 120 && last.image.sun_shadows == 2);
                             dialog->reject();
                         }
                     });
    timer.start(200);
    CHECK(editGraphicsOptions(nullptr, options, true,
                              [&](const auto &state)
                              {
                                  last = state;
                                  ++previews;
                              }) == QDialog::Rejected);
    CHECK(options.image.brightness == 1 && last.image.brightness == 1 && last.image.render_percent == 100);
    CHECK(last.image.fov_y == 0);
    CHECK(last.image.enhanced == 0 && last.image.ao_quality == 0);
    CHECK(last.image.fragment_lighting == 1);
    CHECK(last.image.particle_light_radius == 2.f);
    CHECK(last.image.geometry_refinement == 0 && last.image.material_enhancement == 0 && last.image.material_compare == 0 && last.image.pom_quality == 0 && last.image.texture_scale == 1 && last.image.texture_backend == 1);
    CHECK(last.image.environment == 0 && last.image.sun_preset == 0);
    std::cout << "live preview and Cancel restoration passed\n";
}
