#include "exportcommon.h"
#include <filesystem>

bool CreateDirectoryRecursive(std::string const & dirName)
{
    std::error_code err;
    if (!std::filesystem::create_directories(dirName, err))
    {
        if (std::filesystem::exists(dirName))
        {
            // The folder already exists:
            return true;
        }
        return false;
    }
    return true;
}

QModelIndex FindSceneRoot(const NifModel* nif, const QModelIndex& iNode)
{
    auto links = iNode.isValid() ? nif->getChildLinks(nif->getBlockNumber(iNode)) : nif->getRootLinks();
    foreach(int l, links) {
        QModelIndex iChild = nif->getBlock(l);

        if (nif->inherits(iChild, "NiNode")) {
            if (nif->get<QString>(iChild, "Name") == "Scene_Root")
                return iChild;
            auto result = FindSceneRoot(nif, iChild);
            if (result.isValid())
                return result;
        }
    }

    return QModelIndex();
}
