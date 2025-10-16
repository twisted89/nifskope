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

struct TEXTURE_INSTANCE {
    std::string texture;
    bool hasTransparency;
};

struct FBX_ExportContext {
    std::string ExportPath;
    std::map<uint, FbxNode*> NodeMap;
    std::map<uint, std::vector<TEXTURE_INSTANCE>> textureMap;
    std::map<uint, FbxSurfacePhong*> materialMap;
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

void ProcessNode(const NifModel * nif, const QModelIndex & iNode, FbxScene* scene, FbxAnimLayer* animLayer, FbxNode* parentnode, FBX_ExportContext &ctx)
{
    foreach ( const int l, nif->getChildLinks( nif->getBlockNumber( iNode )) ) {
        QModelIndex iBlock = nif->getBlock( l );
        auto t = Transform( nif, iBlock );
        auto blockName = nif->get<QString>( iBlock, "Name" ).toStdString();

        if(nif->isNiBlock( iBlock, "NiNode") || nif->inherits( iBlock, "NiNode" ))
        {
            FbxNode* node = node = FbxNode::Create(scene, blockName.c_str() );
            if(node) {

                ctx.NodeMap[nif->getBlockNumber( iBlock )] = node;

                FbxSkeleton* lSkeletonLimbNodeAttribute1 = FbxSkeleton::Create(scene, blockName.c_str());
                lSkeletonLimbNodeAttribute1->SetSkeletonType( FbxSkeleton::eLimb ); //HasChildBone(nif, iBlock) ?  FbxSkeleton::eLimb : FbxSkeleton::eEffector
                node->AddNodeAttribute(lSkeletonLimbNodeAttribute1);

                Eigen::Vector3d pos = t.translation.toYUp();
                Eigen::Vector3d rot = t.rotation.toEulerXYZ();

                node->LclTranslation.Set(FbxDouble3(pos.x(), pos.y(), pos.z()));
                node->LclRotation.Set(FbxDouble3(rot.x(), rot.y(), rot.z()));

                if ( nif->isNiBlock(iBlock ,"Ni3dsAnimationNode") || nif->isNiBlock( iBlock , "Ni3dsBone"))
                {
                    auto start = nif->get<float>( iBlock, "Start Time" );
                    //auto hiKeyTime = nif->get<float>( iBlock, "HiKeyTime" );
                    //auto lowKeyTime = nif->get<float>( iBlock, "LoKeyTime" );
                    //auto stop = hiKeyTime - lowKeyTime;
                    //auto phase = nif->get<float>( iBlock, "Phase" );
                    //auto frequency = nif->get<float>( iBlock, "Frequency" );

                    auto iTranslations = nif->getIndex( iBlock, "Translations" );
                    auto iRotations = nif->getIndex( iBlock, "Rotations" );
                    //auto iScales = nif->getIndex( iBlock, "Scales" );
                    //auto iVisibilities = nif->getIndex( iBlock, "Visibilities" );

                    QModelIndex tkeys = nif->getIndex( iTranslations, "Keys" );
                    auto translationKeyType = nif->get<uint>( iTranslations, "Interpolation" );
                    auto rotationKeyType = nif->get<uint>( iRotations, "Rotation Type" );

                    if(tkeys.isValid())
                    {
                        // Get the animation curves for local translation (create if needed)
                        FbxAnimCurve* lTranslationCurveX = node->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X, true);
                        FbxAnimCurve* lTranslationCurveY = node->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y, true);
                        FbxAnimCurve* lTranslationCurveZ = node->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z, true);

                        lTranslationCurveX->KeyModifyBegin();
                        lTranslationCurveY->KeyModifyBegin();
                        lTranslationCurveZ->KeyModifyBegin();

                        for ( int tindex = 0; tindex < nif->rowCount( tkeys ); tindex++ ) {
                            QModelIndex tkey = tkeys.child( tindex, 0 );
                            auto tval = nif->get<Vector3>( tkey, "Value" ).toYUp();
                            float currentTime = nif->get<float>( tkey, "Time" );
                            FbxTime fbxTime(currentTime * FBXSDK_TC_SECOND);

                            int xIndex = lTranslationCurveX->KeyAdd(fbxTime);
                            int yIndex = lTranslationCurveY->KeyAdd(fbxTime);
                            int zIndex = lTranslationCurveZ->KeyAdd(fbxTime);

                            lTranslationCurveX->KeySetValue(xIndex, tval.x());
                            lTranslationCurveY->KeySetValue(yIndex, tval.y());
                            lTranslationCurveZ->KeySetValue(zIndex, tval.z());

                            if(translationKeyType == 1) //Linear
                            {
                                lTranslationCurveX->KeySetInterpolation(xIndex, FbxAnimCurveDef::eInterpolationLinear);
                                lTranslationCurveY->KeySetInterpolation(yIndex, FbxAnimCurveDef::eInterpolationLinear);
                                lTranslationCurveZ->KeySetInterpolation(zIndex, FbxAnimCurveDef::eInterpolationLinear);
                            }
                            else //Bezier
                            {
                                auto outTan = nif->get<Vector3>( tkey, "OutTan" );
                                auto inTan = nif->get<Vector3>( tkey, "InTan" );

                                // Sample the Bézier curve between this key and the next
                                if (tindex + 1 < nif->rowCount(tkeys)) {
                                    QModelIndex nextKey = tkeys.child(tindex + 1, 0);
                                    float nextTime = nif->get<float>(nextKey, "Time");
                                    float deltaTime = nextTime - currentTime;

                                    // Read the precomputed A and B coefficients directly from the NIF
                                    Vector3 m_A = nif->get<Vector3>(tkey, "m_A");
                                    Vector3 m_B = nif->get<Vector3>(tkey, "m_B");
                                    Vector3 currentPos = nif->get<Vector3>(tkey, "Value");

                                    // Sample at 100ms intervals (0.1 seconds)
                                    const float sampleInterval = 0.1f;
                                    int numSamples = static_cast<int>(deltaTime / sampleInterval);

                                    // Always include the current keyframe with linear interpolation
                                    lTranslationCurveX->KeySetInterpolation(xIndex, FbxAnimCurveDef::eInterpolationCubic);
                                    lTranslationCurveY->KeySetInterpolation(yIndex, FbxAnimCurveDef::eInterpolationCubic);
                                    lTranslationCurveZ->KeySetInterpolation(zIndex, FbxAnimCurveDef::eInterpolationCubic);

                                    // Create intermediate samples using the Bézier interpolation formula
                                    for (int s = 1; s < numSamples; s++) {
                                        float sampleTime = currentTime + (s * sampleInterval);
                                        // Normalize time to [0, 1] range for this segment
                                        float t = (sampleTime - currentTime) / deltaTime;

                                        // Use NiBezPosKey::Interpolate formula:
                                        // P(t) = P0 + (OutTan + (A + B*t)*t)*t
                                        Vector3 interpPos = currentPos + (outTan + (m_A + m_B * t) * t) * t;

                                        // Convert to Y-up
                                        auto interpPosYUp = interpPos.toYUp();

                                        // Add the sampled keyframe
                                        FbxTime sampleFbxTime(sampleTime * FBXSDK_TC_SECOND);

                                        int sxIndex = lTranslationCurveX->KeyAdd(sampleFbxTime);
                                        int syIndex = lTranslationCurveY->KeyAdd(sampleFbxTime);
                                        int szIndex = lTranslationCurveZ->KeyAdd(sampleFbxTime);

                                        lTranslationCurveX->KeySetValue(sxIndex, interpPosYUp.x());
                                        lTranslationCurveY->KeySetValue(syIndex, interpPosYUp.y());
                                        lTranslationCurveZ->KeySetValue(szIndex, interpPosYUp.z());

                                        lTranslationCurveX->KeySetInterpolation(sxIndex, FbxAnimCurveDef::eInterpolationCubic);
                                        lTranslationCurveY->KeySetInterpolation(syIndex, FbxAnimCurveDef::eInterpolationCubic);
                                        lTranslationCurveZ->KeySetInterpolation(szIndex, FbxAnimCurveDef::eInterpolationCubic);
                                    }
                                } else {
                                    // Last keyframe - just use linear interpolation
                                    lTranslationCurveX->KeySetInterpolation(xIndex, FbxAnimCurveDef::eInterpolationCubic);
                                    lTranslationCurveY->KeySetInterpolation(yIndex, FbxAnimCurveDef::eInterpolationCubic);
                                    lTranslationCurveZ->KeySetInterpolation(zIndex, FbxAnimCurveDef::eInterpolationCubic);
                                }
                            }
                        }

                        lTranslationCurveX->KeyModifyEnd();
                        lTranslationCurveY->KeyModifyEnd();
                        lTranslationCurveZ->KeyModifyEnd();
                    }


                    QModelIndex rkeys = nif->getIndex( iRotations, "Quaternion Keys" );
                    if(rkeys.isValid())
                    {
                        // Get the animation curves for local rotation (create if needed)
                        FbxAnimCurve* lRotationCurveX = node->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X, true);
                        FbxAnimCurve* lRotationCurveY = node->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y, true);
                        FbxAnimCurve* lRotationCurveZ = node->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z, true);

                        lRotationCurveX->KeyModifyBegin();
                        lRotationCurveY->KeyModifyBegin();
                        lRotationCurveZ->KeyModifyBegin();

                        // Track previous quaternion AND euler for continuity
                        Quat prevQuat;
                        bool hasPrevQuat = false;
                        Eigen::Vector3d prevEuler(0, 0, 0);
                        bool hasPrevEuler = false;

                        // Helper lambda to unwrap Euler angles
                        auto unwrapEuler = [](const Eigen::Vector3d& prev, Eigen::Vector3d& current) {
                            // Unwrap each component to be continuous with previous
                            for (int i = 0; i < 3; i++) {
                                double diff = current[i] - prev[i];
                                // If difference is > 180 degrees, we crossed a boundary
                                if (diff > 180.0) {
                                    current[i] -= 360.0;
                                } else if (diff < -180.0) {
                                    current[i] += 360.0;
                                }
                            }
                        };

                        for ( int rindex = 0; rindex < nif->rowCount( rkeys ); rindex++ ) {
                            QModelIndex rkey = rkeys.child( rindex, 0 );
                            Quat rot = nif->get<Quat>( rkey, "Value" );
                            float keyTime = nif->get<float>( rkey, "Time" );

                            // Ensure quaternion continuity - flip if needed to take shortest path
                            if (hasPrevQuat) {
                                float dot = Quat::dotproduct(prevQuat, rot);
                                if (dot < 0.0f) {
                                    // Flip quaternion to ensure shortest rotation path
                                    rot[0] = -rot[0];
                                    rot[1] = -rot[1];
                                    rot[2] = -rot[2];
                                    rot[3] = -rot[3];
                                }
                            }
                            prevQuat = rot;
                            hasPrevQuat = true;

                            FbxTime fbxTime(keyTime * FBXSDK_TC_SECOND);

                            // Convert quaternion to matrix, then use the same Z-up to Y-up conversion as toEulerXYZ
                            Matrix rotMatrix;
                            rotMatrix.fromQuat(rot);

                            // Use the toEulerXYZ function which handles coordinate system conversion
                            Eigen::Vector3d euler = rotMatrix.toEulerXYZ();

                            // Unwrap Euler angles to ensure continuity
                            if (hasPrevEuler) {
                                unwrapEuler(prevEuler, euler);
                            }
                            prevEuler = euler;
                            hasPrevEuler = true;

                            if(rotationKeyType == 1) //Linear
                            {
                                int xIndex = lRotationCurveX->KeyAdd(fbxTime);
                                int yIndex = lRotationCurveY->KeyAdd(fbxTime);
                                int zIndex = lRotationCurveZ->KeyAdd(fbxTime);

                                lRotationCurveX->KeySetValue(xIndex, euler.x());
                                lRotationCurveY->KeySetValue(yIndex, euler.y());
                                lRotationCurveZ->KeySetValue(zIndex, euler.z());

                                lRotationCurveX->KeySetInterpolation(xIndex, FbxAnimCurveDef::eInterpolationLinear);
                                lRotationCurveY->KeySetInterpolation(yIndex, FbxAnimCurveDef::eInterpolationLinear);
                                lRotationCurveZ->KeySetInterpolation(zIndex, FbxAnimCurveDef::eInterpolationLinear);
                            }
                            else if(rotationKeyType == 3) //TCB
                            {
                                // TCB uses Squad interpolation which FBX doesn't support directly
                                // We need to sample the Squad curve and create intermediate keys

                                // Add the main keyframe
                                int xIndex = lRotationCurveX->KeyAdd(fbxTime);
                                int yIndex = lRotationCurveY->KeyAdd(fbxTime);
                                int zIndex = lRotationCurveZ->KeyAdd(fbxTime);

                                lRotationCurveX->KeySetValue(xIndex, euler.x());
                                lRotationCurveY->KeySetValue(yIndex, euler.y());
                                lRotationCurveZ->KeySetValue(zIndex, euler.z());

                                lRotationCurveX->KeySetInterpolation(xIndex, FbxAnimCurveDef::eInterpolationLinear);
                                lRotationCurveY->KeySetInterpolation(yIndex, FbxAnimCurveDef::eInterpolationLinear);
                                lRotationCurveZ->KeySetInterpolation(zIndex, FbxAnimCurveDef::eInterpolationLinear);

                                // Sample the Squad curve between this key and the next
                                if (rindex + 1 < nif->rowCount( rkeys )) {
                                    QModelIndex nextKey = rkeys.child( rindex + 1, 0 );
                                    Quat nextRot = nif->get<Quat>( nextKey, "Value" );
                                    Quat m_A = nif->get<Quat>( rkey, "A" );
                                    Quat m_B = nif->get<Quat>( nextKey, "B" );

                                    // Ensure continuity for control quaternions
                                    if (Quat::dotproduct(rot, m_A) < 0.0f) {
                                        m_A[0] = -m_A[0];
                                        m_A[1] = -m_A[1];
                                        m_A[2] = -m_A[2];
                                        m_A[3] = -m_A[3];
                                    }
                                    if (Quat::dotproduct(rot, nextRot) < 0.0f) {
                                        nextRot[0] = -nextRot[0];
                                        nextRot[1] = -nextRot[1];
                                        nextRot[2] = -nextRot[2];
                                        nextRot[3] = -nextRot[3];
                                    }
                                    if (Quat::dotproduct(nextRot, m_B) < 0.0f) {
                                        m_B[0] = -m_B[0];
                                        m_B[1] = -m_B[1];
                                        m_B[2] = -m_B[2];
                                        m_B[3] = -m_B[3];
                                    }

                                    float nextTime = nif->get<float>( nextKey, "Time" );
                                    float deltaTime = nextTime - keyTime;

                                    // Sample Squad at intermediate points
                                    const int numSamples = 4;
                                    Quat prevSampleQuat = rot;

                                    for (int s = 1; s < numSamples; s++) {
                                        float t = static_cast<float>(s) / numSamples;
                                        float sampleTime = keyTime + (deltaTime * t);

                                        // Use Squad interpolation: Squad(t, p, a, b, q)
                                        // This matches NiTCBRotKey::Interpolate implementation
                                        Quat interpQuat = Quat::slerp(2.0f * t * (1.0f - t),
                                                                      Quat::slerp(t, rot, nextRot),
                                                                      Quat::slerp(t, m_A, m_B));

                                        // Ensure continuity between samples
                                        if (Quat::dotproduct(prevSampleQuat, interpQuat) < 0.0f) {
                                            interpQuat[0] = -interpQuat[0];
                                            interpQuat[1] = -interpQuat[1];
                                            interpQuat[2] = -interpQuat[2];
                                            interpQuat[3] = -interpQuat[3];
                                        }
                                        prevSampleQuat = interpQuat;

                                        // Convert interpolated quaternion to Euler
                                        Matrix interpMatrix;
                                        interpMatrix.fromQuat(interpQuat);
                                        Eigen::Vector3d interpEuler = interpMatrix.toEulerXYZ();

                                        // Unwrap Euler angles for this sample
                                        unwrapEuler(prevEuler, interpEuler);
                                        prevEuler = interpEuler;

                                        // Add intermediate key
                                        FbxTime sampleFbxTime(sampleTime * FBXSDK_TC_SECOND);

                                        int sxIndex = lRotationCurveX->KeyAdd(sampleFbxTime);
                                        int syIndex = lRotationCurveY->KeyAdd(sampleFbxTime);
                                        int szIndex = lRotationCurveZ->KeyAdd(sampleFbxTime);

                                        lRotationCurveX->KeySetValue(sxIndex, interpEuler.x());
                                        lRotationCurveY->KeySetValue(syIndex, interpEuler.y());
                                        lRotationCurveZ->KeySetValue(szIndex, interpEuler.z());

                                        lRotationCurveX->KeySetInterpolation(sxIndex, FbxAnimCurveDef::eInterpolationLinear);
                                        lRotationCurveY->KeySetInterpolation(syIndex, FbxAnimCurveDef::eInterpolationLinear);
                                        lRotationCurveZ->KeySetInterpolation(szIndex, FbxAnimCurveDef::eInterpolationLinear);
                                    }
                                }
                            }
                            else //Bezier or other cubic types
                            {
                                int xIndex = lRotationCurveX->KeyAdd(fbxTime);
                                int yIndex = lRotationCurveY->KeyAdd(fbxTime);
                                int zIndex = lRotationCurveZ->KeyAdd(fbxTime);

                                lRotationCurveX->KeySetValue(xIndex, euler.x());
                                lRotationCurveY->KeySetValue(yIndex, euler.y());
                                lRotationCurveZ->KeySetValue(zIndex, euler.z());

                                lRotationCurveX->KeySetInterpolation(xIndex, FbxAnimCurveDef::eInterpolationCubic);
                                lRotationCurveY->KeySetInterpolation(yIndex, FbxAnimCurveDef::eInterpolationCubic);
                                lRotationCurveZ->KeySetInterpolation(zIndex, FbxAnimCurveDef::eInterpolationCubic);
                            }
                        }

                        lRotationCurveX->KeyModifyEnd();
                        lRotationCurveY->KeyModifyEnd();
                        lRotationCurveZ->KeyModifyEnd();
                    }
                }

                parentnode->AddChild(node);

                ProcessNode(nif, iBlock, scene, animLayer, node, ctx);
            }
            else
            {
                qCCritical( nsIo ) << "Failed to create node with block ID" << nif->getBlockNumber( iBlock );
            }
        }
        else if(nif->inherits( iBlock, "NiSkinCore" ) || nif->itemName( iBlock ) == "NiTriShape"
                 || nif->inherits( iBlock, "NiTriShape" ))
        {
            for ( const auto pl : nif->getLinkArray( iBlock, "Properties" ) ) {

                QModelIndex ipBlock = nif->getBlock( pl );

                if(nif->isNiBlock( ipBlock, "NiMaterialProperty"))
                {
                    float alpha = nif->get<float>( ipBlock, "Alpha" );

                    if ( alpha < 0.0 )
                        alpha = 0.0;

                    if ( alpha > 1.0 )
                        alpha = 1.0;

                    auto ambient  = nif->get<Color3>( ipBlock, "Ambient Color" );
                    auto diffuse  = nif->get<Color3>( ipBlock, "Diffuse Color" );
                    auto specular = nif->get<Color3>( ipBlock, "Specular Color" );
                    auto emissive = nif->get<Color3>( ipBlock, "Emissive Color" );
                    auto shininess = nif->get<float>( ipBlock, "Glossiness" );

                    FbxSurfacePhong* lMaterial = FbxSurfacePhong::Create(scene, "");

                    // Generate primary and secondary colors.
                    lMaterial->Emissive           .Set(FbxDouble3(emissive.red(), emissive.green(), emissive.blue()));
                    lMaterial->Ambient            .Set(FbxDouble3(ambient.red(), ambient.green(), ambient.blue()));
                    lMaterial->AmbientFactor      .Set(1.);
                    // Add texture for diffuse channel
                    lMaterial->Diffuse           .Set(FbxDouble3(diffuse.red(), diffuse.green(), diffuse.blue()));
                    lMaterial->DiffuseFactor     .Set(1.);
                    lMaterial->TransparencyFactor.Set(1.0 - alpha); // Invert: FBX uses opacity not transparency
                    lMaterial->ShadingModel      .Set("Phong");
                    lMaterial->Shininess         .Set(shininess);
                    lMaterial->Specular          .Set(FbxDouble3(specular.red(), specular.green(), specular.blue()));
                    lMaterial->SpecularFactor    .Set(0.0);

                    ctx.materialMap[nif->getBlockNumber( iBlock )] = lMaterial;
                }

                else if(nif->isNiBlock( ipBlock, "NiTextureProperty"))
                {
                    foreach ( const int cl, nif->getChildLinks( nif->getBlockNumber( ipBlock )) ) {
                        QModelIndex ciBlock = nif->getBlock( cl );
                        if(nif->isNiBlock( ciBlock, "NiImage"))
                        {
                            std::string textureName = std::to_string(nif->getBlockNumber(ciBlock));
                            QModelIndex iImage = nif->getBlock( nif->getLink( ciBlock, "Image Data" ));
                            if(nif->getBlockName(iImage) == "NiRawImageData")
                            {
                                auto width  = nif->get<uint>( iImage, "Width" );
                                auto height = nif->get<uint>( iImage, "Height" );
                                auto type = nif->get<int>( iImage, "Image Type" );

                                QModelIndex iPixelData;
                                int components;
                                switch(type)
                                {
                                case 1: //RGB
                                    components = 3;
                                    iPixelData = nif->getIndex( iImage, "RGB Image Data" );
                                    break;
                                case 2: // RGBA
                                    components = 4;
                                    iPixelData = nif->getIndex( iImage, "RGBA Image Data" );
                                    break;
                                default:
                                    qCCritical( nsIo ) << "Unsupported image type" << type << "for block ID" << nif->getBlockNumber( iImage );
                                    continue;
                                }

                                if(!std::filesystem::exists(ctx.ExportPath
                                                             + "/Textures/"
                                                             + textureName
                                                             + ".png"))
                                {
                                    if ( iPixelData.isValid() ) {
                                        if ( QByteArray * pdata = nif->get<QByteArray *>( iPixelData.child(0, 0) ) ) {
                                            const std::string filename = ctx.ExportPath
                                                                         + "/Textures/"
                                                                         + textureName
                                                                         + ".png";

                                            // Save the image
                                            stbi_write_png(filename.c_str(), width, height, components, pdata->data(), width * components);
                                        }
                                    }
                                }
                                ctx.textureMap[nif->getBlockNumber( iBlock )].push_back(TEXTURE_INSTANCE {textureName, components == 4 });
                                break;
                            }
                        }
                    }
                }

            }
        }
        else if(nif->isNiBlock( iBlock, "NiCamera"))
        {
            FbxCamera* lCamera = FbxCamera::Create(scene, "GameCamera");
            lCamera->ProjectionType.Set(FbxCamera::ePerspective);

            auto nearPlane = nif->get<float>( iBlock, "Frustum Near");
            auto farPlane = nif->get<float>( iBlock, "Frustum Far");

            lCamera->NearPlane.Set(nearPlane);
            lCamera->FarPlane.Set(100000.0);

            parentnode->LclRotation.Set(FbxDouble3(-90, 0, -90));

            parentnode->SetNodeAttribute(lCamera);
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

            // We need to move any skinned meshes into the root space otherwise they'll pivot around their parent origins
            if(nif->inherits( iBlock, "NiSkinCore" ))
            {
                meshNode->LclTranslation.Set(parentnode->EvaluateGlobalTransform().GetT());
                meshNode->LclRotation.Set(parentnode->EvaluateGlobalTransform().GetR());
                pScene->GetRootNode()->AddChild(meshNode);
            }
            else {
                meshNode->LclTranslation.Set(FbxVector4(meshTranslation.x(), meshTranslation.y(), meshTranslation.z()));
                parentnode->AddChild(meshNode);
            }
            meshNode->SetNodeAttribute(mesh);
            meshNode->SetShadingMode(FbxNode::eTextureShading);

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
                    textureCoords.append(FbxVector2(v3[0], 1.0 - v3[1]));
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
                            for(unsigned int i = 0; i < instanceCount; i++)
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

            //Apply texture
            auto textures = ctx.textureMap[nif->getBlockNumber( iBlock )];
            if(textures.size())
            {
                auto textureInfo = textures.front();
                std::string textureFilename = ctx.ExportPath + "/Textures/" + textureInfo.texture + ".png";
                FbxSurfacePhong* lMaterial = ctx.materialMap[nif->getBlockNumber( iBlock )];
                lMaterial->TransparencyFactor.Set(0.0); // Start with fully opaque
                if (lMaterial)
                {
                    FbxLayer* lLayer = mesh->GetLayer(0);

                    if (!lLayer)
                    {
                        mesh->CreateLayer();
                        lLayer = mesh->GetLayer(0);
                    }

                    // Create a layer element material to handle proper mapping.
                    FbxLayerElementMaterial* lLayerElementMaterial = FbxLayerElementMaterial::Create(mesh, "");

                    // This allows us to control where the materials are mapped.  Using eAllSame
                    // means that all faces/polygons of the mesh will be assigned the same material.
                    lLayerElementMaterial->SetMappingMode(FbxLayerElement::eAllSame);
                    lLayerElementMaterial->SetReferenceMode(FbxLayerElement::eIndexToDirect);
                    // Add an index to the lLayerElementMaterial.  Since we have only one, and are using eAllSame mapping mode,
                    // we only need to add one.
                    lLayerElementMaterial->GetIndexArray().Add(0);

                    // Save the material on the layer
                    lLayer->SetMaterials(lLayerElementMaterial);

                    FbxFileTexture* lTexture = FbxFileTexture::Create(pScene, textureInfo.texture.c_str());

                    // Set texture properties.
                    lTexture->SetFileName(textureFilename.c_str()); // Resource file is in current directory.
                    lTexture->SetTextureUse(FbxTexture::eStandard);
                    lTexture->SetMappingType(FbxTexture::eUV);
                    lTexture->SetSwapUV(false);
                    lTexture->SetMaterialUse(FbxFileTexture::eModelMaterial);
                    lTexture->SetTranslation(0.0, 0.0);
                    lTexture->SetScale(1.0, 1.0);
                    lTexture->SetRotation(0.0, 0.0);
                    // lTexture->UVSet.Set(FbxString(gDiffuseElementName)); // Connect texture to the proper UV

                    // Connect the texture to the corresponding property of the material
                    lMaterial->Diffuse.ConnectSrcObject(lTexture);

                    FbxFileTexture* tTexture = nullptr;
                    // If the texture has an alpha channel, set up proper alpha blending
                    if(textureInfo.hasTransparency)
                    {
                        // Don't create a separate alpha texture - instead, let the diffuse texture handle transparency
                        // Remove the SetAlphaSource call on diffuse texture to let it use RGB channels normally

                        // For Maya, we need to connect the same texture file to the transparency channel
                        // but configure it to read the alpha channel
                        tTexture = FbxFileTexture::Create(pScene, (textureInfo.texture + "_opacity").c_str());
                        tTexture->SetFileName(textureFilename.c_str());
                        tTexture->SetTextureUse(FbxTexture::eStandard);
                        tTexture->SetMappingType(FbxTexture::eUV);
                        tTexture->SetSwapUV(false);
                        tTexture->SetMaterialUse(FbxFileTexture::eModelMaterial);

                        // This is the key: use eRGBIntensity to read the alpha channel from the PNG
                        // Don't use eBlack - that treats black pixels as transparent
                        tTexture->SetAlphaSource(FbxTexture::EAlphaSource::eRGBIntensity);

                        tTexture->SetTranslation(0.0, 0.0);
                        tTexture->SetScale(1.0, 1.0);
                        tTexture->SetRotation(0.0, 0.0);

                        // Connect to TransparentColor for Maya compatibility
                        lMaterial->TransparentColor.ConnectSrcObject(tTexture);

                        // Set base transparency to maximum so texture controls it
                        lMaterial->TransparencyFactor.Set(1.0);
                    }
                    else
                    {
                        // No alpha channel - keep fully opaque
                    }


                    FbxLayerElementTexture* lTextureElement = FbxLayerElementTexture::Create(mesh, "Diffuse Texture");
                    lTextureElement->SetMappingMode(FbxLayerElement::eByControlPoint); // Use control point mapping for direct access.
                    lTextureElement->SetReferenceMode(FbxLayerElement::eDirect); // Direct reference to the textures.
                    lTextureElement->GetDirectArray().Add(lTexture);
                    if(tTexture)
                        lTextureElement->GetDirectArray().Add(tTexture);
                    lLayer->SetTextures(FbxLayerElement::EType::eTextureDiffuse, lTextureElement);

                    // Create and configure the UV element for direct index access.
                    FbxLayerElementUV* lUVElement = FbxLayerElementUV::Create(mesh, "DiffuseUV");
                    lUVElement->SetMappingMode(FbxLayerElement::eByControlPoint); // Mapping for direct UV access.
                    lUVElement->SetReferenceMode(FbxLayerElement::eDirect); // Reference mode for direct UV values.

                    for(auto &tc : textureCoords)
                    {
                        lUVElement->GetDirectArray().Add(tc);
                    }

                    lLayer->SetUVs(lUVElement, FbxLayerElement::EType::eTextureDiffuse);

                    meshNode->AddMaterial(lMaterial);
                }
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

    // Create an animation stack
    FbxAnimStack* lAnimStack = FbxAnimStack::Create(pScene, "AnimationStack");

    // Create an animation layer for the stack
    FbxAnimLayer* lAnimLayer = FbxAnimLayer::Create(pScene, "Base Layer");
    lAnimStack->AddMember(lAnimLayer);

    FbxTimeSpan ts;
    ts.Set(FbxTime(0), 120 * FBXSDK_TC_SECOND) ;
    lAnimStack->SetLocalTimeSpan(ts);

    pScene->SetCurrentAnimationStack(lAnimStack);

    ProcessNode(nif, iRoot, pScene, lAnimLayer, lSkeletonRoot, ctx);
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
