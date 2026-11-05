#pragma once
class QWidget;

// Directional navigation follows visible rows. Focusing a control also reveals
// it in enclosing scroll areas. Buttons, steppers and tabs share these rules.
void shellFocusControl(QWidget *widget);
bool shellMoveFocus(QWidget *scope, QWidget *current, int key);
bool shellNavigate(QWidget *scope, int key);
