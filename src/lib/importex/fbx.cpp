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
#include "exportcommon.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define tr( x ) QApplication::tr( x )

struct TEXTURE_INSTANCE {
    std::string texture;
    bool hasTransparency;
    Color3 emissiveColor;
};

struct TEXTURE_FLIPPER {
    float startTime, rate;
    std::string textureNames;
};

struct FBX_ExportContext {
    std::string ExportPath;
    std::map<uint, FbxNode*> NodeMap;
    std::list<FbxNode*> billboardNodes;
    std::list<FbxCamera*> cameras;
    std::map<uint, std::vector<TEXTURE_INSTANCE>> textureMap;
    std::map<uint, FbxSurfaceLambert*> materialMap;
    std::map<uint, TEXTURE_FLIPPER> textureFlipMap;
    FbxArray<FbxNode*> NodeLinksArray;
};


bool CreateTransparencyMap(const std::string& sourceTexturePath, const std::string& outputPath, int width, int height, const unsigned char* rgbaData)
{
    // Create a grayscale alpha map - INVERTED for Maya's TransparentColor
    std::vector<unsigned char> alphaData(width * height);
    
    for (int i = 0; i < width * height; i++)
    {
        // INVERT: In TransparentColor channel, BLACK = opaque, WHITE = transparent
        // Original alpha: 255 = opaque, 0 = transparent
        // Inverted: 0 = opaque, 255 = transparent
        alphaData[i] = 255 - rgbaData[i * 4 + 3];
    }
    
    // Save as grayscale PNG
    return stbi_write_png(outputPath.c_str(), width, height, 1, alphaData.data(), width) != 0;
}

bool CreateEmissiveMap(const std::string& sourceTexturePath, const std::string& outputPath, int width, int height, const unsigned char* rgbaData, const Color3& emissiveColor)
{
    // Create an emissive texture by multiplying the diffuse RGB by the emissive color
    // Only apply emissive to opaque areas (based on alpha channel)
    std::vector<unsigned char> emissiveData(width * height * 3);
    
    for (int i = 0; i < width * height; i++)
    {
        unsigned char alpha = rgbaData[i * 4 + 3];
        
        // Apply emissive color only to opaque pixels (alpha > 127)
        // This prevents emissive glow on transparent areas
        float emissiveFactor = (alpha > 127) ? 1.0f : 0.0f;
        
        // Multiply texture RGB by emissive color
        emissiveData[i * 3 + 0] = static_cast<unsigned char>(rgbaData[i * 4 + 0] * emissiveColor.red() * emissiveFactor);
        emissiveData[i * 3 + 1] = static_cast<unsigned char>(rgbaData[i * 4 + 1] * emissiveColor.green() * emissiveFactor);
        emissiveData[i * 3 + 2] = static_cast<unsigned char>(rgbaData[i * 4 + 2] * emissiveColor.blue() * emissiveFactor);
    }
    
    // Save as RGB PNG
    return stbi_write_png(outputPath.c_str(), width, height, 3, emissiveData.data(), width * 3) != 0;
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

void ProcessTextureFlips(const NifModel * nif, const QModelIndex & iNode, FbxScene* scene, FBX_ExportContext &ctx)
{
    foreach ( const int l, nif->getChildLinks( nif->getBlockNumber( iNode )) ) {
        QModelIndex iBlock = nif->getBlock( l );

        if(nif->isNiBlock( iBlock, "NiFlipTextures"))
        {
            auto texturePropertyId = nif->getLink( iBlock, "Texture Property" );
            auto textureProperty = nif->getBlock( texturePropertyId );
            if(textureProperty.isValid())
            {
                TEXTURE_FLIPPER flipperCtx;
                flipperCtx.startTime = nif->get<float>( iBlock, "Start Time" );
                flipperCtx.rate = nif->get<float>( iBlock, "Rate 2" );

                foreach ( const int cl, nif->getChildLinks(texturePropertyId) ) {
                    QModelIndex ciBlock = nif->getBlock( cl );
                    if(nif->isNiBlock( ciBlock, "NiImage"))
                    {
                        flipperCtx.textureNames.append(std::to_string(nif->getBlockNumber(ciBlock)) + ";");
                    }
                }

                ctx.textureFlipMap[texturePropertyId] = flipperCtx;
            }
        }
    }
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
                
                // only apply billboards to nodes which aren't animated
                if(nif->isNiBlock( iBlock, "NiBillboardNode") && !nif->isNiBlock(iNode ,"Ni3dsAnimationNode") )
                {
                    ctx.billboardNodes.push_back(node);
                }
                
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
                    auto iVisibilities = nif->getIndex( iBlock, "Visibilities" );
                    
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
                                //auto inTan = nif->get<Vector3>( tkey, "InTan" );
                                
                                // Sample the B?zier curve between this key and the next
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
                                    
                                    // Create intermediate samples using the B?zier interpolation formula
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
                        
                        Eigen::Vector3d prevEuler(0, 0, 0);
                        bool hasPrevEuler = false;
                        Quat prevRot; 
                        bool hasPrevRot = false; 
                        
                        // Helper lambda to unwrap Euler angles
                        auto unwrapEuler = [](const Eigen::Vector3d& prev, Eigen::Vector3d& current) {
                            // First, try standard unwrapping for each component
                            Eigen::Vector3d unwrapped = current;
                            for (int i = 0; i < 3; i++) {
                                double diff = unwrapped[i] - prev[i];
                                // Unwrap to closest representation
                                while (diff > 180.0) {
                                    unwrapped[i] -= 360.0;
                                    diff -= 360.0;
                                }
                                while (diff < -180.0) {
                                    unwrapped[i] += 360.0;
                                    diff += 360.0;
                                }
                            }

                            // Check alternative Euler representations due to gimbal lock
                            Eigen::Vector3d alternative;
                            alternative[0] = unwrapped[0] + 180.0;
                            alternative[1] = 180.0 - unwrapped[1];
                            alternative[2] = unwrapped[2] + 180.0;

                            // Normalize the alternative to [-180, 180] range
                            for (int i = 0; i < 3; i++) {
                                while (alternative[i] > 180.0) alternative[i] -= 360.0;
                                while (alternative[i] < -180.0) alternative[i] += 360.0;
                            }

                            // Calculate distances
                            double distStandard = (unwrapped - prev).squaredNorm();
                            double distAlternative = (alternative - prev).squaredNorm();

                            // Use whichever representation is closer to the previous frame
                            current = (distAlternative < distStandard) ? alternative : unwrapped;
                        };
                        
                        for ( int rindex = 0; rindex < nif->rowCount( rkeys ); rindex++ ) {
                            QModelIndex rkey = rkeys.child( rindex, 0 );
                            Quat rot = nif->get<Quat>( rkey, "Value" );
                            //float angle = nif->get<Quat>( rkey, "Angle" );
                            //Vector3 axis = nif->get<Vector3>( rkey, "Axis" );
                            float keyTime = nif->get<float>( rkey, "Time" );
                            
                            FbxTime fbxTime(keyTime * FBXSDK_TC_SECOND);
                            
                            if (hasPrevRot && Quat::dotproduct(prevRot, rot) < 0.0f) {
                                rot[0] = -rot[0];
                                rot[1] = -rot[1];
                                rot[2] = -rot[2];
                                rot[3] = -rot[3];
                            }
                            prevRot = rot;
                            hasPrevRot = true;
                            
                            Matrix rotMatrix;
                            rotMatrix.fromQuat(rot);
                            
                            Eigen::Vector3d euler = rotMatrix.toEulerXYZ();
                            
                            // Unwrap Euler angles to ensure continuity
                            if (hasPrevEuler) {
                                unwrapEuler(prevEuler, euler);
                            }
                            prevEuler = euler;
                            hasPrevEuler = true;
                            
                            if(rotationKeyType == 1) //Linear
                            {
                                // Unwrap Euler angles to ensure continuity
                                if (hasPrevEuler) {
                                    unwrapEuler(prevEuler, euler);
                                }
                                prevEuler = euler;
                                hasPrevEuler = true;
                                
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
                                // Helper function to convert quaternion to Euler angles with continuity
                                auto quatToEulerContinuous = [](const Quat& q, const Eigen::Vector3d& prevEuler) -> Eigen::Vector3d {
                                    // Convert quaternion to rotation matrix
                                    Matrix m;
                                    m.fromQuat(q);

                                    // Convert matrix to Euler angles (XYZ order) with proper Y-up coordinate transform
                                    Eigen::Vector3d euler = m.toEulerXYZ();

                                    // Generate all equivalent Euler angle representations
                                    // (accounting for the 2π periodicity and gimbal lock alternatives)
                                    std::vector<Eigen::Vector3d> candidates;
                                    candidates.reserve(12);

                                    // Standard representation
                                    candidates.push_back(euler);

                                    // Gimbal lock alternatives (when pitch is near ±90°)
                                    candidates.push_back(Eigen::Vector3d(euler[0] + 180.0, 180.0 - euler[1], euler[2] + 180.0));
                                    candidates.push_back(Eigen::Vector3d(euler[0] - 180.0, 180.0 - euler[1], euler[2] + 180.0));
                                    candidates.push_back(Eigen::Vector3d(euler[0] + 180.0, 180.0 - euler[1], euler[2] - 180.0));
                                    candidates.push_back(Eigen::Vector3d(euler[0] - 180.0, 180.0 - euler[1], euler[2] - 180.0));

                                    // Wrap-around alternatives (±360° for each axis)
                                    for (int i = 0; i < 3; i++) {
                                        Eigen::Vector3d plusWrap = euler;
                                        Eigen::Vector3d minusWrap = euler;
                                        plusWrap[i] += 360.0;
                                        minusWrap[i] -= 360.0;
                                        candidates.push_back(plusWrap);
                                        candidates.push_back(minusWrap);
                                    }

                                    // Find the candidate with minimum distance to previous Euler angles
                                    double minDist = std::numeric_limits<double>::max();
                                    Eigen::Vector3d bestEuler = euler;

                                    for (auto& candidate : candidates) {
                                        // Normalize to [-180, 180] range
                                        for (int i = 0; i < 3; i++) {
                                            while (candidate[i] > 180.0) candidate[i] -= 360.0;
                                            while (candidate[i] < -180.0) candidate[i] += 360.0;
                                        }

                                        // Calculate weighted distance (prioritize pitch/Y stability to minimize gimbal lock artifacts)
                                        Eigen::Vector3d diff = candidate - prevEuler;
                                        double dist = diff[0] * diff[0] + diff[1] * diff[1] * 2.0 + diff[2] * diff[2];

                                        if (dist < minDist) {
                                            minDist = dist;
                                            bestEuler = candidate;
                                        }
                                    }

                                    return bestEuler;
                                };

                                // Convert current keyframe quaternion to Euler angles
                                Eigen::Vector3d euler = quatToEulerContinuous(rot, prevEuler);
                                prevEuler = euler;

                                // Add the main keyframe
                                int xIndex = lRotationCurveX->KeyAdd(fbxTime);
                                int yIndex = lRotationCurveY->KeyAdd(fbxTime);
                                int zIndex = lRotationCurveZ->KeyAdd(fbxTime);

                                lRotationCurveX->KeySetValue(xIndex, euler.x());
                                lRotationCurveY->KeySetValue(yIndex, euler.y());
                                lRotationCurveZ->KeySetValue(zIndex, euler.z());

                                // Use constant interpolation to prevent Unity from auto-smoothing between samples
                                lRotationCurveX->KeySetInterpolation(xIndex, FbxAnimCurveDef::eInterpolationConstant);
                                lRotationCurveY->KeySetInterpolation(yIndex, FbxAnimCurveDef::eInterpolationConstant);
                                lRotationCurveZ->KeySetInterpolation(zIndex, FbxAnimCurveDef::eInterpolationConstant);

                                // Sample the Squad curve between this key and the next
                                if (rindex + 1 < nif->rowCount(rkeys)) {
                                    QModelIndex nextKey = rkeys.child(rindex + 1, 0);
                                    Quat nextRot = nif->get<Quat>(nextKey, "Value");
                                    Quat m_A = nif->get<Quat>(rkey, "A");
                                    Quat m_B = nif->get<Quat>(nextKey, "B");
                                    float nextTime = nif->get<float>(nextKey, "Time");
                                    float deltaTime = nextTime - keyTime;

                                    // Ensure quaternion continuity (shortest path interpolation)
                                    if (Quat::dotproduct(rot, m_A) < 0.0f) {
                                        m_A[0] = -m_A[0]; m_A[1] = -m_A[1]; m_A[2] = -m_A[2]; m_A[3] = -m_A[3];
                                    }
                                    if (Quat::dotproduct(rot, nextRot) < 0.0f) {
                                        nextRot[0] = -nextRot[0]; nextRot[1] = -nextRot[1];
                                        nextRot[2] = -nextRot[2]; nextRot[3] = -nextRot[3];
                                    }
                                    if (Quat::dotproduct(nextRot, m_B) < 0.0f) {
                                        m_B[0] = -m_B[0]; m_B[1] = -m_B[1]; m_B[2] = -m_B[2]; m_B[3] = -m_B[3];
                                    }

                                    // Sample at 30fps (33.333ms per frame)
                                    const float sampleInterval = 1.0f / 30.0f; // ~0.033 seconds
                                    int numSamples = std::max(1, static_cast<int>(std::ceil(deltaTime / sampleInterval)));

                                    Quat prevSampleQuat = rot;

                                    // Generate intermediate samples using Squad (Spherical Cubic) interpolation
                                    for (int s = 1; s < numSamples; s++) {
                                        // Normalized time parameter [0, 1] for this segment
                                        float t = static_cast<float>(s) / static_cast<float>(numSamples);

                                        // Don't oversample past the next keyframe
                                        if (t >= 1.0f) break;

                                        float sampleTime = keyTime + (t * deltaTime);

                                        // Squad interpolation formula: Squad(t, p, a, b, q) = Slerp(2t(1-t), Slerp(t, p, q), Slerp(t, a, b))
                                        Quat interpQuat = Quat::slerp(2.0f * t * (1.0f - t),
                                                                      Quat::slerp(t, rot, nextRot),
                                                                      Quat::slerp(t, m_A, m_B));

                                        // Ensure shortest path for consecutive samples
                                        if (Quat::dotproduct(prevSampleQuat, interpQuat) < 0.0f) {
                                            interpQuat[0] = -interpQuat[0]; interpQuat[1] = -interpQuat[1];
                                            interpQuat[2] = -interpQuat[2]; interpQuat[3] = -interpQuat[3];
                                        }
                                        prevSampleQuat = interpQuat;

                                        // Convert interpolated quaternion to Euler angles with continuity
                                        Eigen::Vector3d interpEuler = quatToEulerContinuous(interpQuat, prevEuler);
                                        prevEuler = interpEuler;

                                        // Add sampled keyframe
                                        FbxTime sampleFbxTime(sampleTime * FBXSDK_TC_SECOND);

                                        int sxIndex = lRotationCurveX->KeyAdd(sampleFbxTime);
                                        int syIndex = lRotationCurveY->KeyAdd(sampleFbxTime);
                                        int szIndex = lRotationCurveZ->KeyAdd(sampleFbxTime);

                                        lRotationCurveX->KeySetValue(sxIndex, interpEuler.x());
                                        lRotationCurveY->KeySetValue(syIndex, interpEuler.y());
                                        lRotationCurveZ->KeySetValue(szIndex, interpEuler.z());

                                        // Use constant interpolation to prevent Unity from modifying tangents
                                        lRotationCurveX->KeySetInterpolation(sxIndex, FbxAnimCurveDef::eInterpolationConstant);
                                        lRotationCurveY->KeySetInterpolation(syIndex, FbxAnimCurveDef::eInterpolationConstant);
                                        lRotationCurveZ->KeySetInterpolation(szIndex, FbxAnimCurveDef::eInterpolationConstant);
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
                    
                    QModelIndex vkeys = nif->getIndex( iVisibilities, "Visibility Keys" );
                    if(vkeys.isValid())
                    {
                        // Get the animation curve for visibility (create if needed)
                        FbxAnimCurve* lVisibilityCurve = node->Visibility.GetCurve(animLayer, true);
                        
                        if (lVisibilityCurve) {
                            lVisibilityCurve->KeyModifyBegin();
                            
                            for ( int vindex = 0; vindex < nif->rowCount( vkeys ); vindex++ ) {
                                QModelIndex vkey = vkeys.child( vindex, 0 );
                                float currentTime = nif->get<float>( vkey, "Time" );
                                unsigned char isVisible = nif->get<unsigned char>( vkey, "Value" );
                                
                                FbxTime fbxTime(currentTime * FBXSDK_TC_SECOND);
                                
                                // Add visibility keyframe
                                int vKeyIndex = lVisibilityCurve->KeyAdd(fbxTime);
                                
                                // Set visibility value (1.0 for visible, 0.0 for hidden)
                                lVisibilityCurve->KeySetValue(vKeyIndex, isVisible ? 1.0 : 0.0);
                                
                                // Use constant interpolation for visibility (sharp on/off transitions)
                                lVisibilityCurve->KeySetInterpolation(vKeyIndex, FbxAnimCurveDef::eInterpolationConstant);
                            }
                            
                            lVisibilityCurve->KeyModifyEnd();
                        }
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
                    //auto emissive = nif->get<Color3>( ipBlock, "Emissive Color" );
                    auto shininess = nif->get<float>( ipBlock, "Glossiness" );
                    
                    FbxSurfaceLambert* lMaterial = FbxSurfaceLambert::Create(scene, "");
                    
                    // Generate primary and secondary colors.
                    //lMaterial->Emissive           .Set(FbxDouble3(emissive.red(), emissive.green(), emissive.blue()));
                    lMaterial->Emissive           .Set(FbxDouble3(0.0, 0.0, 0.0));
                    lMaterial->Ambient            .Set(FbxDouble3(ambient.red(), ambient.green(), ambient.blue()));
                    lMaterial->AmbientFactor      .Set(1.);
                    // Add texture for diffuse channel
                    lMaterial->Diffuse           .Set(FbxDouble3(diffuse.red(), diffuse.green(), diffuse.blue()));
                    lMaterial->DiffuseFactor     .Set(1.);
                    lMaterial->TransparencyFactor.Set(1.0 - alpha); // Invert: FBX uses opacity not transparency
                    lMaterial->ShadingModel      .Set("Lambert");
                    //lMaterial->Shininess         .Set(shininess);
                    //lMaterial->Specular          .Set(FbxDouble3(specular.red(), specular.green(), specular.blue()));
                    //lMaterial->SpecularFactor    .Set(0.0);
                    
                    ctx.materialMap[nif->getBlockNumber( iBlock )] = lMaterial;
                }
                
                else if(nif->isNiBlock( ipBlock, "NiTextureProperty") || nif->isNiBlock( ipBlock, "NiMultiTextureProperty"))
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
                                
                                std::string diffuseFilename = ctx.ExportPath + "/Textures/" + textureName + ".png";
                                std::string alphaFilename = ctx.ExportPath + "/Textures/" + textureName + "_alpha.png";
                                std::string emissiveFilename = ctx.ExportPath + "/Textures/" + textureName + "_emissive.png";
                                
                                // Get the emissive color from the material property
                                Color3 emissiveColor;
                                for ( const auto pl : nif->getLinkArray( iBlock, "Properties" ) ) {
                                    QModelIndex ipBlock = nif->getBlock( pl );
                                    if(nif->isNiBlock( ipBlock, "NiMaterialProperty")) {
                                        emissiveColor = nif->get<Color3>( ipBlock, "Emissive Color" );
                                        break;
                                    }
                                }
                                
                                if ( iPixelData.isValid() ) {
                                    if ( QByteArray * pdata = nif->get<QByteArray *>( iPixelData.child(0, 0) ) ) {
                                        // Save the main diffuse texture
                                        if(!std::filesystem::exists(diffuseFilename))
                                            stbi_write_png(diffuseFilename.c_str(), width, height, components, pdata->data(), width * components);
                                        
                                        // If RGBA, create separate alpha/transparency map
                                        if (components == 4) {
                                            if (!std::filesystem::exists(alphaFilename))
                                                CreateTransparencyMap(diffuseFilename, alphaFilename, width, height, reinterpret_cast<const unsigned char*>(pdata->data()));
                                            
                                            // Create emissive map if emissive color is not black
                                            if (!std::filesystem::exists(emissiveFilename) &&
                                                (emissiveColor.red() > 0.0f || emissiveColor.green() > 0.0f || emissiveColor.blue() > 0.0f)) {
                                                CreateEmissiveMap(diffuseFilename, emissiveFilename, width, height,
                                                                  reinterpret_cast<const unsigned char*>(pdata->data()), emissiveColor);
                                            }
                                        }
                                    }
                                }
                                if(!nif->isNiBlock( ipBlock, "NiMultiTextureProperty") && ctx.textureMap.find(nif->getBlockNumber( iBlock )) == ctx.textureMap.end())
                                {
                                    ctx.textureMap[nif->getBlockNumber( iBlock )].push_back(TEXTURE_INSTANCE {textureName, components == 4, emissiveColor });
                                }
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
            
            //float frustumLeft = nif->get<float>( iBlock, "Frustum Left");
            //float frustumRight = nif->get<float>( iBlock, "Frustum Right");
            //float frustumTop = nif->get<float>( iBlock, "Frustum Top");
            //float frustumBottom = nif->get<float>( iBlock, "Frustum Bottom");
            float frustumNear = nif->get<float>( iBlock, "Frustum Near");
            //float frustumFar = nif->get<float>( iBlock, "Frustum Far");
            
            //float viewportLeft = nif->get<float>( iBlock, "Viewport Left");
            //float viewportRight = nif->get<float>( iBlock, "Viewport Right");
            //float viewportTop = nif->get<float>( iBlock, "Viewport Top");
            //float viewportBottom = nif->get<float>( iBlock, "Viewport Bottom");
            
            // Get the camera's view plane vectors (in NIF Z-up space)
            //auto ViewPlaneNormal = nif->get<Vector3>( iBlock, "ViewPlane Normal");
            //auto ViewPlaneUp = nif->get<Vector3>( iBlock, "ViewPlane Up");
            //auto ViewPlaneRight = nif->get<Vector3>( iBlock, "ViewPlane Right");
            
            lCamera->NearPlane.Set(frustumNear);
            lCamera->FarPlane.Set(100000.0);
            
            parentnode->SetName("GameCamera");
            
            // Set camera to face world up (Y-axis) in Maya/FBX Y-up coordinate system
            // FBX cameras look down -Z by default, so we need -90 degrees on X to look up
            parentnode->LclRotation.Set(FbxDouble3(-90.0, 0.0, -90));
            
            parentnode->SetNodeAttribute(lCamera);
            
            ctx.cameras.push_back(lCamera);
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

void ProcessObjects(const NifModel * nif, const QModelIndex & iNode, FbxScene* pScene, FBX_ExportContext &ctx)
{
    foreach ( const int l, nif->getChildLinks( nif->getBlockNumber( iNode )) ) {
        QModelIndex iBlock = nif->getBlock( l );
        
        if(nif->isNiBlock( iBlock, "NiNode") || nif->inherits( iBlock, "NiNode" ))
        {
            ProcessObjects(nif, iBlock, pScene, ctx);
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

            if(nif->getBlockNumber( iBlock ) == 949)
                int test = 0;
            
            FbxSurfaceLambert* lMaterial = ctx.materialMap[nif->getBlockNumber( iBlock )];
            
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
                auto textures = ctx.textureMap[nif->getBlockNumber( iBlock )];

                if(textures.size())
                {
                    auto textureInfo = textures.front();
                    std::string textureFilename = ctx.ExportPath + "/Textures/" + textureInfo.texture + ".png";
                    
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
                    FbxFileTexture* eTexture = nullptr;
                    // If the texture has an alpha channel, set up proper alpha blending
                    if(textureInfo.hasTransparency)
                    {
                        // Create a separate transparency texture pointing to the alpha map
                        std::string alphaFilename = ctx.ExportPath + "/Textures/" + textureInfo.texture + "_alpha.png";
                        
                        tTexture = FbxFileTexture::Create(pScene, (textureInfo.texture + "_alpha").c_str());
                        tTexture->SetFileName(alphaFilename.c_str());
                        tTexture->SetTextureUse(FbxTexture::eStandard);
                        tTexture->SetMappingType(FbxTexture::eUV);
                        tTexture->SetSwapUV(false);
                        tTexture->SetMaterialUse(FbxFileTexture::eModelMaterial);
                        
                        // For grayscale alpha maps, use eBlack (black = transparent, white = opaque)
                        tTexture->SetAlphaSource(FbxTexture::EAlphaSource::eBlack);
                        
                        tTexture->SetTranslation(0.0, 0.0);
                        tTexture->SetScale(1.0, 1.0);
                        tTexture->SetRotation(0.0, 0.0);
                        
                        // Connect to TransparentColor for Maya compatibility
                        lMaterial->TransparentColor.ConnectSrcObject(tTexture);
                        
                        // Set base transparency to maximum so texture controls it
                        lMaterial->TransparencyFactor.Set(1.0);
                        
                        // Apply emissive texture if it exists
                        std::string emissiveFilename = ctx.ExportPath + "/Textures/" + textureInfo.texture + "_emissive.png";
                        if (std::filesystem::exists(emissiveFilename)) {
                            eTexture = FbxFileTexture::Create(pScene, (textureInfo.texture + "_emissive").c_str());
                            eTexture->SetFileName(emissiveFilename.c_str());
                            eTexture->SetTextureUse(FbxTexture::eStandard);
                            eTexture->SetMappingType(FbxTexture::eUV);
                            eTexture->SetSwapUV(false);
                            eTexture->SetMaterialUse(FbxFileTexture::eModelMaterial);
                            eTexture->SetTranslation(0.0, 0.0);
                            eTexture->SetScale(1.0, 1.0);
                            eTexture->SetRotation(0.0, 0.0);
                            
                            // Connect to Emissive channel
                            lMaterial->Emissive.ConnectSrcObject(eTexture);
                            lMaterial->EmissiveFactor.Set(1.0);
                        }
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
                    
                    // Create and configure the UV element for polygon mapping (not control point).
                    // This ensures UVs are properly associated with faces for Maya compatibility.
                    FbxLayerElementUV* lUVElement = FbxLayerElementUV::Create(mesh, "DiffuseUV");
                    lUVElement->SetMappingMode(FbxLayerElement::eByPolygonVertex); // Map UVs to polygon vertices
                    lUVElement->SetReferenceMode(FbxLayerElement::eIndexToDirect); // Use indexed direct reference
      
                    // Build UV indices for each polygon vertex
                    for(int i = 0; i < triangles.count(); i++)
                    {
                        // Each triangle has 3 vertices
                        for(int v = 0; v < 3; v++)
                        {
                            int vertexIndex = triangles[i][v];
                            if(vertexIndex < textureCoords.count())
                            {
                                lUVElement->GetDirectArray().Add(textureCoords[vertexIndex]);
                                lUVElement->GetIndexArray().Add(lUVElement->GetDirectArray().GetCount() - 1);
                            }
                            else
                            {
                                // Fallback UV if out of range
                                lUVElement->GetDirectArray().Add(FbxVector2(0.0, 0.0));
                                lUVElement->GetIndexArray().Add(lUVElement->GetDirectArray().GetCount() - 1);
                            }
                        }
                    }
                    lLayer->SetUVs(lUVElement, FbxLayerElement::EType::eTextureDiffuse);
                }
                else
                {
                    // No texture - ensure TransparentColor is set for Maya compatibility
                    // Maya needs TransparentColor set to white * transparency for materials without maps
                    double transFactor = lMaterial->TransparencyFactor.Get();
                    if (transFactor > 0.0)
                    {
                        // Set TransparentColor to gray value based on transparency factor
                        // In Maya: white = fully transparent, black = opaque
                        lMaterial->TransparentColor.Set(FbxDouble3(transFactor, transFactor, transFactor));
                    }
                }
                meshNode->AddMaterial(lMaterial);
            }
            
            // Add texture flipper properties if this mesh has animated textures
            if (ctx.textureFlipMap.find(nif->getBlockNumber( iBlock )) != ctx.textureFlipMap.end() && lMaterial) {
                auto flipInfo = ctx.textureFlipMap[nif->getBlockNumber( iBlock )];

                // Create custom properties for Maya texture flipper script
                FbxProperty flipProp = FbxProperty::Create(lMaterial, FbxDoubleDT, "NifTextureFlipStartTime");
                flipProp.Set(FbxDouble(flipInfo.startTime));
                flipProp.ModifyFlag(FbxPropertyFlags::eUserDefined, true);

                FbxProperty rateProp = FbxProperty::Create(lMaterial, FbxDoubleDT, "NifTextureFlipRate");
                rateProp.Set(FbxDouble(flipInfo.rate));
                rateProp.ModifyFlag(FbxPropertyFlags::eUserDefined, true);

                // Store the texture names
                FbxProperty framesProp = FbxProperty::Create(lMaterial, FbxStringDT, "NifTextureFlipTextures");
                framesProp.Set(FbxString(flipInfo.textureNames.c_str()));
                framesProp.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
            }

            AddNodeRecursively(ctx, meshNode);
        }
        else if(nif->isNiBlock( iBlock, "NiLight"))
        {
            unsigned char switchState  = nif->get<unsigned char>( iBlock, "Switch State" );
            float spotAngle = nif->get<float>( iBlock, "Spot Angle" );
            float spotExponent = nif->get<float>( iBlock, "Spot Exponent" );
            float dimmer = nif->get<float>( iBlock, "Dimmer" );
            //Color3 ambientColor = nif->get<Color3>( iBlock, "Ambient Color" );
            Color3 diffuseColor = nif->get<Color3>( iBlock, "Diffuse Color" );
            //Color3 specularColor = nif->get<Color3>( iBlock, "Specular Color" );
            float attenuationDistance = nif->get<float>( iBlock, "Attenuation Distance" );
            //float attenuationCurve = nif->get<float>( iBlock, "Attenuation Curve" );
            unsigned char attenuation  = nif->get<unsigned char>( iBlock, "Attenuation" );
            uint lightType = nif->get<uint>( iBlock, "Light Type" );

            FbxLight* lLight = FbxLight::Create(pScene, "Light");
            if (lLight) {
                // Determine light type - default to point light
                FbxLight::EType fbxlightType = FbxLight::ePoint;
                switch (lightType) {
                case 0:
                    fbxlightType = FbxLight::eArea;
                    break;
                case 2:
                    fbxlightType = FbxLight::ePoint;
                    break;
                case 3:
                    fbxlightType = FbxLight::eDirectional;
                    break;
                case 4:
                    fbxlightType = FbxLight::eSpot;
                    break;
                }

                lLight->LightType.Set(fbxlightType);

                // Set light color (using diffuse as the primary light color)
                lLight->Color.Set(FbxDouble3(diffuseColor.red(), diffuseColor.green(), diffuseColor.blue()));

                float intensity = dimmer * 100.0f;
                lLight->Intensity.Set(intensity);

                lLight->CastLight.Set(switchState != 0);

                if (lightType == FbxLight::eSpot) {
                    // Convert spot angle from degrees to FBX format
                    // FBX uses OuterAngle for the full cone angle
                    lLight->OuterAngle.Set(spotAngle);

                    // Inner angle is typically smaller - use a reasonable default
                    // or derive from spot exponent (higher exponent = tighter hotspot)
                    float innerAngle = spotAngle * 0.8f; // 80% of outer angle
                    lLight->InnerAngle.Set(innerAngle);

                    // Fog intensity based on spot exponent
                    lLight->Fog.Set(spotExponent);
                }

                if (lightType != FbxLight::eDirectional) {
                    FbxLight::EDecayType decayType = FbxLight::eNone;

                    switch (attenuation) {
                    case 0: // No attenuation
                        decayType = FbxLight::eNone;
                        break;
                    case 1: // Linear attenuation
                        decayType = FbxLight::eLinear;
                        break;
                    case 2: // Quadratic attenuation (most physically accurate)
                        decayType = FbxLight::eQuadratic;
                        break;
                    case 3: // Cubic attenuation
                        decayType = FbxLight::eCubic;
                        break;
                    default:
                        decayType = FbxLight::eQuadratic; // Default to physically accurate
                        break;

                        lLight->DecayType.Set(decayType);

                    }
                }
                /*
                if (attenuationDistance > 0.0f && attenuationDistance < 100000.0f) {
                    lLight->EnableFarAttenuation.Set(true);
                    lLight->FarAttenuationEnd.Set(attenuationDistance);
                    lLight->FarAttenuationStart.Set(attenuationDistance * 0.5f); // Start at 50% of max distance
                }
                */

                lLight->CastShadows.Set(true);

                auto parentnode = ctx.NodeMap[nif->getBlockNumber( iNode )];
                if (parentnode) {
                    FbxNode* lightNode = FbxNode::Create(pScene, std::string(parentnode->GetName()).append("_Light").c_str());
                    lightNode->SetNodeAttribute(lLight);

                    parentnode->AddChild(lightNode);
                    ctx.NodeMap[nif->getBlockNumber( iBlock )] = lightNode;
                    AddNodeRecursively(ctx, lightNode);
                } else {
                    qCCritical( nsIo ) << "Failed to find parent node for light with block ID" << nif->getBlockNumber( iBlock );
                }
            }
            else {
                qCCritical( nsIo ) << "Failed to create FbxLight for block ID" << nif->getBlockNumber( iBlock );
            }
        }
    }
}

void ProcessBillboards(FbxScene* pScene, FBX_ExportContext &ctx)
{
    if (ctx.billboardNodes.empty() || ctx.cameras.empty())
        return;

    FbxCamera* targetCamera = ctx.cameras.front();
    if (!targetCamera)
        return;

    FbxNode* cameraNode = targetCamera->GetNode();
    if (!cameraNode)
        return;

    for (FbxNode* billboardNode : ctx.billboardNodes)
    {
        if (!billboardNode)
            continue;

        // Add custom property to mark this as a billboard
        FbxProperty billboardProp = FbxProperty::Create(billboardNode, FbxStringDT, "NifBillboard");
        billboardProp.Set(FbxString(cameraNode->GetName()));
        billboardProp.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
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
        qInfo( nsIo ) << "Failed to find scene root, using root node...";
        auto links = nif->getRootLinks();
        foreach ( int l,  links ) {
            QModelIndex iChild = nif->getBlock( l );

            if ( nif->inherits( iChild, "NiNode" ) )
            {
                FbxNode* node = node = FbxNode::Create(pScene, "Scene_Root" );
                if(node) {
                    iRoot = iChild;
                    ctx.NodeMap[nif->getBlockNumber( iRoot )] = node;
                    break;
                }
            }
        }
        if(!iRoot.isValid())
        {
            qCCritical( nsIo ) << "No NiNode found in root links, cannot export scene.";
            return false;
        }
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

    ProcessTextureFlips(nif, iRoot, pScene, ctx);
    ProcessNode(nif, iRoot, pScene, lAnimLayer, lSkeletonRoot, ctx);
    // Process objects after navigating node tree to ensure bones are mapped
    ProcessObjects(nif, iRoot, pScene, ctx);
    ProcessBillboards(pScene, ctx);

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
