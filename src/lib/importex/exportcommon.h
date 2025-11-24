#ifndef EXPORTCOMMON_H
#define EXPORTCOMMON_H
#include <string>
#include "model/nifmodel.h"

bool CreateDirectoryRecursive(std::string const & dirName);
QModelIndex FindSceneRoot(const NifModel* nif, const QModelIndex& iNode);

#endif // EXPORTCOMMON_H
