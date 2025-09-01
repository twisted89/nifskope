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
            if(nif->get<QString>( iChild, "Name" ) == "Scene_Root")
                return iChild;
            auto result = FindSceneRoot(nif, iChild);
            if( result.isValid() )
                return result;
        }
    }

    return QModelIndex();
}

bool HasChildBone(const NifModel * nif, const QModelIndex & iNode)
{
    auto links = nif->getChildLinks( nif->getBlockNumber( iNode ) );
    foreach ( int l,  links ) {
        QModelIndex iChild = nif->getBlock( l );

        if ( nif->isNiBlock( iChild, "Ni3dsAnimationNode") || nif->inherits( iChild, "Ni3dsAnimationNode" ) )
        {
            return true;
        }

        if ( nif->inherits( iChild, "NiNode" ) )
        {
            if( HasChildBone(nif, iChild) )
                return true;
        }
    }

    return false;
}


void WriteNode(const NifModel * nif, const QModelIndex & iNode, FbxScene* pScene, FbxNode* parentnode)
{
    foreach ( const int l, nif->getChildLinks( nif->getBlockNumber( iNode )) ) {
        QModelIndex iBlock = nif->getBlock( l );
        if(nif->isNiBlock( iBlock, "NiNode") || nif->inherits( iBlock, "NiNode" ))
        {
            auto t = Transform( nif, iBlock );
            auto nodeName = nif->get<QString>( iBlock, "Name" ).toStdString();
            FbxNode* node = node = FbxNode::Create(pScene, nodeName.c_str() );
            if(node) {

                FbxSkeleton* lSkeletonLimbNodeAttribute1 = FbxSkeleton::Create(pScene, nodeName.c_str());
                lSkeletonLimbNodeAttribute1->SetSkeletonType( FbxSkeleton::eLimb ); //HasChildBone(nif, iBlock) ?  FbxSkeleton::eLimb : FbxSkeleton::eEffector
                node->SetNodeAttribute(lSkeletonLimbNodeAttribute1);

                Eigen::Vector3d pos = t.translation.toYUp();
                Eigen::Vector3d rot = t.rotation.toEulerXYZ();

                node->LclTranslation.Set(FbxDouble3(pos.x(), pos.y(), pos.z()));
                node->LclRotation.Set(FbxDouble3(rot.x() / PI * 180, rot.y() / PI * 180, rot.z() / PI * 180));

                parentnode->AddChild(node);
                WriteNode(nif, iBlock, pScene, node);
            }
            else
            {
                qCCritical( nsIo ) << "Failed to create node with block ID" << nif->getBlockNumber( iBlock );
            }
        }
    }
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
    lRootNode->LclTranslation.Set(FbxVector4(0.0, 0.0, 0.0));

    FbxSkeleton* lSkeletonRootAttribute = FbxSkeleton::Create(pScene, "Skeleton");
    lSkeletonRootAttribute->SetSkeletonType(FbxSkeleton::eRoot);
    FbxNode* lSkeletonRoot = FbxNode::Create(pScene, "Scene_Root");
    lSkeletonRoot->SetNodeAttribute(lSkeletonRootAttribute);
    lSkeletonRoot->LclTranslation.Set(FbxVector4(0.0, 0.0, 0.0));

    WriteNode(nif, iRoot, pScene, lSkeletonRoot);

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

/*
    if(lScene->GetGlobalSettings().GetSystemUnit() == FbxSystemUnit::cm)
    {
        const FbxSystemUnit::ConversionOptions lConversionOptions = {
            true, // mConvertRrsNodes
            true, // mConvertLimits
            true, // mConvertClusters
            true, // mConvertLightIntensity
            true, // mConvertPhotometricLProperties
            true  // mConvertCameraClipPlanes
        };

        // Convert the scene to meters using the defined options.
        FbxSystemUnit::m.ConvertScene(lScene, lConversionOptions);
    }
    */

    // Save the scene.
    auto exportFile = QString( "%1/%2.fbx" ).arg( exportDir ).arg( QFileInfo(nif->getFilename()).baseName() ).toStdString();
    lResult = SaveScene(lSdkManager, lScene, exportFile);

    if(lResult == false)
    {
        qCCritical( nsIo ) << "An error occurred while saving the scene...";
        DestroySdkObjects(lSdkManager, lResult);
        return;
    }

    // Destroy all objects created by the FBX SDK.
    DestroySdkObjects(lSdkManager, lResult);
}
