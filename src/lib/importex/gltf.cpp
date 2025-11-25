#include "gltf.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "tiny_gltf.h"

#include "message.h"
#include "model/nifmodel.h"
#include "data/niftypes.h"
#include "exportcommon.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QDebug>

#include <filesystem>
#include <map>
#include <vector>
#include <algorithm>

#define tr(x) QApplication::tr(x)

using namespace tinygltf;

// Scale factor: NIF units to meters
// Adjust this based on your source game:
// - Oblivion/Skyrim: 1 NIF unit = ~0.0143 meters (70 units per meter)
// - Fallout 3/NV: 1 NIF unit = ~0.01428 meters
// - For centimeters: use 0.01
constexpr float NIF_TO_METERS = 0.01f; // Default: assume NIF is in centimeters

struct GLTF_TextureInstance {
    std::string textureName;
    bool hasTransparency;
    Color3 emissiveColor;
};

struct GLTF_ExportContext {
    Model model;
    std::string exportPath;

    // Mappings from NIF block IDs to glTF indices
    std::map<uint, int> nodeMap;          // NIF block ID -> glTF node index
    std::map<uint, int> meshMap;          // NIF block ID -> glTF mesh index
    std::map<uint, int> materialMap;      // NIF block ID -> glTF material index
    std::map<uint, int> textureMap;       // NIF block ID -> glTF texture index
    std::map<uint, int> imageMap;         // NIF block ID -> glTF image index
    std::map<uint, int> skinMap;          // NIF block ID -> glTF skin index

    std::map<uint, std::vector<GLTF_TextureInstance>> textureInstances;

    // Animation data
    std::vector<float> animationTimes;
    int animationIndex = -1;

    // Billboard tracking
    std::vector<int> billboardNodes;      // List of node indices that are billboards
    int cameraNodeIdx = -1;               // Index of the camera node (if any)
};

// Helper: Add buffer data to model
int AddBufferData(GLTF_ExportContext& ctx, const std::vector<unsigned char>& data)
{
    Buffer buffer;
    buffer.data = data;

    // For GLB: empty URI means embedded in binary chunk
    // For glTF JSON: set URI to external .bin file
    // We'll determine this later based on file extension
    buffer.uri = "";  // Will be set during write

    int bufferIdx = ctx.model.buffers.size();
    ctx.model.buffers.push_back(buffer);
    return bufferIdx;
}

// Helper: Create buffer view
int AddBufferView(GLTF_ExportContext& ctx, int bufferIdx, size_t byteOffset, size_t byteLength, int target = 0)
{
    BufferView bufferView;
    bufferView.buffer = bufferIdx;
    bufferView.byteOffset = byteOffset;
    bufferView.byteLength = byteLength;
    if (target > 0) {
        bufferView.target = target;
    }

    int viewIdx = ctx.model.bufferViews.size();
    ctx.model.bufferViews.push_back(bufferView);
    return viewIdx;
}

// Helper: Create accessor
int AddAccessor(GLTF_ExportContext& ctx, int bufferViewIdx, int componentType, int count,
                const int& type, const std::vector<double>& minValues = {},
                const std::vector<double>& maxValues = {})
{
    Accessor accessor;
    accessor.bufferView = bufferViewIdx;
    accessor.byteOffset = 0;
    accessor.componentType = componentType;
    accessor.count = count;
    accessor.type = type;

    if (!minValues.empty()) accessor.minValues = minValues;
    if (!maxValues.empty()) accessor.maxValues = maxValues;

    int accIdx = ctx.model.accessors.size();
    ctx.model.accessors.push_back(accessor);
    return accIdx;
}

// Process textures and create glTF images/textures
void ProcessTextures(const NifModel* nif, const QModelIndex& iBlock, GLTF_ExportContext& ctx)
{
    uint meshBlockNum = nif->getBlockNumber(iBlock);

    for (const auto pl : nif->getLinkArray(iBlock, "Properties")) {
        QModelIndex ipBlock = nif->getBlock(pl);

        if (nif->isNiBlock(ipBlock, "NiTextureProperty") || nif->isNiBlock(ipBlock, "NiMultiTextureProperty")) {
            foreach(const int cl, nif->getChildLinks(nif->getBlockNumber(ipBlock))) {
                QModelIndex ciBlock = nif->getBlock(cl);
                if (nif->isNiBlock(ciBlock, "NiImage")) {
                    uint imageBlockNum = nif->getBlockNumber(ciBlock);

                    // Check if we already processed this image
                    if (ctx.imageMap.find(imageBlockNum) != ctx.imageMap.end()) {
                        // Already processed, just add reference
                        int textureIdx = ctx.textureMap[imageBlockNum];
                        ctx.textureInstances[meshBlockNum].push_back(
                            GLTF_TextureInstance{std::to_string(imageBlockNum), false, Color3()}
                            );
                        continue;
                    }

                    QModelIndex iImage = nif->getBlock(nif->getLink(ciBlock, "Image Data"));
                    if (nif->getBlockName(iImage) == "NiRawImageData") {
                        auto width = nif->get<uint>(iImage, "Width");
                        auto height = nif->get<uint>(iImage, "Height");
                        auto type = nif->get<int>(iImage, "Image Type");

                        QModelIndex iPixelData;
                        int components;
                        switch (type) {
                        case 1: components = 3; iPixelData = nif->getIndex(iImage, "RGB Image Data"); break;
                        case 2: components = 4; iPixelData = nif->getIndex(iImage, "RGBA Image Data"); break;
                        default: continue;
                        }

                        if (iPixelData.isValid()) {
                            if (QByteArray* pdata = nif->get<QByteArray*>(iPixelData.child(0, 0))) {
                                // Create glTF image
                                Image image;
                                image.name = std::to_string(imageBlockNum);
                                image.width = width;
                                image.height = height;
                                image.component = components;
                                image.bits = 8;
                                image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;

                                // Set MIME type for embedded images
                                image.mimeType = "image/png";

                                // Copy image data (raw pixel data, not PNG)
                                image.image.resize(pdata->size());
                                memcpy(image.image.data(), pdata->data(), pdata->size());

                                int imageIdx = ctx.model.images.size();
                                ctx.model.images.push_back(image);
                                ctx.imageMap[imageBlockNum] = imageIdx;

                                // Create glTF texture with sampler
                                Texture texture;
                                texture.source = imageIdx;
                                texture.name = std::to_string(imageBlockNum);

                                // Create default sampler for proper texture filtering
                                Sampler sampler;
                                sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
                                sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
                                sampler.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
                                sampler.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;

                                int samplerIdx = ctx.model.samplers.size();
                                ctx.model.samplers.push_back(sampler);
                                texture.sampler = samplerIdx;

                                int textureIdx = ctx.model.textures.size();
                                ctx.model.textures.push_back(texture);
                                ctx.textureMap[imageBlockNum] = textureIdx;

                                // Get emissive color from material property
                                Color3 emissiveColor;
                                for (const auto pl2 : nif->getLinkArray(iBlock, "Properties")) {
                                    QModelIndex ipBlock2 = nif->getBlock(pl2);
                                    if (nif->isNiBlock(ipBlock2, "NiMaterialProperty")) {
                                        emissiveColor = nif->get<Color3>(ipBlock2, "Emissive Color");
                                        break;
                                    }
                                }

                                // Store texture instance for this mesh
                                ctx.textureInstances[meshBlockNum].push_back(
                                    GLTF_TextureInstance{std::to_string(imageBlockNum), components == 4, emissiveColor}
                                    );
                            }
                        }
                    }
                }
            }
        }
    }
}

// Create glTF material from NIF properties
int CreateMaterial(const NifModel* nif, const QModelIndex& iBlock, GLTF_ExportContext& ctx)
{
    uint meshBlockNum = nif->getBlockNumber(iBlock);

    if (ctx.materialMap.find(meshBlockNum) != ctx.materialMap.end()) {
        return ctx.materialMap[meshBlockNum];
    }

    Material material;
    material.name = nif->get<QString>(iBlock, "Name").toStdString();
    if (material.name.empty()) {
        material.name = "Material_" + std::to_string(meshBlockNum);
    }

    // Default PBR values
    material.pbrMetallicRoughness.baseColorFactor = {1.0, 1.0, 1.0, 1.0};
    material.pbrMetallicRoughness.metallicFactor = 0.0;
    material.pbrMetallicRoughness.roughnessFactor = 1.0;

    // Check if this mesh has textures FIRST
    bool hasTexture = ctx.textureInstances.find(meshBlockNum) != ctx.textureInstances.end()
                      && !ctx.textureInstances[meshBlockNum].empty();

    // Process material properties
    for (const auto pl : nif->getLinkArray(iBlock, "Properties")) {
        QModelIndex ipBlock = nif->getBlock(pl);

        if (nif->isNiBlock(ipBlock, "NiMaterialProperty")) {
            float alpha = nif->get<float>(ipBlock, "Alpha");
            alpha = std::max(0.0f, std::min(1.0f, alpha));

            auto diffuse = nif->get<Color3>(ipBlock, "Diffuse Color");

            if (!hasTexture) {
                // No texture - use the material's diffuse color
                material.pbrMetallicRoughness.baseColorFactor = {
                    diffuse.red(), diffuse.green(), diffuse.blue(), alpha
                };
            } else {
                material.pbrMetallicRoughness.baseColorFactor = {1.0, 1.0, 1.0, alpha};
            }

            // CRITICAL FIX: Use MASK mode instead of BLEND for better depth handling
            // BLEND mode causes depth sorting issues at close range
            if (alpha < 1.0f && alpha > 0.0f) {
                material.alphaMode = "MASK";  // Changed from BLEND
                material.alphaCutoff = 0.5;   // Binary transparency threshold
                material.doubleSided = true;
            } else if (alpha <= 0.0f) {
                // Fully transparent - still use BLEND
                material.alphaMode = "BLEND";
                material.doubleSided = true;
            }
            // If alpha == 1.0f, use default OPAQUE mode
        }
    }

    // Add textures
    if (hasTexture) {
        auto& textures = ctx.textureInstances[meshBlockNum];
        if (!textures.empty()) {
            auto& texInfo = textures.front();

            // Convert texture name string back to block number for lookup
            uint textureBlockNum = std::stoul(texInfo.textureName);

            // Find texture index by block number
            if (ctx.textureMap.find(textureBlockNum) != ctx.textureMap.end()) {
                int textureIdx = ctx.textureMap[textureBlockNum];

                TextureInfo baseColorTexture;
                baseColorTexture.index = textureIdx;
                baseColorTexture.texCoord = 0;  // Use TEXCOORD_0
                material.pbrMetallicRoughness.baseColorTexture = baseColorTexture;

                // CRITICAL FIX: Always use MASK mode for textures with alpha
                // This prevents depth sorting issues at close range
                if (texInfo.hasTransparency) {
                    material.alphaMode = "MASK";  // Changed from BLEND
                    material.alphaCutoff = 0.5;   // Pixels with alpha < 0.5 are discarded
                    material.doubleSided = true;
                }

                qDebug() << "Assigned texture" << textureIdx << "to material" << QString::fromStdString(material.name);
            } else {
                qWarning() << "Failed to find texture index for block" << textureBlockNum;
            }
        }
    }

    int materialIdx = ctx.model.materials.size();
    ctx.model.materials.push_back(material);
    ctx.materialMap[meshBlockNum] = materialIdx;

    return materialIdx;
}

void ProcessMorphTargetAnimation(const NifModel* nif, const QModelIndex& iBlock,
                                 uint targetIndex, uint targetCount,
                                 const std::string& targetName, GLTF_ExportContext& ctx)
{
    uint morphKeyCount = nif->get<uint>(iBlock, "Morph Key Count");
    if (morphKeyCount == 0) {
        return;
    }

    auto morphKeyType = nif->get<uint>(iBlock, "Morph Key Type");
    QModelIndex iMorphKeys = nif->getIndex(iBlock, "Morph Keys");

    if (!iMorphKeys.isValid()) {
        return;
    }

    std::vector<float> times;
    std::vector<float> weights;

    // Extract keyframe data based on type
    for (uint k = 0; k < morphKeyCount; k++) {
        QModelIndex iKey = iMorphKeys.child(k, 0);
        if (!iKey.isValid()) {
            continue;
        }

        float keyTime = 0.0f;
        float keyWeight = 0.0f;

        if (morphKeyType == 1) { // LINEAR_KEY
            keyTime = nif->get<float>(iKey, "Time");
            keyWeight = nif->get<float>(iKey, "Value");
        }
        else if (morphKeyType == 2) { // BEZIER_KEY
            QModelIndex iNiKey = nif->getIndex(iKey, "Key");
            keyTime = nif->get<float>(iNiKey, "Time");
            keyWeight = nif->get<float>(iNiKey, "Value");
            // TODO: Sample Bezier curve using OutTan for smoother interpolation
        }
        else if (morphKeyType == 3) { // TBC_KEY
            QModelIndex iNiKey = nif->getIndex(iKey, "Key");
            keyTime = nif->get<float>(iNiKey, "Time");
            keyWeight = nif->get<float>(iNiKey, "Value");
            // TODO: Use Tension/Bias/Continuity for proper interpolation
        }
        else if (morphKeyType == 4) { // MORPH_KEY (NiMorphKey)
            QModelIndex iTCBKey = nif->getIndex(iKey, "TCB Float Key");
            QModelIndex iFloatKey = nif->getIndex(iTCBKey, "Key");
            keyTime = nif->get<float>(iFloatKey, "Time");
            keyWeight = nif->get<float>(iFloatKey, "Value");
        }
        else if (morphKeyType == 5) { // BARY_MORPH_KEY (NiBaryMorphKey)
            QModelIndex iMorphKey = nif->getIndex(iKey, "Morph Key");
            QModelIndex iTCBKey = nif->getIndex(iMorphKey, "TCB Float Key");
            QModelIndex iFloatKey = nif->getIndex(iTCBKey, "Key");
            keyTime = nif->get<float>(iFloatKey, "Time");

            // For barycentric morph, use weight from Weights 1 array for this target
            QModelIndex iWeights1 = nif->getIndex(iKey, "Weights 1");
            if (iWeights1.isValid() && targetIndex < nif->rowCount(iWeights1)) {
                keyWeight = nif->get<float>(iWeights1.child(targetIndex, 0));
            } else {
                keyWeight = 0.0f;
            }
        }
        else if (morphKeyType == 6) { // CUBIC_MORPH_KEY (NiCubicMorphKey)
            qWarning() << "NiCubicMorphKey (type 6) not yet supported for morph target animation";
            continue;
        }

        times.push_back(keyTime);
        weights.push_back(keyWeight);
    }

    if (times.empty()) {
        return;
    }

    // Create animation for this specific morph target
    Animation anim;
    anim.name = targetName;

    // Pack animation data
    std::vector<unsigned char> timeData;
    std::vector<unsigned char> weightData;

    for (float t : times) {
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&t);
        timeData.insert(timeData.end(), bytes, bytes + sizeof(float));
    }

    // Create weights array with ALL morph targets (glTF requirement)
    // Only this target will be animated, others stay at 0
    for (size_t k = 0; k < times.size(); k++) {
        for (uint t = 0; t < targetCount; t++) {
            float w = (t == targetIndex) ? weights[k] : 0.0f;
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&w);
            weightData.insert(weightData.end(), bytes, bytes + sizeof(float));
        }
    }

    // Create buffer
    std::vector<unsigned char> animBufferData;
    size_t timeOffset = 0;
    size_t weightOffset = timeData.size();

    animBufferData.insert(animBufferData.end(), timeData.begin(), timeData.end());
    animBufferData.insert(animBufferData.end(), weightData.begin(), weightData.end());

    int animBufferIdx = AddBufferData(ctx, animBufferData);

    // Create accessors
    int timeViewIdx = AddBufferView(ctx, animBufferIdx, timeOffset, timeData.size());
    int timeAccIdx = AddAccessor(ctx, timeViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                 times.size(), TINYGLTF_TYPE_SCALAR,
                                 {times.front()}, {times.back()});

    int weightViewIdx = AddBufferView(ctx, animBufferIdx, weightOffset, weightData.size());
    int weightAccIdx = AddAccessor(ctx, weightViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                   times.size(), TINYGLTF_TYPE_SCALAR);

    // Create animation sampler
    AnimationSampler sampler;
    sampler.input = timeAccIdx;
    sampler.output = weightAccIdx;
    sampler.interpolation = "LINEAR";

    int samplerIdx = anim.samplers.size();
    anim.samplers.push_back(sampler);

    // Create animation channel targeting the morph weights
    AnimationChannel channel;
    channel.sampler = samplerIdx;
    channel.target_node = nif->getBlockNumber(iBlock);
    channel.target_path = "weights";

    anim.channels.push_back(channel);

    // Add animation to model
    ctx.model.animations.push_back(anim);

    qInfo(nsIo) << "Exported morph target animation" << QString::fromStdString(targetName)
                << "for target" << targetIndex
                << "with" << times.size() << "keyframes";
}

void ProcessMorphTargets(const NifModel* nif, const QModelIndex& iBlock, int meshIdx, GLTF_ExportContext& ctx)
{
    if (!nif->isNiBlock(iBlock, "Ni3dsMorphShape")) {
        return;
    }

    // Get base mesh information
    QVector<Vector3> baseVerts = nif->getArray<Vector3>(iBlock, "Vertices");
    if (baseVerts.isEmpty()) {
        return;
    }

    uint targetCount = nif->get<uint>(iBlock, "Target Count");
    uint morphKeyCount = nif->get<uint>(iBlock, "Morph Key Count");

    if (targetCount == 0) {
        return;
    }

    // Get the morph key type
    auto morphKeyType = nif->get<uint>(iBlock, "Morph Key Type");

    // Get target vertices array
    QModelIndex iTargetVerts = nif->getIndex(iBlock, "Target Vertices");
    if (!iTargetVerts.isValid()) {
        return;
    }

    // glTF supports morph targets as position offsets from base mesh
    Mesh& mesh = ctx.model.meshes[meshIdx];

    // Storage for morph target data
    std::vector<std::vector<Vector3>> morphTargets;
    std::vector<std::string> morphTargetNames;
    std::vector<double> morphWeights;

    // Extract morph targets
    // Target vertices are stored as: [target0_vert0, target0_vert1, ..., target1_vert0, ...]
    int vertsPerTarget = baseVerts.count();

    for (uint t = 0; t < targetCount; t++) {
        std::vector<Vector3> targetVerts;
        targetVerts.reserve(vertsPerTarget);

        for (int v = 0; v < vertsPerTarget; v++) {
            int index = t * vertsPerTarget + v;
            QModelIndex iVert = iTargetVerts.child(index, 0);

            if (iVert.isValid()) {
                Vector3 vert = nif->get<Vector3>(iVert);
                targetVerts.push_back(vert);
            } else {
                // If vertex data is missing, use base vertex
                targetVerts.push_back(baseVerts[v]);
            }
        }

        morphTargets.push_back(targetVerts);
        morphTargetNames.push_back("Target_" + std::to_string(t));
    }

    // Get initial weights (first keyframe values)
    QModelIndex iMorphKeys = nif->getIndex(iBlock, "Morph Keys");
    morphWeights.resize(targetCount, 0.0);

    if (iMorphKeys.isValid() && morphKeyCount > 0) {
        QModelIndex iKey = iMorphKeys.child(0, 0);
        if (iKey.isValid()) {
            if (morphKeyType == 4) { // NiMorphKey
                QModelIndex iWeights = nif->getIndex(iKey, "Weights 1");
                if (iWeights.isValid() && nif->rowCount(iWeights) == targetCount) {
                    for (uint w = 0; w < targetCount; w++) {
                        morphWeights[w] = nif->get<float>(iWeights.child(w, 0));
                    }
                }
            }
            else if (morphKeyType == 5) { // NiBaryMorphKey
                QModelIndex iWeights1 = nif->getIndex(iKey, "Weights 1");
                if (iWeights1.isValid() && nif->rowCount(iWeights1) == targetCount) {
                    for (uint w = 0; w < targetCount; w++) {
                        morphWeights[w] = nif->get<float>(iWeights1.child(w, 0));
                    }
                }
            }
        }
    }

    // Create glTF morph targets (stored as position deltas)
    for (size_t t = 0; t < morphTargets.size(); t++) {
        std::vector<unsigned char> morphData;

        // Calculate position deltas and bounds
        float minDelta[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float maxDelta[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

        for (int v = 0; v < vertsPerTarget; v++) {
            // Convert to Y-up and apply scale
            Eigen::Vector3d delta = morphTargets[t][v].toYUp();
            delta -= baseVerts[v].toYUp();

            // Scale by NIF_TO_METERS
            delta *= NIF_TO_METERS;

            // Track bounds
            for (int i = 0; i < 3; i++) {
                float val = static_cast<float>(delta[i]);
                minDelta[i] = std::min(minDelta[i], val);
                maxDelta[i] = std::max(maxDelta[i], val);
            }

            // Pack delta (float32)
            for (int i = 0; i < 3; i++) {
                float val = static_cast<float>(delta[i]);
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&val);
                morphData.insert(morphData.end(), bytes, bytes + sizeof(float));
            }
        }

        // Create buffer for morph target
        int morphBufferIdx = AddBufferData(ctx, morphData);

        // Create buffer view and accessor
        int morphViewIdx = AddBufferView(ctx, morphBufferIdx, 0, morphData.size(), TINYGLTF_TARGET_ARRAY_BUFFER);
        int morphAccIdx = AddAccessor(ctx, morphViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                      vertsPerTarget, TINYGLTF_TYPE_VEC3,
                                      {minDelta[0], minDelta[1], minDelta[2]},
                                      {maxDelta[0], maxDelta[1], maxDelta[2]});

        // Add morph target to the primitive
        std::map<std::string, int> morphTarget;
        morphTarget["POSITION"] = morphAccIdx;

        // Add to the first primitive (Ni3dsMorphShape only has one primitive)
        if (!mesh.primitives.empty()) {
            mesh.primitives[0].targets.push_back(morphTarget);
        }
    }

    // Set morph target names and initial weights
    if (!mesh.primitives.empty()) {
        mesh.weights = morphWeights;

        // Store morph target names in mesh extras
        Value morphExtras(Value::Object{});
        Value namesArray(Value::Array{});
        for (const auto& name : morphTargetNames) {
            namesArray.Get<Value::Array>().push_back(Value(name));
        }
        morphExtras.Get<Value::Object>()["targetNames"] = namesArray;
        mesh.extras = morphExtras;
    }

    qInfo(nsIo) << "Exported" << morphTargets.size() << "morph targets for mesh"
                << QString::fromStdString(mesh.name);

    // EXPORT ANIMATIONS: Create separate animation for each morph target
    if (morphKeyCount > 0) {

        // Process animation for each target as a separate animation
        for (uint t = 0; t < targetCount; t++) {
            ProcessMorphTargetAnimation(nif, iBlock, t, targetCount, morphTargetNames[t], ctx);
        }

    }
}

// Process mesh geometry
int ProcessMesh(const NifModel* nif, const QModelIndex& iBlock, GLTF_ExportContext& ctx)
{
    auto blockName = nif->get<QString>(iBlock, "Name").toStdString();

    // Get mesh data
    QVector<Vector3> verts = nif->getArray<Vector3>(iBlock, "Vertices");
    QVector<Vector3> norms = nif->getArray<Vector3>(iBlock, "Normals");
    QVector<Triangle> triangles = nif->getArray<Triangle>(iBlock, "Triangles");

    if (verts.isEmpty() || triangles.isEmpty()) {
        return -1;
    }

    // CRITICAL: Process textures BEFORE creating material
    ProcessTextures(nif, iBlock, ctx);

    // Get UVs
    QVector<Vector3> uvs;
    QModelIndex uvcoord = nif->getIndex(iBlock, "UV Sets");
    if (!uvcoord.isValid()) {
        uvcoord = nif->getIndex(iBlock, "UV Sets 2");
    }
    if (uvcoord.isValid()) {
        uvs = nif->getArray<Vector3>(uvcoord);
    }

    // Convert vertices to Y-up and pack into buffer
    std::vector<unsigned char> vertexData;
    std::vector<unsigned char> normalData;
    std::vector<unsigned char> uvData;
    std::vector<unsigned char> indexData;

    // FIX: Initialize bounds with the FIRST TRANSFORMED vertex
    auto firstVert = verts[0].toYUp();
    firstVert *= NIF_TO_METERS;
    float minPos[3] = {
        static_cast<float>(firstVert[0]),
        static_cast<float>(firstVert[1]),
        static_cast<float>(firstVert[2])
    };
    float maxPos[3] = {
        static_cast<float>(firstVert[0]),
        static_cast<float>(firstVert[1]),
        static_cast<float>(firstVert[2])
    };

    for (const auto& v : verts) {
        auto vUp = v.toYUp();

        // Apply scale conversion: NIF units -> meters
        vUp *= NIF_TO_METERS;

        // Track bounds (in meters) - NOW CORRECTLY COMPARING TRANSFORMED VALUES
        for (int i = 0; i < 3; i++) {
            float val = static_cast<float>(vUp[i]);
            minPos[i] = std::min(minPos[i], val);
            maxPos[i] = std::max(maxPos[i], val);
        }

        // Pack position (float32)
        for (int i = 0; i < 3; i++) {
            float val = static_cast<float>(vUp[i]);
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&val);
            vertexData.insert(vertexData.end(), bytes, bytes + sizeof(float));
        }
    }

    // Pack normals (normals are unit vectors, no scale needed)
    if (!norms.isEmpty() && norms.size() == verts.size()) {
        for (const auto& n : norms) {
            // Transform normal using the same Y-up conversion as vertices
            auto nUp = n.toYUp();

            // Normalize to ensure unit length after transformation
            double length = std::sqrt(nUp[0] * nUp[0] + nUp[1] * nUp[1] + nUp[2] * nUp[2]);
            if (length > 0.0001) {
                nUp[0] /= length;
                nUp[1] /= length;
                nUp[2] /= length;
            }

            for (int i = 0; i < 3; i++) {
                float val = static_cast<float>(nUp[i]);
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&val);
                normalData.insert(normalData.end(), bytes, bytes + sizeof(float));
            }
        }
    }

    // Pack UVs (no scale needed)
    if (!uvs.isEmpty() && uvs.size() == verts.size()) {
        for (const auto& uv : uvs) {
            float u = uv[0];
            float v = uv[1];

            const unsigned char* uBytes = reinterpret_cast<const unsigned char*>(&u);
            const unsigned char* vBytes = reinterpret_cast<const unsigned char*>(&v);
            uvData.insert(uvData.end(), uBytes, uBytes + sizeof(float));
            uvData.insert(uvData.end(), vBytes, vBytes + sizeof(float));
        }
    }

    // Pack indices (uint16)
    for (const auto& tri : triangles) {
        for (int i = 0; i < 3; i++) {
            uint16_t idx = tri[i];
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&idx);
            indexData.insert(indexData.end(), bytes, bytes + sizeof(uint16_t));
        }
    }

    // Create buffer
    std::vector<unsigned char> bufferData;
    size_t vertexOffset = 0;
    size_t normalOffset = vertexData.size();
    size_t uvOffset = normalOffset + normalData.size();
    size_t indexOffset = uvOffset + uvData.size();

    bufferData.insert(bufferData.end(), vertexData.begin(), vertexData.end());
    bufferData.insert(bufferData.end(), normalData.begin(), normalData.end());
    bufferData.insert(bufferData.end(), uvData.begin(), uvData.end());
    bufferData.insert(bufferData.end(), indexData.begin(), indexData.end());

    int bufferIdx = AddBufferData(ctx, bufferData);

    // Create buffer views and accessors
    int vertexViewIdx = AddBufferView(ctx, bufferIdx, vertexOffset, vertexData.size(), TINYGLTF_TARGET_ARRAY_BUFFER);
    int vertexAccIdx = AddAccessor(ctx, vertexViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT, verts.size(), TINYGLTF_TYPE_VEC3,
                                   {minPos[0], minPos[1], minPos[2]}, {maxPos[0], maxPos[1], maxPos[2]});

    int normalAccIdx = -1;
    if (!normalData.empty()) {
        int normalViewIdx = AddBufferView(ctx, bufferIdx, normalOffset, normalData.size(), TINYGLTF_TARGET_ARRAY_BUFFER);
        normalAccIdx = AddAccessor(ctx, normalViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT, norms.size(), TINYGLTF_TYPE_VEC3);
    }

    int uvAccIdx = -1;
    if (!uvData.empty()) {
        int uvViewIdx = AddBufferView(ctx, bufferIdx, uvOffset, uvData.size(), TINYGLTF_TARGET_ARRAY_BUFFER);
        uvAccIdx = AddAccessor(ctx, uvViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT, uvs.size(), TINYGLTF_TYPE_VEC2);
    }

    int indexViewIdx = AddBufferView(ctx, bufferIdx, indexOffset, indexData.size(), TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
    int indexAccIdx = AddAccessor(ctx, indexViewIdx, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, triangles.size() * 3, TINYGLTF_TYPE_SCALAR);

    // Create primitive
    Primitive primitive;
    primitive.attributes["POSITION"] = vertexAccIdx;
    if (normalAccIdx >= 0) primitive.attributes["NORMAL"] = normalAccIdx;
    if (uvAccIdx >= 0) primitive.attributes["TEXCOORD_0"] = uvAccIdx;
    primitive.indices = indexAccIdx;
    primitive.mode = TINYGLTF_MODE_TRIANGLES;

    // Assign material (now textures are already processed)
    int materialIdx = CreateMaterial(nif, iBlock, ctx);
    primitive.material = materialIdx;

    // Create mesh
    Mesh mesh;
    mesh.name = blockName;
    mesh.primitives.push_back(primitive);

    int meshIdx = ctx.model.meshes.size();
    ctx.model.meshes.push_back(mesh);
    ctx.meshMap[nif->getBlockNumber(iBlock)] = meshIdx;

    // MORPH TARGET SUPPORT: Process morph targets if this is a Ni3dsMorphShape
    if (nif->isNiBlock(iBlock, "Ni3dsMorphShape")) {
        ProcessMorphTargets(nif, iBlock, meshIdx, ctx);
    }

    return meshIdx;
}

// Process animation data with QUATERNION rotations
void ProcessAnimations(const NifModel* nif, const QModelIndex& iBlock, int nodeIdx, GLTF_ExportContext& ctx)
{
    // Create animation if it doesn't exist
    if (ctx.animationIndex < 0) {
        Animation anim;
        anim.name = "Take 001";
        ctx.animationIndex = ctx.model.animations.size();
        ctx.model.animations.push_back(anim);
    }

    Animation& anim = ctx.model.animations[ctx.animationIndex];

    // Process Translation animations
    auto iTranslations = nif->getIndex(iBlock, "Translations");
    QModelIndex tkeys = nif->getIndex(iTranslations, "Keys");

    if (tkeys.isValid()) {
        auto translationKeyType = nif->get<uint>(iTranslations, "Interpolation");

        std::vector<float> times;
        std::vector<float> translations;  // x, y, z

        for (int tindex = 0; tindex < nif->rowCount(tkeys); tindex++) {
            QModelIndex tkey = tkeys.child(tindex, 0);
            Vector3 trans = nif->get<Vector3>(tkey, "Value");
            float keyTime = nif->get<float>(tkey, "Time");

            times.push_back(keyTime);

            // Convert from Z-up to Y-up and apply scale
            auto transYUp = trans.toYUp();
            translations.push_back(transYUp[0] * NIF_TO_METERS);  // x
            translations.push_back(transYUp[1] * NIF_TO_METERS);  // y
            translations.push_back(transYUp[2] * NIF_TO_METERS);  // z

            // Handle Bezier interpolation by sampling
            if (translationKeyType == 2 && tindex + 1 < nif->rowCount(tkeys)) { // 2 = Bezier
                QModelIndex nextKey = tkeys.child(tindex + 1, 0);
                float nextTime = nif->get<float>(nextKey, "Time");
                float deltaTime = nextTime - keyTime;

                Vector3 outTan = nif->get<Vector3>(tkey, "OutTan");
                Vector3 m_A = nif->get<Vector3>(tkey, "m_A");
                Vector3 m_B = nif->get<Vector3>(tkey, "m_B");
                Vector3 currentPos = nif->get<Vector3>(tkey, "Value");

                // Sample at 30fps
                const float sampleInterval = 1.0f / 30.0f;
                int numSamples = std::max(1, static_cast<int>(std::ceil(deltaTime / sampleInterval)));

                for (int s = 1; s < numSamples; s++) {
                    float t = static_cast<float>(s) / static_cast<float>(numSamples);
                    if (t >= 1.0f) break;

                    float sampleTime = keyTime + (t * deltaTime);

                    // Bezier interpolation: P(t) = P0 + (OutTan + (A + B*t)*t)*t
                    Vector3 interpPos = currentPos + (outTan + (m_A + m_B * t) * t) * t;
                    auto interpPosYUp = interpPos.toYUp();

                    times.push_back(sampleTime);
                    translations.push_back(interpPosYUp[0] * NIF_TO_METERS);
                    translations.push_back(interpPosYUp[1] * NIF_TO_METERS);
                    translations.push_back(interpPosYUp[2] * NIF_TO_METERS);
                }
            }
        }

        if (!times.empty()) {
            // Verify data integrity
            if (translations.size() != times.size() * 3) {
                qCCritical(nsIo) << "Translation animation data mismatch: "
                                 << times.size() << " keyframes but "
                                 << (translations.size() / 3) << " values";
                return;
            }

            // Pack translation animation data
            std::vector<unsigned char> timeData;
            std::vector<unsigned char> translationData;

            for (float t : times) {
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&t);
                timeData.insert(timeData.end(), bytes, bytes + sizeof(float));
            }

            for (float tr : translations) {
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&tr);
                translationData.insert(translationData.end(), bytes, bytes + sizeof(float));
            }

            // Create buffer
            std::vector<unsigned char> animBufferData;
            size_t timeOffset = 0;
            size_t translationOffset = timeData.size();

            animBufferData.insert(animBufferData.end(), timeData.begin(), timeData.end());
            animBufferData.insert(animBufferData.end(), translationData.begin(), translationData.end());

            int animBufferIdx = AddBufferData(ctx, animBufferData);

            // Create accessors
            int timeViewIdx = AddBufferView(ctx, animBufferIdx, timeOffset, timeData.size());
            int timeAccIdx = AddAccessor(ctx, timeViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT, times.size(), TINYGLTF_TYPE_SCALAR,
                                         {times.front()}, {times.back()});

            int transViewIdx = AddBufferView(ctx, animBufferIdx, translationOffset, translationData.size());
            int transAccIdx = AddAccessor(ctx, transViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT, times.size(), TINYGLTF_TYPE_VEC3);

            // Create animation sampler
            AnimationSampler sampler;
            sampler.input = timeAccIdx;
            sampler.output = transAccIdx;
            // CRITICAL FIX: Always use LINEAR when we're sampling Bezier curves
            // CUBICSPLINE requires 3x the data (in-tangent, value, out-tangent)
            sampler.interpolation = "LINEAR";

            int samplerIdx = anim.samplers.size();
            anim.samplers.push_back(sampler);

            // Create animation channel
            AnimationChannel channel;
            channel.sampler = samplerIdx;
            channel.target_node = nodeIdx;
            channel.target_path = "translation";

            anim.channels.push_back(channel);
        }
    }

    // Process Rotation animations
    auto iRotations = nif->getIndex(iBlock, "Rotations");
    QModelIndex rkeys = nif->getIndex(iRotations, "Quaternion Keys");

    if (rkeys.isValid()) {
        auto rotationKeyType = nif->get<uint>(iRotations, "Rotation Type");

        std::vector<float> times;
        std::vector<float> rotations;  // QUATERNIONS: x, y, z, w

        Quat prevRot;
        bool hasPrevRot = false;

        for (int rindex = 0; rindex < nif->rowCount(rkeys); rindex++) {
            QModelIndex rkey = rkeys.child(rindex, 0);
            Quat rot = nif->get<Quat>(rkey, "Value");
            float keyTime = nif->get<float>(rkey, "Time");

            // Ensure shortest path (quaternion continuity)
            if (hasPrevRot && Quat::dotproduct(prevRot, rot) < 0.0f) {
                rot[0] = -rot[0];
                rot[1] = -rot[1];
                rot[2] = -rot[2];
                rot[3] = -rot[3];
            }
            prevRot = rot;
            hasPrevRot = true;

            times.push_back(keyTime);

            // Convert from Z-up (NIF) to Y-up (glTF/Unity)
            rotations.push_back(rot[1]);   // x
            rotations.push_back(rot[3]);   // z -> new y
            rotations.push_back(-rot[2]);  // -y -> new z
            rotations.push_back(rot[0]);   // w

            // Handle TCB (Squad) interpolation by sampling
            if (rotationKeyType == 3 && rindex + 1 < nif->rowCount(rkeys)) {
                QModelIndex nextKey = rkeys.child(rindex + 1, 0);
                Quat nextRot = nif->get<Quat>(nextKey, "Value");
                Quat m_A = nif->get<Quat>(rkey, "A");
                Quat m_B = nif->get<Quat>(nextKey, "B");
                float nextTime = nif->get<float>(nextKey, "Time");
                float deltaTime = nextTime - keyTime;

                // Ensure continuity for control quaternions
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

                // Sample at 30fps
                const float sampleInterval = 1.0f / 30.0f;
                int numSamples = std::max(1, static_cast<int>(std::ceil(deltaTime / sampleInterval)));
                Quat prevSampleQuat = rot;

                for (int s = 1; s < numSamples; s++) {
                    float t = static_cast<float>(s) / static_cast<float>(numSamples);
                    if (t >= 1.0f) break;

                    float sampleTime = keyTime + (t * deltaTime);

                    // Squad interpolation
                    Quat interpQuat = Quat::slerp(2.0f * t * (1.0f - t),
                                                  Quat::slerp(t, rot, nextRot),
                                                  Quat::slerp(t, m_A, m_B));

                    // Ensure shortest path
                    if (Quat::dotproduct(prevSampleQuat, interpQuat) < 0.0f) {
                        interpQuat[0] = -interpQuat[0]; interpQuat[1] = -interpQuat[1];
                        interpQuat[2] = -interpQuat[2]; interpQuat[3] = -interpQuat[3];
                    }
                    prevSampleQuat = interpQuat;

                    times.push_back(sampleTime);
                    // Apply Z-up to Y-up transformation
                    rotations.push_back(interpQuat[1]);   // x
                    rotations.push_back(interpQuat[3]);   // z -> new y
                    rotations.push_back(-interpQuat[2]);  // -y -> new z
                    rotations.push_back(interpQuat[0]);   // w
                }
            }
        }

        if (!times.empty()) {
            // Pack animation data
            std::vector<unsigned char> timeData;
            std::vector<unsigned char> rotationData;

            for (float t : times) {
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&t);
                timeData.insert(timeData.end(), bytes, bytes + sizeof(float));
            }

            for (float r : rotations) {
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&r);
                rotationData.insert(rotationData.end(), bytes, bytes + sizeof(float));
            }

            // Create buffer
            std::vector<unsigned char> animBufferData;
            size_t timeOffset = 0;
            size_t rotationOffset = timeData.size();

            animBufferData.insert(animBufferData.end(), timeData.begin(), timeData.end());
            animBufferData.insert(animBufferData.end(), rotationData.begin(), rotationData.end());

            int animBufferIdx = AddBufferData(ctx, animBufferData);

            // Create accessors
            int timeViewIdx = AddBufferView(ctx, animBufferIdx, timeOffset, timeData.size());
            int timeAccIdx = AddAccessor(ctx, timeViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT, times.size(), TINYGLTF_TYPE_SCALAR,
                                         {times.front()}, {times.back()});

            int rotViewIdx = AddBufferView(ctx, animBufferIdx, rotationOffset, rotationData.size());
            int rotAccIdx = AddAccessor(ctx, rotViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT, times.size(), TINYGLTF_TYPE_VEC4);

            // Create animation sampler
            AnimationSampler sampler;
            sampler.input = timeAccIdx;
            sampler.output = rotAccIdx;
            sampler.interpolation = "LINEAR";

            int samplerIdx = anim.samplers.size();
            anim.samplers.push_back(sampler);

            // Create animation channel
            AnimationChannel channel;
            channel.sampler = samplerIdx;
            channel.target_node = nodeIdx;
            channel.target_path = "rotation";  // QUATERNION ROTATION!

            anim.channels.push_back(channel);
        }
    }

    // Process Scale animations
    auto iScales = nif->getIndex(iBlock, "Scales");
    QModelIndex skeys = nif->getIndex(iScales, "Keys");

    if (skeys.isValid()) {
        std::vector<float> times;
        std::vector<float> scales;

        for (int sindex = 0; sindex < nif->rowCount(skeys); sindex++) {
            QModelIndex skey = skeys.child(sindex, 0);
            float scale = nif->get<float>(skey, "Value");
            float keyTime = nif->get<float>(skey, "Time");

            times.push_back(keyTime);
            // glTF scale is a vec3 (uniform scaling)
            scales.push_back(scale);
            scales.push_back(scale);
            scales.push_back(scale);
        }

        if (!times.empty()) {
            // Pack scale animation data
            std::vector<unsigned char> timeData;
            std::vector<unsigned char> scaleData;

            for (float t : times) {
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&t);
                timeData.insert(timeData.end(), bytes, bytes + sizeof(float));
            }

            for (float s : scales) {
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&s);
                scaleData.insert(scaleData.end(), bytes, bytes + sizeof(float));
            }

            // Create buffer
            std::vector<unsigned char> animBufferData;
            size_t timeOffset = 0;
            size_t scaleOffset = timeData.size();

            animBufferData.insert(animBufferData.end(), timeData.begin(), timeData.end());
            animBufferData.insert(animBufferData.end(), scaleData.begin(), scaleData.end());

            int animBufferIdx = AddBufferData(ctx, animBufferData);

            // Create accessors
            int timeViewIdx = AddBufferView(ctx, animBufferIdx, timeOffset, timeData.size());
            int timeAccIdx = AddAccessor(ctx, timeViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT, times.size(), TINYGLTF_TYPE_SCALAR,
                                         {times.front()}, {times.back()});

            int scaleViewIdx = AddBufferView(ctx, animBufferIdx, scaleOffset, scaleData.size());
            int scaleAccIdx = AddAccessor(ctx, scaleViewIdx, TINYGLTF_COMPONENT_TYPE_FLOAT, times.size(), TINYGLTF_TYPE_VEC3);

            // Create animation sampler
            AnimationSampler sampler;
            sampler.input = timeAccIdx;
            sampler.output = scaleAccIdx;
            sampler.interpolation = "LINEAR";

            int samplerIdx = anim.samplers.size();
            anim.samplers.push_back(sampler);

            // Create animation channel
            AnimationChannel channel;
            channel.sampler = samplerIdx;
            channel.target_node = nodeIdx;
            channel.target_path = "scale";

            anim.channels.push_back(channel);
        }
    }
}

// Process camera and add to glTF model
int ProcessCamera(const NifModel* nif, const QModelIndex& iBlock, GLTF_ExportContext& ctx)
{
    if (!nif->isNiBlock(iBlock, "NiCamera")) {
        return -1;
    }

    // Get camera parameters from NIF
    float frustumNear = nif->get<float>(iBlock, "Frustum Near");
    float frustumFar = nif->get<float>(iBlock, "Frustum Far");
    float frustumLeft = nif->get<float>(iBlock, "Frustum Left");
    float frustumRight = nif->get<float>(iBlock, "Frustum Right");
    float frustumTop = nif->get<float>(iBlock, "Frustum Top");
    float frustumBottom = nif->get<float>(iBlock, "Frustum Bottom");

    // Create glTF camera
    Camera camera;
    camera.name = nif->get<QString>(iBlock, "Name").toStdString();
    if (camera.name.empty()) {
        camera.name = "Camera_" + std::to_string(nif->getBlockNumber(iBlock));
    }

    // glTF cameras can be perspective or orthographic
    // NIF cameras with frustum parameters are typically perspective
    camera.type = "perspective";

    // Calculate field of view from frustum dimensions
    // For a symmetric frustum: tan(fov/2) = top / near
    float fovY = 2.0f * std::atan((frustumTop - frustumBottom) / (2.0f * frustumNear));
    float aspectRatio = (frustumRight - frustumLeft) / (frustumTop - frustumBottom);

    // Set perspective camera parameters
    // Note: glTF uses radians for FOV
    camera.perspective.yfov = fovY;
    camera.perspective.aspectRatio = aspectRatio;
    camera.perspective.znear = frustumNear * NIF_TO_METERS;  // Apply scale
    camera.perspective.zfar = frustumFar * NIF_TO_METERS;    // Apply scale

    int cameraIdx = ctx.model.cameras.size();
    ctx.model.cameras.push_back(camera);

    qInfo(nsIo) << "Exported camera" << QString::fromStdString(camera.name)
                << "as camera index" << cameraIdx;

    return cameraIdx;
}

// Process billboards and add custom extension data
void ProcessBillboards(GLTF_ExportContext& ctx)
{
    if (ctx.billboardNodes.empty() || ctx.cameraNodeIdx < 0) {
        return;
    }

    // Add billboard constraint information to each billboard node
    for (int nodeIdx : ctx.billboardNodes) {
        if (nodeIdx >= 0 && nodeIdx < ctx.model.nodes.size()) {
            Node& node = ctx.model.nodes[nodeIdx];

            // Store billboard information in extras (JSON object)
            // This allows Unity/other engines to implement billboard behavior
            // Build the nested structure manually using Get() to access the map
            Value billboardData(Value::Object{});
            billboardData.Get<Value::Object>()["targetCamera"] = Value(ctx.cameraNodeIdx);
            billboardData.Get<Value::Object>()["type"] = Value("screen_aligned");

            Value extras(Value::Object{});
            extras.Get<Value::Object>()["NifBillboard"] = billboardData;

            node.extras = extras;

            qInfo(nsIo) << "Marked node" << nodeIdx << "as billboard targeting camera node" << ctx.cameraNodeIdx;
        }
    }
}

// Process node hierarchy recursively
int ProcessNode(const NifModel* nif, const QModelIndex& iBlock, int parentNodeIdx, GLTF_ExportContext& ctx)
{
    auto blockName = nif->get<QString>(iBlock, "Name").toStdString();
    auto t = Transform(nif, iBlock);

    Node node;
    node.name = blockName;

    // Set transform - glTF uses Y-up and meters
    auto pos = t.translation.toYUp();
    node.translation = {
        pos.x() * NIF_TO_METERS,
        pos.y() * NIF_TO_METERS,
        pos.z() * NIF_TO_METERS
    };

    // CRITICAL: Convert rotation from Z-up (NIF) to Y-up (glTF/Unity)
    Quat rot = t.rotation.toQuat();

    // NIF quaternion (Z-up): w, x, y, z
    // glTF quaternion (Y-up): w, x, z, -y (swap Y and Z, negate new Z)
    node.rotation = {
        rot[1],   // x stays the same
        rot[3],   // z becomes new y
        -rot[2],  // -y becomes new z
        rot[0]    // w stays the same
    };

    // Scale is unitless, no conversion needed
    node.scale = {t.scale, t.scale, t.scale};

    int nodeIdx = ctx.model.nodes.size();
    ctx.model.nodes.push_back(node);
    ctx.nodeMap[nif->getBlockNumber(iBlock)] = nodeIdx;

    // BILLBOARD DETECTION: Track NiBillboardNode that aren't animated
    // Only apply billboards to nodes which aren't animated (same logic as FBX export)
    if (nif->isNiBlock(iBlock, "NiBillboardNode") &&
        !nif->isNiBlock(iBlock, "Ni3dsAnimationNode")) {
        ctx.billboardNodes.push_back(nodeIdx);
        qInfo(nsIo) << "Detected billboard node:" << QString::fromStdString(blockName) << "(index" << nodeIdx << ")";
    }

    // Link to parent
    if (parentNodeIdx >= 0) {
        ctx.model.nodes[parentNodeIdx].children.push_back(nodeIdx);
    }

    // Process animations
    if (nif->isNiBlock(iBlock, "Ni3dsAnimationNode") || nif->isNiBlock(iBlock, "Ni3dsBone")) {
        ProcessAnimations(nif, iBlock, nodeIdx, ctx);
    }

    // Process children
    foreach(const int l, nif->getChildLinks(nif->getBlockNumber(iBlock))) {
        QModelIndex iChild = nif->getBlock(l);

        if (nif->isNiBlock(iChild, "NiNode") || nif->inherits(iChild, "NiNode")) {
            ProcessNode(nif, iChild, nodeIdx, ctx);
        }
        else if (nif->inherits(iChild, "NiTriShape")
                 || nif->itemName(iChild) == "NiTriShape"
                 || nif->isNiBlock(iChild, "Ni3dsMorphShape")) {  // ADD THIS LINE
            int meshIdx = ProcessMesh(nif, iChild, ctx);
            if (meshIdx >= 0) {
                ctx.model.nodes[nodeIdx].mesh = meshIdx;
            }
        }
        else if (nif->isNiBlock(iChild, "NiCamera")) {
            // Process camera and attach to this node
            int cameraIdx = ProcessCamera(nif, iChild, ctx);
            if (cameraIdx >= 0) {
                // Apply camera rotation adjustment for Y-up coordinate system
                // NIF cameras look down +Y (forward), glTF cameras look down -Z
                // Apply -90° rotation around X axis

                // Override the rotation for camera nodes
                // glTF cameras look down -Z, up is +Y
                // This is a -90° rotation around X axis: quaternion(x, y, z, w)
                ctx.model.nodes[nodeIdx].rotation = {
                    -0.7071067811865475,  // x = sin(-90°/2)
                    0.0,                  // y
                    0.0,                  // z
                    0.7071067811865476    // w = cos(-90°/2)
                };

                // Assign camera to node
                ctx.model.nodes[nodeIdx].camera = cameraIdx;

                // Track camera node for billboard targeting
                ctx.cameraNodeIdx = nodeIdx;

                qInfo(nsIo) << "Attached camera index" << cameraIdx
                            << "to node" << nodeIdx;
            }
        }
    }

    return nodeIdx;
}

// Main export function
void exportGLTF(const NifModel* nif, const QModelIndex& index)
{
    Q_UNUSED(index);

    QString exportPath = QFileDialog::getSaveFileName(
        qApp->activeWindow(),
        tr("Export glTF"),
        QString(),
        tr("glTF Binary (*.glb);;glTF JSON (*.gltf)")
        );

    if (exportPath.isEmpty()) {
        return;
    }

    GLTF_ExportContext ctx;
    ctx.exportPath = exportPath.toStdString();

    ctx.model.asset.version = "2.0";
    ctx.model.asset.generator = "NifSkope glTF Exporter";

    QModelIndex iRoot = FindSceneRoot(nif, QModelIndex());
    if (!iRoot.isValid()) {
        auto links = nif->getRootLinks();
        foreach(int l, links) {
            QModelIndex iChild = nif->getBlock(l);
            if (nif->inherits(iChild, "NiNode")) {
                iRoot = iChild;
                break;
            }
        }
    }

    if (!iRoot.isValid()) {
        qCCritical(nsIo) << "No valid root node found for glTF export";
        return;
    }

    int rootNodeIdx = ProcessNode(nif, iRoot, -1, ctx);

    ProcessBillboards(ctx);

    Scene scene;
    scene.name = "Scene";
    scene.nodes.push_back(rootNodeIdx);

    ctx.model.scenes.push_back(scene);
    ctx.model.defaultScene = 0;

    bool binary = exportPath.endsWith(".glb", Qt::CaseInsensitive);

    if (!binary && !ctx.model.buffers.empty()) {
        QFileInfo fileInfo(exportPath);
        QString binFileName = fileInfo.completeBaseName() + ".bin";

        // Set all buffers to use the same external .bin file
        for (auto& buffer : ctx.model.buffers) {
            buffer.uri = binFileName.toStdString();
        }
    }

    TinyGLTF writer;

    // - GLB (binary): embedBuffers = true (embed in binary chunk)
    // - glTF JSON: embedBuffers = false (write external .bin file)
    bool embedBuffers = binary;  // Only embed for GLB format

    // Parameters:
    // - embedImages: true (embed textures in GLB or as data URIs)
    // - embedBuffers: true for GLB, false for glTF+bin
    // - prettyPrint: true for readable JSON
    // - writeBinary: true for GLB, false for glTF
    bool success = writer.WriteGltfSceneToFile(
        &ctx.model,
        ctx.exportPath,
        true,          // embedImages - textures embedded
        embedBuffers,  // embedBuffers - false for JSON, true for GLB
        true,          // prettyPrint - readable JSON if not binary
        binary         // writeBinary - GLB format
        );

    if (success) {
        // Report billboard export
        if (!ctx.billboardNodes.empty()) {
            qInfo(nsIo) << "Exported" << ctx.billboardNodes.size() << "billboard nodes";
        }

        qInfo(nsIo) << "Successfully exported glTF to" << exportPath;
        if (!binary) {
            QFileInfo fileInfo(exportPath);
            QString binPath = fileInfo.absolutePath() + "/" + fileInfo.completeBaseName() + ".bin";
            qInfo(nsIo) << "Binary data written to" << binPath;
        }
    } else {
        qCCritical(nsIo) << "Failed to write glTF file to" << exportPath;
    }
}
