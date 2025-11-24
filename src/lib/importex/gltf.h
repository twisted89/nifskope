#pragma once

#include "gl/gltex.h"
#include "model/nifmodel.h"
#include <QModelIndex>
#include <QString>

//! Export NIF to glTF 2.0 format
void exportGLTF(const NifModel* nif, const QModelIndex& index);
