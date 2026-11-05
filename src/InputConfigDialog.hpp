#pragma once
#include "InputProfile.hpp"
class QWidget;
// Edits a copy; Cancel never changes the caller's profiles.
int editInputProfiles(QWidget *parent, QVector<InputPortProfile> &profiles, const QStringList &devices);
