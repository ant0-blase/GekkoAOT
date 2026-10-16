#include "../../src/InputConfigDialog.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QKeySequenceEdit>
#include <QSlider>
#include <QTabBar>
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
    QVector<InputPortProfile> profiles;
    for (int i = 0; i < 4; ++i)
        profiles.append(defaultInputPortProfile(i));
    profiles[0].values["future_setting"] = "preserve me";
    QTimer timer;
    int phase = 0;
    QObject::connect(
        &timer, &QTimer::timeout,
        [&]
        {
            auto *d = qobject_cast<QDialog *>(app.activeModalWidget());
            if (!d)
                return;
            CHECK(d->findChild<QWidget *>("gameCubeController"));
            auto *ports = d->findChild<QTabBar *>("inputPorts");
            auto *control = d->findChild<QComboBox *>("inputControl");
            CHECK(ports && ports->count() == 4 && control && control->count() == 18);
            if (phase == 0)
            {
                d->findChild<QKeySequenceEdit *>("buttonKey")->setKeySequence(QKeySequence("F9"));
                control->setCurrentIndex(control->findData("main_y"));
                d->findChild<QSlider *>("stickDeadzone")->setValue(10000);
                ports->setCurrentIndex(1);
                auto *source = d->findChild<QComboBox *>("inputSource");
                source->setCurrentIndex(source->findData("keyboard"));
                control->setCurrentIndex(control->findData("a"));
                d->findChild<QKeySequenceEdit *>("buttonKey")->setKeySequence(QKeySequence("F8"));
                ports->setCurrentIndex(0);
                control->setCurrentIndex(control->findData("a"));
                CHECK(d->findChild<QKeySequenceEdit *>("buttonKey")->keySequence() == QKeySequence("F9"));
                if (argc > 1)
                    d->grab().save(argv[1]);
                ++phase;
                d->accept();
            }
            else
            {
                d->findChild<QKeySequenceEdit *>("buttonKey")->setKeySequence(QKeySequence("F7"));
                d->reject();
            }
        });
    timer.start(150);
    CHECK(editInputProfiles(nullptr, profiles, {"Example gamepad"}) == QDialog::Accepted);
    CHECK(profiles[0].values["a_key"] == "F9" && profiles[0].values["main_y_deadzone"] == "10000");
    CHECK(profiles[1].values["a_key"] == "F8" && profiles[1].source == "keyboard");
    CHECK(profiles[0].values["future_setting"] == "preserve me");
    CHECK(profiles[2].source == "disabled" && profiles[2].values == defaultInputPortProfile(2).values);
    CHECK(editInputProfiles(nullptr, profiles, {}) == QDialog::Rejected);
    CHECK(profiles[0].values["a_key"] == "F9");
    std::cout << "Per-port bindings, unknown values, Save and Cancel passed\n";
}
