#include "ShellKeyboard.hpp"
#include "ShellStyle.hpp"
#include <QDialog>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
void showShellKeyboard(QLineEdit *field) {
  QPointer<QLineEdit> target = field;
  QWidget *host = field->window();
  QDialog dialog(host);
  dialog.setObjectName("shellKeyboard");
  dialog.setProperty("shellSpatial", true);
  dialog.resize(900, 480);
  auto *layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(24, 24, 24, 24);
  layout->setSpacing(14);
  auto *preview = new QLabel(field->text());
  preview->setObjectName("keyboardPreview");
  preview->setTextFormat(Qt::PlainText);
  preview->setWordWrap(true);
  preview->setMinimumHeight(48);
  preview->setStyleSheet(
      "font-size:22px; padding:10px; background:rgba(255,255,255,10); "
      "border-radius:12px;");
  layout->addWidget(preview);
  auto *grid = new QGridLayout;
  grid->setSpacing(6);
  layout->addLayout(grid);
  QString text = field->text();
  bool upper = true;
  QList<QPushButton *> letters;
  auto refresh = [&] {
    preview->setText(text.isEmpty() ? QString(" ") : text);
  };
  const QStringList rows = {"1234567890", "QWERTYUIOP", "ASDFGHJKL'",
                            "ZXCVBNM.,-"};
  QPushButton *first = nullptr;
  for (int row = 0; row < rows.size(); ++row)
    for (int col = 0; col < rows[row].size(); ++col) {
      const QString value = rows[row].mid(col, 1);
      auto *key = new QPushButton(value);
      key->setMinimumHeight(40);
      key->setObjectName("keyboardKey" + value);
      grid->addWidget(key, row, col);
      letters << key;
      if (!first)
        first = key;
      QObject::connect(key, &QPushButton::clicked, &dialog, [&, value] {
        if (text.size() < field->maxLength())
          text += upper ? value : value.toLower();
        refresh();
      });
    }
  auto add = [&](const QString &label, int col, int span, auto callback) {
    auto *b = new QPushButton(label);
    b->setMinimumHeight(40);
    grid->addWidget(b, 4, col, 1, span);
    QObject::connect(b, &QPushButton::clicked, &dialog, callback);
    return b;
  };
  add("Shift", 0, 2, [&] {
    upper = !upper;
    for (auto *b : letters)
      b->setText(upper ? b->text().toUpper() : b->text().toLower());
  });
  add("Space", 2, 3, [&] {
    text += " ";
    refresh();
  });
  add("⌫", 5, 2, [&] {
    text.chop(1);
    refresh();
  });
  add("Clear", 7, 1, [&] {
    text.clear();
    refresh();
  });
  add("Done", 8, 2, [&] { dialog.accept(); });
  presentShellDialog(dialog, "Keyboard", shellTVMode(field));
  QTimer::singleShot(10, &dialog, [first] { first->setFocus(); });
  if (execShellDialog(dialog) == QDialog::Accepted && target) {
    target->setText(text);
    target->setFocus();
  }
}
