#include "message.h"
#include "gl/gltex.h"
#include "model/nifmodel.h"
#include "spells/tangentspace.h"

#include "lib/nvtristripwrapper.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>

#include "FBXCommon.h"

#define tr( x ) QApplication::tr( x )

QModelIndex FindSceneRoot(const NifModel * nif, const QModelIndex & iNode)
{
    auto links = iNode.isValid() ? nif->getChildLinks( nif->getBlockNumber( iNode ) ) : nif->getRootLinks();
    foreach ( int l,  links ) {
        QModelIndex iChild = nif->getBlock( l );

        if ( nif->inherits( iChild, "NiNode" ) )
        {
            if(nif->getBlockName(iChild) == "Scene_Root")
                return iChild;
            return FindSceneRoot(nif, iNode);
        }
    }

    return QModelIndex();
}

bool CreateScene(const NifModel * nif, FbxManager *pSdkManager, FbxScene* pScene, QString exportDir)
{
    // create scene info
    FbxDocumentInfo* sceneInfo = FbxDocumentInfo::Create(pSdkManager,"SceneInfo");
    sceneInfo->mTitle = "Nif";
    sceneInfo->mSubject = "Export from NifScope";
    sceneInfo->mAuthor = "Twisted";
    sceneInfo->mRevision = "rev. 1.0";
    sceneInfo->mKeywords = "NIF";
    sceneInfo->mComment = "";

    // we need to add the sceneInfo before calling AddThumbNailToScene because
    // that function is asking the scene for the sceneInfo.
    pScene->SetSceneInfo(sceneInfo);

    QList<int> roots;
    QModelIndex iRoot = FindSceneRoot(nif, QModelIndex());

    if (!iRoot.isValid() ) {
        qCCritical( nsIo ) << "Failed to find scene root";
        return false;
    }

    FbxNode* lRootNode = pScene->GetRootNode();

    FbxSkeleton* lSkeletonRootAttribute = FbxSkeleton::Create(pScene, "Skeleton");
    lSkeletonRootAttribute->SetSkeletonType(FbxSkeleton::eRoot);
    FbxNode* lSkeletonRoot = FbxNode::Create(pScene, "Skeleton Root");
    lSkeletonRoot->SetNodeAttribute(lSkeletonRootAttribute);
    lSkeletonRoot->LclTranslation.Set(FbxVector4(0.0, -40.0, 0.0));

    foreach ( const int l, nif->getChildLinks( nif->getBlockNumber( iRoot )) ) {
        QModelIndex iBlock = nif->getBlock( l );
        if ( nif->inherits( iBlock, "NiNode" ) )
        {
            //FbxNode* lSkeletonRoot = CreateSkeleton(pScene, "Skeleton");

            // Build the node tree.
           // FbxNode* lRootNode = pScene->GetRootNode();
            //lRootNode->AddChild(lSkeletonRoot);

            // Store poses
            //LinkPatchToSkeleton(pScene, lPatch, lSkeletonRoot);
            //StoreBindPose(pScene, lPatch);
            //StoreRestPose(pScene, lSkeletonRoot);

            // Animation
            //AnimateSkeleton(pScene, lSkeletonRoot);
            break;
        }
    }

    lRootNode->AddChild(lSkeletonRoot);

    return true;
}

void exportFBX( const NifModel * nif, const QModelIndex & index )
{
    Q_UNUSED(index);
    FbxManager* lSdkManager = NULL;
    FbxScene* lScene = NULL;
    bool lResult;

    QString exportDir = QFileDialog::getExistingDirectory( qApp->activeWindow(), tr( "Choose a folder for export" ));

    if ( exportDir.isEmpty() )
        return;

    // Prepare the FBX SDK.
    InitializeSdkObjects(lSdkManager, lScene);

    // Create the scene.
    lResult = CreateScene(nif, lSdkManager, lScene, exportDir);

    if(lResult == false)
    {
        qCCritical( nsIo ) << "An error occurred while creating the scene...";
        DestroySdkObjects(lSdkManager, lResult);
        return;
    }

    // Save the scene.
    QString exportFile = QString( "%1/%2.fbx" ).arg( exportDir ).arg( QFileInfo(nif->getFilename()).baseName() );
    lResult = SaveScene(lSdkManager, lScene, exportDir);

    if(lResult == false)
    {
        qCCritical( nsIo ) << "An error occurred while saving the scene...";
        DestroySdkObjects(lSdkManager, lResult);
        return;
    }

    // Destroy all objects created by the FBX SDK.
    DestroySdkObjects(lSdkManager, lResult);
}
