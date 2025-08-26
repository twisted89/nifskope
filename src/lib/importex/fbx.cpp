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

bool CreateScene(const NifModel * nif, FbxManager *pSdkManager, FbxScene* pScene)
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
    QModelIndex iBlock = FindSceneRoot(nif, QModelIndex());

    if (!iBlock.isValid() ) {
        qCCritical( nsIo ) << "Failed to find scene root";
        return false;
    }

    foreach ( int l, roots ) {
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

    return true;
}

void exportFBX( const NifModel * nif, const QModelIndex & index )
{
    FbxManager* lSdkManager = NULL;
    FbxScene* lScene = NULL;
    bool lResult;

    // Prepare the FBX SDK.
    InitializeSdkObjects(lSdkManager, lScene);

    // Create the scene.
    lResult = CreateScene(nif, lSdkManager, lScene);

    if(lResult == false)
    {
        qCCritical( nsIo ) << "An error occurred while creating the scene...";
        DestroySdkObjects(lSdkManager, lResult);
        return;
    }

    // Save the scene.
    QSettings settings;
    settings.beginGroup( "Import-Export" );
    settings.beginGroup( "FBX" );
    QString fname = QFileDialog::getSaveFileName( qApp->activeWindow(), tr( "Choose a .FBX file for export" ), settings.value( "File Name" ).toString(), "FBX (*.fbx)" );

    if ( fname.isEmpty() )
        return;

    lResult = SaveScene(lSdkManager, lScene, fname.toUtf8().data());

    if(lResult == false)
    {
        qCCritical( nsIo ) << "An error occurred while saving the scene...";
        DestroySdkObjects(lSdkManager, lResult);
        return;
    }

    // Destroy all objects created by the FBX SDK.
    DestroySdkObjects(lSdkManager, lResult);
}
