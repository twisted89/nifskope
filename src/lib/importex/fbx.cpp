#include "message.h"
#include "gl/gltex.h"
#include "model/nifmodel.h"
#include "lib/nvtristripwrapper.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>
#include <filesystem>

#include "FBXCommon.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define tr( x ) QApplication::tr( x )

struct FBX_ExportContext {
    std::string ExportPath;
    std::map<uint, FbxNode*> NodeMap;
    FbxArray<FbxNode*> NodeLinksArray;
};

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

void ProcessNode(const NifModel * nif, const QModelIndex & iNode, FbxScene* pScene, FbxNode* parentnode, FBX_ExportContext &ctx)
{
    foreach ( const int l, nif->getChildLinks( nif->getBlockNumber( iNode )) ) {
        QModelIndex iBlock = nif->getBlock( l );
        auto t = Transform( nif, iBlock );
        auto blockName = nif->get<QString>( iBlock, "Name" ).toStdString();

        if(nif->isNiBlock( iBlock, "NiNode") || nif->inherits( iBlock, "NiNode" ))
        {
            FbxNode* node = node = FbxNode::Create(pScene, blockName.c_str() );
            if(node) {

                ctx.NodeMap[nif->getBlockNumber( iBlock )] = node;

                FbxSkeleton* lSkeletonLimbNodeAttribute1 = FbxSkeleton::Create(pScene, blockName.c_str());
                lSkeletonLimbNodeAttribute1->SetSkeletonType( FbxSkeleton::eLimb ); //HasChildBone(nif, iBlock) ?  FbxSkeleton::eLimb : FbxSkeleton::eEffector
                node->AddNodeAttribute(lSkeletonLimbNodeAttribute1);

                Eigen::Vector3d pos = t.translation.toYUp();
                Eigen::Vector3d rot = t.rotation.toEulerXYZ();

                node->LclTranslation.Set(FbxDouble3(pos.x(), pos.y(), pos.z()));
                node->LclRotation.Set(FbxDouble3(rot.x() / PI * 180, rot.y() / PI * 180, rot.z() / PI * 180));

                parentnode->AddChild(node);

                ProcessNode(nif, iBlock, pScene, node, ctx);
            }
            else
            {
                qCCritical( nsIo ) << "Failed to create node with block ID" << nif->getBlockNumber( iBlock );
            }
        }
        else if(nif->isNiBlock( iBlock, "NiTextureProperty"))
        {
            foreach ( const int cl, nif->getChildLinks( nif->getBlockNumber( iNode )) ) {
                QModelIndex ciBlock = nif->getBlock( cl );
                if(nif->isNiBlock( ciBlock, "NiImage"))
                {
                    QModelIndex iImage = nif->getBlock( nif->getLink( ciBlock, "Image Data" ));
                    if(nif->getBlockName(iImage) == "NiRawImageData")
                    {
                        auto width  = nif->get<uint>( iImage, "Width" );
                        auto height = nif->get<uint>( iImage, "Height" );
                        auto type = nif->get<int>( iImage, "Image Type" );

                        int components;
                        switch(type)
                        {
                        case 1: //RGB
                            components = 3;
                            break;
                        case 2: // RGBA
                            components = 4;
                            break;
                        default:
                            continue;
                        }

                        QModelIndex iPixelData = nif->getIndex( iImage, "RGBA Image Data" );

                        if ( iPixelData.isValid() ) {
                            if ( QByteArray * pdata = nif->get<QByteArray *>( iPixelData.child(0, 0) ) ) {

                                const std::string filename = ctx.ExportPath
                                                             + "/Textures/"
                                                             + std::to_string(nif->getBlockNumber( iNode ))
                                                             + "_" + std::to_string(nif->getBlockNumber( ciBlock ))
                                                             + ".png";

                                // Save the image
                                stbi_write_png(filename.c_str(), width, height, components, pdata->data(), 100);
                            }
                        }
                    }
                }
            }
        }
    }
}

void AddNodeRecursively(FBX_ExportContext &ctx, FbxNode* pNode)
{
    if (pNode)
    {
        AddNodeRecursively(ctx, pNode->GetParent());

        if (ctx.NodeLinksArray.Find(pNode) == -1)
        {
            // Node not in the list, add it
            ctx.NodeLinksArray.Add(pNode);
        }
    }
}

float precision( float f, int places )
{
    float n = std::pow(10.0f, places ) ;
    return std::round(f * n) / n ;
}

FbxAMatrix GetRelativeTransform(FbxNode* pChildNode, FbxNode* pParentNode, FbxTime pTime = FBXSDK_TIME_INFINITE) {
    if (!pChildNode || !pParentNode) {
        return FbxAMatrix(); // Return an identity matrix or handle error
    }

    // Get the global transform of the child node at the specified time
    FbxAMatrix childGlobalTransform = pChildNode->EvaluateGlobalTransform(pTime);

    // Get the global transform of the desired parent node at the specified time
    FbxAMatrix parentGlobalTransform = pParentNode->EvaluateGlobalTransform(pTime);
    FbxAMatrix parentGlobalTransformInverse = parentGlobalTransform.Inverse();

    // The relative transform is calculated by: Inverse(Parent) * Child
    FbxAMatrix relativeTransform = parentGlobalTransformInverse * childGlobalTransform;

    return relativeTransform;
}

void ProcessMeshes(const NifModel * nif, const QModelIndex & iNode, FbxScene* pScene, FBX_ExportContext &ctx)
{   
    foreach ( const int l, nif->getChildLinks( nif->getBlockNumber( iNode )) ) {
        QModelIndex iBlock = nif->getBlock( l );

        if(nif->isNiBlock( iBlock, "NiNode") || nif->inherits( iBlock, "NiNode" ))
        {
            ProcessMeshes(nif, iBlock, pScene, ctx);
        }
        else if(nif->inherits( iBlock, "NiSkinCore" ) || nif->itemName( iBlock ) == "NiTriShape"
                 || nif->inherits( iBlock, "NiTriShape" ))
        {
            auto meshTranslation = Transform( nif, iBlock ).translation.toYUp();
            auto blockName = nif->get<QString>( iBlock, "Name" ).toStdString();

            FbxNode* meshNode = FbxNode::Create(pScene, "");
            FbxMesh* mesh = FbxMesh::Create(pScene, blockName.c_str());

            auto parentnode = ctx.NodeMap[nif->getBlockNumber( iNode )];
            if(!parentnode)
            {
                qCCritical( nsIo ) << "Failed to find node attached to mesh with block ID" << nif->getBlockNumber( iNode );
                continue;
            }

            meshNode->LclTranslation.Set(FbxVector4(meshTranslation.x(), meshTranslation.y(), meshTranslation.z()));

            parentnode->AddChild(meshNode);
            meshNode->SetNodeAttribute(mesh);

            QVector<Vector3> verts  = nif->getArray<Vector3>( iBlock, "Vertices" );
            QVector<Vector3> norms  = nif->getArray<Vector3>( iBlock, "Normals" );
            QVector<Triangle> triangles;
            QVector<FbxVector2> textureCoords;

            if ( norms.count() < verts.count() )
                norms.clear();

            QModelIndex uvcoord = nif->getIndex( iBlock, "UV Sets" );

            if ( !uvcoord.isValid() )
                uvcoord = nif->getIndex( iBlock, "UV Sets 2" );

            if ( uvcoord.isValid() ) {
                FbxVector4 tc;
                QVector<Vector3> vec3 = nif->getArray<Vector3>( uvcoord );
                for(const Vector3& v3 : vec3)
                {
                    textureCoords.append(FbxVector2(v3[0], v3[1]));
                }

                if ( textureCoords.count() < verts.count() )
                    textureCoords.clear();
            }

            QVector<Triangle> ftriangles = nif->getArray<Triangle>( iBlock, "Triangles" );
            triangles.clear();
            int inv_idx = 0;

            for ( int i = 0; i < ftriangles.count(); i++ ) {
                Triangle t = ftriangles[i];
                inv_idx = 0;

                for ( int j = 0; j < 3; j++ ) {
                    if ( t[j] >= verts.count() ) {
                        inv_idx = 1;
                        break;
                    }
                }

                if ( !inv_idx )
                    triangles.append( t );
            }

            // Create UV for Diffuse channel
            FbxGeometryElementUV* lUVDiffuseElement = mesh->CreateElementUV("");
            FBX_ASSERT( lUVDiffuseElement != NULL);
            lUVDiffuseElement->SetMappingMode(FbxGeometryElement::eByControlPoint);
            lUVDiffuseElement->SetReferenceMode(FbxGeometryElement::eDirect);

            for(auto &tc : textureCoords)
            {
                lUVDiffuseElement->GetDirectArray().Add(tc);
            }

            // Create control points
            mesh->InitControlPoints(verts.count());
            FbxVector4* controlPoints = mesh->GetControlPoints();

            if(!nif->inherits( iBlock, "NiSkinCore" ))
            {
                for(int i = 0; i < verts.count(); i++)
                {
                    auto vUp = verts[i].toYUp();
                    controlPoints[i].Set(vUp.x(), vUp.y(), vUp.z());
                }
            }

            // Create polygons. Assign texture and texture UV indices.
            // all faces of the cube have the same texture
            for(int i = 0; i < triangles.count(); i++)
            {
                mesh->BeginPolygon(-1, -1, -1, false);

                // Control point indices
                mesh->AddPolygon(triangles[i].v1());
                mesh->AddPolygon(triangles[i].v2());
                mesh->AddPolygon(triangles[i].v3());

                mesh->EndPolygon ();
            }

            if(nif->inherits( iBlock, "NiSkinCore" ))
            {
                //Keep track of cluster linked to specific bone for the entire mesh
                std::map<uint, FbxCluster*> clusterMap;
                auto skinParent = ctx.NodeMap[nif->getBlockNumber( iNode )];

                if(!skinParent)
                {
                    qCCritical( nsIo ) << "Failed to find skin parent " << nif->getBlockNumber( iNode ) << "For skin" << nif->getBlockNumber( iBlock );
                    continue;
                }

                FbxSkin* meshSkin = FbxSkin::Create(pScene, "");

                QModelIndex idxSkinVertices = nif->getIndex( iBlock, "Skin Vertex Data" );
                if ( idxSkinVertices.isValid() ) {
                    for ( int vindex = 0; vindex < nif->rowCount( idxSkinVertices ) && vindex < verts.count(); vindex++ ) {
                        QModelIndex skinData = idxSkinVertices.child( vindex, 0 );
                        if(skinData.isValid())
                        {
                            auto instanceCount = nif->get<uint>( skinData, "Skin Vertex Count");
                            auto instanceArray = nif->getIndex( skinData, "data" );
                            for(int i = 0; i < instanceCount; i++)
                            {
                                QModelIndex skinInstance = instanceArray.child( i, 0 );
                                if(skinInstance.isValid())
                                {
                                    auto weight = nif->get<float>( skinInstance, "Weight");//, 7; precision(
                                    auto offset = nif->get<Vector3>( skinInstance, "Offset").toYUp();
                                    auto boneIdx = nif->getLink(skinInstance, "Bone");

                                    if(weight > 1.0f || weight < 0.0f)
                                        __debugbreak();

                                    FbxNode* boneNode = ctx.NodeMap[boneIdx];
                                    if(!boneNode)
                                    {
                                        qCCritical( nsIo ) << "Failed to find bone index " << boneIdx << "For mesh" << nif->getBlockNumber( iBlock );
                                        continue;
                                    }

                                    FbxCluster *boneCluster = clusterMap[boneIdx];
                                    if(!boneCluster)
                                    {
                                        boneCluster = FbxCluster::Create(pScene,"");
                                        boneCluster->SetLink(boneNode);
                                        boneCluster->SetLinkMode(FbxCluster::eTotalOne);
                                        boneCluster->SetTransformMatrix(meshNode->EvaluateGlobalTransform());
                                        boneCluster->SetTransformLinkMatrix(boneNode->EvaluateGlobalTransform());
                                        clusterMap[boneIdx] = boneCluster;

                                        AddNodeRecursively(ctx, boneNode);
                                        meshSkin->AddCluster(boneCluster);
                                    }
                                    boneCluster->AddControlPointIndex(vindex, weight);

                                    //Weights use offsets so we need to fix the transform
                                    FbxAMatrix trans = GetRelativeTransform(boneNode, parentnode);
                                    controlPoints[vindex] += trans.MultT(FbxVector4(offset.x(), offset.y(), offset.z())) * weight;
                                }
                            }
                        }
                    }
                }
                mesh->AddDeformer(meshSkin);


            }
            AddNodeRecursively(ctx, meshNode);
        }
    }
}

bool CreateScene(const NifModel * nif, FbxManager *pSdkManager, FbxScene* pScene, FBX_ExportContext &ctx)
{
    // create scene info
    FbxDocumentInfo* sceneInfo = FbxDocumentInfo::Create(pSdkManager,"SceneInfo");
    sceneInfo->mTitle = "Nif";
    sceneInfo->mSubject = "Export from NifScope";
    sceneInfo->mAuthor = "Twisted";
    sceneInfo->mRevision = "rev. 1.0";
    sceneInfo->mKeywords = "NIF";
    sceneInfo->mComment = "";

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

    ProcessNode(nif, iRoot, pScene, lSkeletonRoot, ctx);
    // Process meshes after navigating node tree to ensure bones are mapped
    ProcessMeshes(nif, iRoot, pScene, ctx);

    // Now create a bind pose with the link list
    if (ctx.NodeLinksArray.GetCount())
    {
        // A pose must be named
        FbxPose* bindPose = FbxPose::Create(pScene, "BindPose");

        // default pose type is rest pose, so we need to set the type as bind pose
        bindPose->SetIsBindPose(true);

        for (int i =0 ; i < ctx.NodeLinksArray.GetCount(); i++)
        {
            FbxNode*  lKFbxNode   = ctx.NodeLinksArray.GetAt(i);
            FbxMatrix lBindMatrix = lKFbxNode->EvaluateGlobalTransform();

            bindPose->Add(lKFbxNode, lBindMatrix);
        }

        // Add the pose to the scene
        pScene->AddPose(bindPose);
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

    FBX_ExportContext ctx;

    QString exportDir = QFileDialog::getExistingDirectory( qApp->activeWindow(), tr( "Choose a folder for export" ));

    if ( exportDir.isEmpty() )
        return;

    ctx.ExportPath = exportDir.toStdString();
    CreateDirectoryRecursive(exportDir.toStdString() + "/Textures");

    // Prepare the FBX SDK.
    InitializeSdkObjects(lSdkManager, lScene);

    // Create the scene.
    lResult = CreateScene(nif, lSdkManager, lScene, ctx);

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
