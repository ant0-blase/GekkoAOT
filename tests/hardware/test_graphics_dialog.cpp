// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/GraphicsConfigDialog.hpp"
#include <QApplication>
#include <QDoubleSpinBox>
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
                             CHECK(tabs && tabs->count() == 4);
                             dialog->findChild<QDoubleSpinBox *>("brightness")->setValue(1.75);
                             dialog->findChild<QDoubleSpinBox *>("saturation")->setValue(0);
                             dialog->findChild<QDoubleSpinBox *>("renderPercent")->setValue(200);
                             CHECK(dialog->findChild<QLabel *>("renderDimensions")->text() == "3840 × 2400");
                             if (argc > 1)
                             {
                                 for (int i = 0; i < tabs->count(); ++i)
                                 {
                                     tabs->setCurrentIndex(i);
                                     app.processEvents();
                                     dialog->grab().save(QString(argv[1]) + QString::number(i) + ".png");
                                 }
                             }
                         }
                         else
                         {
                             CHECK(previews > 0 && last.image.brightness == 1.75f &&
                                   last.image.saturation == 0);
                             CHECK(last.image.render_percent == 200);
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
    std::cout << "live preview and Cancel restoration passed\n";
}
