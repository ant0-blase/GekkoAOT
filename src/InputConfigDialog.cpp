#include "InputConfigDialog.hpp"
#include "SettingsStyle.hpp"
#include "ShellStyle.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
struct Control
{
    const char *name;
    const char *id;
    int type;
};
constexpr Control controls[] = {{"A", "a", 0},
                                {"B", "b", 0},
                                {"X", "x", 0},
                                {"Y", "y", 0},
                                {"Start / Pause", "start", 0},
                                {"Z", "z", 0},
                                {"L — digital click", "l", 0},
                                {"R — digital click", "r", 0},
                                {"D-Pad ↑", "dpad_up", 0},
                                {"D-Pad ↓", "dpad_down", 0},
                                {"D-Pad ←", "dpad_left", 0},
                                {"D-Pad →", "dpad_right", 0},
                                {"Control Stick — horizontal", "main_x", 1},
                                {"Control Stick — vertical", "main_y", 1},
                                {"C Stick — horizontal", "c_x", 1},
                                {"C Stick — vertical", "c_y", 1},
                                {"L — analog trigger", "l_analog", 2},
                                {"R — analog trigger", "r_analog", 2}};

// Vector artwork remains sharp at every display scale. The same paths are used
// for drawing and hit testing, so every pictured control selects its binding.
class ControllerDiagram final : public QWidget
{
  public:
    explicit ControllerDiagram(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName("gameCubeController");
        setAccessibleName("GameCube controller");
        setMinimumSize(480, 330);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        auto ellipse = [&](const char *id, QRectF r)
        {
            QPainterPath p;
            p.addEllipse(r);
            shapes.push_back({id, p});
        };
        auto pill = [&](const char *id, QRectF r, double angle = 0.)
        {
            QPainterPath p;
            p.addRoundedRect(r, r.height() / 2, r.height() / 2);
            QTransform t;
            t.translate(r.center().x(), r.center().y());
            t.rotate(angle);
            t.translate(-r.center().x(), -r.center().y());
            shapes.push_back({id, t.map(p)});
        };
        pill("l_analog", {108, 67, 88, 25}, -18);
        pill("r_analog", {405, 67, 88, 25}, 18);
        pill("z", {462, 83, 52, 17}, 27);
        ellipse("main_x", {116, 121, 80, 80});
        ellipse("c_x", {351, 251, 66, 66});
        ellipse("a", {428, 136, 60, 60});
        ellipse("b", {393, 190, 37, 37});
        pill("x", {490, 141, 25, 55}, -25);
        pill("y", {419, 105, 53, 25}, -24);
        ellipse("start", {286, 170, 24, 24});
        const auto part = [&](const char *id, QRectF r)
        {
            QPainterPath p;
            p.addRect(r);
            shapes.push_back({id, p});
        };
        part("dpad_up", {218, 256, 21, 22});
        part("dpad_down", {218, 295, 21, 22});
        part("dpad_left", {198, 276, 22, 21});
        part("dpad_right", {237, 276, 22, 21});
    }
    QString selected = "a";
    std::function<void(const QString &)> select;

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double scale = std::min(width() / 600., height() / 445.);
        p.translate((width() - 600 * scale) / 2, (height() - 445 * scale) / 2);
        p.scale(scale, scale);
        p.setPen(QPen(QColor("#161616"), 10, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(300, 0, 294, 65);
        QPainterPath body;
        body.moveTo(178, 74);
        body.cubicTo(208, 58, 249, 50, 294, 51);
        body.cubicTo(363, 50, 423, 60, 462, 84);
        body.cubicTo(520, 115, 535, 170, 538, 227);
        body.cubicTo(547, 290, 552, 357, 535, 398);
        body.cubicTo(526, 425, 501, 424, 486, 398);
        body.cubicTo(471, 374, 450, 337, 432, 312);
        body.cubicTo(400, 337, 369, 337, 343, 317);
        body.cubicTo(324, 303, 321, 278, 315, 257);
        body.cubicTo(307, 243, 286, 245, 278, 259);
        body.cubicTo(271, 282, 269, 307, 249, 321);
        body.cubicTo(222, 340, 191, 332, 166, 313);
        body.cubicTo(148, 341, 128, 381, 113, 405);
        body.cubicTo(96, 429, 73, 418, 66, 394);
        body.cubicTo(52, 356, 57, 293, 68, 235);
        body.cubicTo(69, 170, 81, 119, 126, 91);
        body.cubicTo(143, 82, 161, 78, 178, 74);
        body.closeSubpath();
        p.save();
        p.translate(0, 5);
        p.fillPath(body, QColor("#1b1b1b"));
        p.restore();
        QLinearGradient shell(180, 65, 400, 425);
        shell.setColorAt(0, QColor("#959595"));
        shell.setColorAt(.28, QColor("#6d6d6d"));
        shell.setColorAt(.7, QColor("#4d4d4d"));
        shell.setColorAt(1, QColor("#777777"));
        p.setPen(QPen(QColor("#a3a3a3"), 1.2));
        p.setBrush(shell);
        p.drawPath(body);
        // Curved seams distinguish the two grips from the three-lobed face.
        p.setPen(QPen(QColor("#444444"), 2));
        p.drawArc(QRectF(77, 85, 166, 165), 75 * 16, 218 * 16);
        p.drawArc(QRectF(352, 88, 165, 164), -110 * 16, 230 * 16);
        p.drawArc(QRectF(166, 230, 120, 105), 188 * 16, 154 * 16);
        p.drawArc(QRectF(323, 227, 120, 109), 199 * 16, 151 * 16);
        p.setPen(QColor("#dedede"));
        QFont brand = font();
        brand.setPixelSize(10);
        brand.setBold(true);
        brand.setLetterSpacing(QFont::AbsoluteSpacing, 3);
        p.setFont(brand);
        p.drawText(QRectF(220, 77, 150, 16), Qt::AlignCenter, "NINTENDO");
        brand.setLetterSpacing(QFont::AbsoluteSpacing, .8);
        brand.setPixelSize(16);
        p.setFont(brand);
        p.drawText(QRectF(213, 91, 170, 23), Qt::AlignCenter, "GAMECUBE");
        const auto octagon = [&](QPointF center, double radius)
        {
            QPainterPath path;
            for (int i = 0; i < 8; ++i)
            {
                const double a = (22.5 + i * 45) * 3.141592653589793 / 180.;
                QPointF point = center + QPointF(std::cos(a) * radius, std::sin(a) * radius);
                if (i == 0)
                    path.moveTo(point);
                else
                    path.lineTo(point);
            }
            path.closeSubpath();
            return path;
        };
        for (const auto &shape : shapes)
        {
            const QString id = shape.first;
            const auto bounds = shape.second.boundingRect();
            QColor fill("#c9c9c9");
            QString label;
            if (id.startsWith("dpad_"))
                continue;
            if (id == "a")
            {
                fill = QColor("#2dbfa6");
                label = "A";
            }
            else if (id == "b")
            {
                fill = QColor("#db3344");
                label = "B";
            }
            else if (id == "c_x")
                fill = QColor("#f2c739");
            else if (id == "z")
            {
                fill = QColor("#4b4b4b");
                label = "Z";
            }
            else if (id == "l_analog")
                label = "L";
            else if (id == "r_analog")
                label = "R";
            else if (id == "x" || id == "y")
                label = id.toUpper();
            const bool active = selected == id || (id == "main_x" && selected == "main_y") ||
                                (id == "c_x" && selected == "c_y") || (id == "l_analog" && selected == "l") ||
                                (id == "r_analog" && selected == "r");
            if (id == "main_x" || id == "c_x")
            {
                const auto gate = octagon(bounds.center(), bounds.width() * .64);
                p.setPen(QPen(QColor("#8c8c8c"), 2));
                p.setBrush(QColor(id == "c_x" ? "#bc851b" : "#383838"));
                p.drawPath(gate);
            }
            QRadialGradient button(bounds.center() - QPointF(8, 10), bounds.width() * .8);
            button.setColorAt(0, fill.lighter(116));
            button.setColorAt(1, fill.darker(125));
            p.setPen(QPen(QColor(active          ? "#ffffff"
                                 : hovered == id ? "#eeeeee"
                                                 : "#424242"),
                          active ? 2.5 : 1));
            p.setBrush(button);
            p.drawPath(shape.second);
            if (id == "main_x" || id == "c_x")
            {
                p.setPen(QPen(fill.darker(130), 1));
                const int rings = id == "main_x" ? 5 : 2;
                for (int i = 0; i < rings; ++i)
                {
                    const double inset = 10 + i * 4;
                    p.drawEllipse(bounds.adjusted(inset, inset, -inset, -inset));
                }
            }
            p.setPen(fill.darker(190));
            QFont f = font();
            f.setPixelSize(id == "a" ? 20 : 12);
            f.setBold(true);
            p.setFont(f);
            p.drawText(bounds, Qt::AlignCenter, label);
        }
        // One continuous cross, with directional hit regions kept separate.
        QPainterPath cross;
        cross.moveTo(219, 256);
        cross.lineTo(238, 256);
        cross.lineTo(238, 277);
        cross.lineTo(259, 277);
        cross.lineTo(259, 296);
        cross.lineTo(238, 296);
        cross.lineTo(238, 317);
        cross.lineTo(219, 317);
        cross.lineTo(219, 296);
        cross.lineTo(198, 296);
        cross.lineTo(198, 277);
        cross.lineTo(219, 277);
        cross.closeSubpath();
        p.setBrush(QColor("#c8c8c8"));
        p.setPen(QPen(QColor(selected.startsWith("dpad_") ? "#ffffff" : "#494949"),
                      selected.startsWith("dpad_") ? 2.5 : 1.5));
        p.drawPath(cross);
        p.setPen(QPen(QColor("#a0a0a0"), 1));
        p.drawEllipse(QRectF(220, 278, 16, 16));
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const auto id = hit(e->position());
        if (hovered != id)
        {
            hovered = id;
            setCursor(id.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
            update();
        }
    }
    void leaveEvent(QEvent *) override
    {
        hovered.clear();
        update();
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton)
        {
            const auto id = hit(e->position());
            if (!id.isEmpty() && select)
                select(id);
        }
    }

  private:
    QString hovered;
    QVector<QPair<QString, QPainterPath>> shapes;
    QString hit(QPointF position) const
    {
        const double scale = std::min(width() / 600., height() / 445.);
        position = (position - QPointF((width() - 600 * scale) / 2, (height() - 445 * scale) / 2)) / scale;
        for (auto i = shapes.crbegin(); i != shapes.crend(); ++i)
            if (i->second.contains(position))
            {
                if (i->first == "main_x" || i->first == "c_x")
                {
                    const QPointF delta = position - i->second.boundingRect().center();
                    if (std::abs(delta.y()) > std::abs(delta.x()))
                        return i->first == "main_x" ? "main_y" : "c_y";
                }
                return i->first;
            }
        return {};
    }
};
} // namespace

int editInputProfiles(QWidget *parent, QVector<InputPortProfile> &profiles, const QStringList &devices)
{
    auto draft = profiles;
    while (draft.size() < 4)
        draft.append(defaultInputPortProfile(draft.size()));
    QDialog dialog(parent);
    dialog.setObjectName("inputSettings");
    dialog.setWindowTitle("Input Settings");
    dialog.setStyleSheet(settingsStyleSheet());
    dialog.resize(900, 780);
    dialog.setMinimumSize(760, 710);
    auto *outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(22, 16, 22, 18);
    outer->setSpacing(13);
    auto *ports = new QTabBar;
    ports->setObjectName("inputPorts");
    ports->setExpanding(false);
    for (int i = 1; i <= 4; ++i)
        ports->addTab(QString("Port %1").arg(i));
    outer->addWidget(ports);
    auto *deviceRow = new QHBoxLayout;
    auto *source = new QComboBox;
    source->setObjectName("inputSource");
    for (const auto &pair : QList<QPair<QString, QString>>{{"Automatic", "auto"},
                                                           {"Keyboard", "keyboard"},
                                                           {"Gamepad", "gamepad"},
                                                           {"Joystick", "joystick"},
                                                           {"Disabled", "disabled"}})
        source->addItem(pair.first, pair.second);
    source->setToolTip("Automatic combines the keyboard with the first compatible controller.");
    auto *device = new QComboBox;
    device->setObjectName("inputDevice");
    device->setEditable(true);
    device->addItem("");
    device->addItems(devices);
    device->lineEdit()->setPlaceholderText("Any controller");
    device->setToolTip("Leave empty for automatic selection, or enter part of the device name.");
    deviceRow->addWidget(new QLabel("Source"));
    deviceRow->addWidget(source, 1);
    deviceRow->addSpacing(20);
    deviceRow->addWidget(new QLabel("Device"));
    deviceRow->addWidget(device, 2);
    outer->addLayout(deviceRow);
    auto *diagram = new ControllerDiagram;
    outer->addWidget(diagram, 1);
    auto *instruction = new QLabel("Select a control on the controller to edit its binding.");
    instruction->setProperty("muted", true);
    instruction->setAlignment(Qt::AlignCenter);
    outer->addWidget(instruction);
    auto *selection = new QComboBox;
    selection->setObjectName("inputControl");
    for (const auto &c : controls)
        selection->addItem(QString::fromUtf8(c.name), QString::fromLatin1(c.id));
    outer->addWidget(selection);
    auto *editors = new QStackedWidget;
    outer->addWidget(editors);
    const auto form = [&]
    {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->setContentsMargins(0, 4, 0, 0);
        f->setHorizontalSpacing(24);
        f->setVerticalSpacing(10);
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        editors->addWidget(w);
        return f;
    };
    const auto key = [](const char *name)
    {
        auto *k = new QKeySequenceEdit;
        k->setObjectName(name);
        k->setMaximumSequenceLength(1);
        return k;
    };
    const auto joy = [](const char *name, int max)
    {
        auto *s = new QSpinBox;
        s->setObjectName(name);
        s->setRange(-1, max);
        s->setSpecialValueText("None");
        return s;
    };
    const auto axis = [](const char *name)
    {
        auto *c = new QComboBox;
        c->setObjectName(name);
        for (const auto &p : QList<QPair<QString, QString>>{{"None", ""},
                                                            {"Left X", "leftx"},
                                                            {"Left Y", "lefty"},
                                                            {"Right X", "rightx"},
                                                            {"Right Y", "righty"},
                                                            {"Left trigger", "left_trigger"},
                                                            {"Right trigger", "right_trigger"}})
            c->addItem(p.first, p.second);
        return c;
    };
    auto *digital = form();
    auto *buttonKey = key("buttonKey");
    digital->addRow("Keyboard", buttonKey);
    auto *button = new QComboBox;
    button->setObjectName("gamepadButton");
    for (const auto &p : QList<QPair<QString, QString>>{{"None", ""},
                                                        {"South / A", "south"},
                                                        {"East / B", "east"},
                                                        {"West / X", "west"},
                                                        {"North / Y", "north"},
                                                        {"Start", "start"},
                                                        {"Back", "back"},
                                                        {"Guide", "guide"},
                                                        {"Left shoulder", "left_shoulder"},
                                                        {"Right shoulder", "right_shoulder"},
                                                        {"Left stick", "left_stick"},
                                                        {"Right stick", "right_stick"},
                                                        {"D-Pad Up", "dpad_up"},
                                                        {"D-Pad Down", "dpad_down"},
                                                        {"D-Pad Left", "dpad_left"},
                                                        {"D-Pad Right", "dpad_right"}})
        button->addItem(p.first, p.second);
    digital->addRow("Gamepad button", button);
    auto *buttonJoy = joy("joystickButton", 63);
    digital->addRow("Joystick button", buttonJoy);
    auto *sticks = form();
    auto *negative = key("negativeKey");
    auto *positive = key("positiveKey");
    auto *keyRow = new QHBoxLayout;
    keyRow->addWidget(new QLabel("−"));
    keyRow->addWidget(negative, 1);
    keyRow->addSpacing(16);
    keyRow->addWidget(new QLabel("+"));
    keyRow->addWidget(positive, 1);
    sticks->addRow("Keyboard", keyRow);
    auto *stickAxis = axis("stickAxis");
    auto *invert = new QCheckBox("Invert");
    auto *axisRow = new QHBoxLayout;
    axisRow->addWidget(stickAxis, 1);
    axisRow->addWidget(invert);
    sticks->addRow("Gamepad axis", axisRow);
    auto *stickJoy = joy("joystickAxis", 31);
    sticks->addRow("Joystick axis", stickJoy);
    auto *deadzone = new QSlider(Qt::Horizontal);
    deadzone->setObjectName("stickDeadzone");
    deadzone->setRange(0, 30000);
    deadzone->setSingleStep(128);
    auto *deadzoneValue = new QLabel;
    deadzoneValue->setFixedWidth(52);
    auto *dzRow = new QHBoxLayout;
    dzRow->addWidget(deadzone, 1);
    dzRow->addWidget(deadzoneValue);
    sticks->addRow("Deadzone", dzRow);
    auto *triggers = form();
    auto *triggerKey = key("triggerKey");
    triggers->addRow("Keyboard", triggerKey);
    auto *triggerAxis = axis("triggerAxis");
    triggers->addRow("Gamepad axis", triggerAxis);
    auto *triggerJoy = joy("joystickTrigger", 31);
    triggers->addRow("Joystick axis", triggerJoy);
    auto *footer = new QHBoxLayout;
    auto *reset = new QPushButton("Reset port");
    reset->setObjectName("resetInputPort");
    footer->addWidget(reset);
    footer->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    footer->addWidget(buttons);
    outer->addLayout(footer);
    int port = 0;
    bool loading = false;
    const auto setData = [](QComboBox *c, const QString &v)
    {
        int i = c->findData(v);
        if (i < 0)
        {
            c->addItem(v, v);
            i = c->count() - 1;
        }
        c->setCurrentIndex(i);
    };
    const auto load = [&]
    {
        loading = true;
        const auto &p = draft[port];
        const auto &c = controls[selection->currentIndex()];
        const QString id = c.id;
        setData(source, p.source);
        device->setCurrentText(p.device);
        editors->setCurrentIndex(c.type);
        const auto seq = [&](const QString &suffix)
        { return QKeySequence::fromString(p.values.value(id + suffix), QKeySequence::PortableText); };
        if (c.type == 0)
        {
            buttonKey->setKeySequence(seq("_key"));
            setData(button, p.values.value(id + "_button"));
            buttonJoy->setValue(p.values.value(id + "_joy_button", "-1").toInt());
        }
        else if (c.type == 1)
        {
            negative->setKeySequence(seq("_negative_key"));
            positive->setKeySequence(seq("_positive_key"));
            setData(stickAxis, p.values.value(id + "_axis"));
            stickJoy->setValue(p.values.value(id + "_joy_axis", "-1").toInt());
            invert->setChecked(p.values.value(id + "_invert") == "true");
            deadzone->setValue(p.values.value(id + "_deadzone", "4096").toInt());
        }
        else
        {
            triggerKey->setKeySequence(seq("_key"));
            setData(triggerAxis, p.values.value(id + "_axis"));
            triggerJoy->setValue(p.values.value(id + "_joy_axis", "-1").toInt());
        }
        deadzoneValue->setText(QString::number(deadzone->value() * 100. / 32767., 'f', 1) + " %");
        diagram->selected = id;
        diagram->update();
        const bool enabled = p.source != "disabled";
        diagram->setEnabled(enabled);
        selection->setEnabled(enabled);
        editors->setEnabled(enabled);
        device->setEnabled(p.source != "disabled" && p.source != "keyboard");
        loading = false;
    };
    const auto saveBinding = [&]
    {
        if (loading)
            return;
        auto &values = draft[port].values;
        const auto &c = controls[selection->currentIndex()];
        const QString id = c.id;
        const auto str = [](QKeySequenceEdit *k)
        { return k->keySequence().toString(QKeySequence::PortableText); };
        if (c.type == 0)
        {
            values[id + "_key"] = str(buttonKey);
            values[id + "_button"] = button->currentData().toString();
            values[id + "_joy_button"] = QString::number(buttonJoy->value());
        }
        else if (c.type == 1)
        {
            values[id + "_negative_key"] = str(negative);
            values[id + "_positive_key"] = str(positive);
            values[id + "_axis"] = stickAxis->currentData().toString();
            values[id + "_joy_axis"] = QString::number(stickJoy->value());
            values[id + "_invert"] = invert->isChecked() ? "true" : "false";
            values[id + "_deadzone"] = QString::number(deadzone->value());
        }
        else
        {
            values[id + "_key"] = str(triggerKey);
            values[id + "_axis"] = triggerAxis->currentData().toString();
            values[id + "_joy_axis"] = QString::number(triggerJoy->value());
        }
        deadzoneValue->setText(QString::number(deadzone->value() * 100. / 32767., 'f', 1) + " %");
    };
    for (auto *k : {buttonKey, negative, positive, triggerKey})
        QObject::connect(k, &QKeySequenceEdit::keySequenceChanged, &dialog, saveBinding);
    for (auto *c : {button, stickAxis, triggerAxis})
        QObject::connect(c, &QComboBox::currentIndexChanged, &dialog, saveBinding);
    for (auto *s : {buttonJoy, stickJoy, triggerJoy})
        QObject::connect(s, qOverload<int>(&QSpinBox::valueChanged), &dialog, saveBinding);
    QObject::connect(invert, &QCheckBox::toggled, &dialog, saveBinding);
    QObject::connect(deadzone, &QSlider::valueChanged, &dialog, saveBinding);
    QObject::connect(selection, &QComboBox::currentIndexChanged, &dialog, load);
    QObject::connect(ports, &QTabBar::currentChanged, &dialog,
                     [&](int i)
                     {
                         port = i;
                         load();
                     });
    QObject::connect(source, &QComboBox::currentIndexChanged, &dialog,
                     [&]
                     {
                         if (!loading)
                         {
                             draft[port].source = source->currentData().toString();
                             load();
                         }
                     });
    QObject::connect(device, &QComboBox::editTextChanged, &dialog,
                     [&](const QString &s)
                     {
                         if (!loading)
                             draft[port].device = s.trimmed();
                     });
    diagram->select = [&](const QString &id) { selection->setCurrentIndex(selection->findData(id)); };
    QObject::connect(reset, &QPushButton::clicked, &dialog,
                     [&]
                     {
                         draft[port] = defaultInputPortProfile(port);
                         load();
                     });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    load();
    presentShellDialog(dialog,"Controllers",shellTVMode(parent));
    const int result = execShellDialog(dialog);
    if (result == QDialog::Accepted)
        profiles = draft;
    return result;
}
