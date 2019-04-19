// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <limits>

#include "GltfMeshData.h"
#include <Magnum/MeshTools/Compile.h>
#include <Magnum/Math/Vector3.h>

namespace esp {
namespace assets {
GltfMeshData::GltfMeshData() : BaseMesh(SupportedMeshType::GLTF_MESH) {
  // bounding box
  bounding_box_coords_[0][0] = std::numeric_limits<float>::max();
  bounding_box_coords_[0][1] = std::numeric_limits<float>::max();
  bounding_box_coords_[0][2] = std::numeric_limits<float>::max();
  
  bounding_box_coords_[1][0] = std::numeric_limits<float>::lowest();
  bounding_box_coords_[1][1] = std::numeric_limits<float>::lowest();
  bounding_box_coords_[1][2] = std::numeric_limits<float>::lowest();
}

void GltfMeshData::uploadBuffersToGPU(bool forceReload) {
  if (forceReload) {
    buffersOnGPU_ = false;
  }
  if (buffersOnGPU_) {
    return;
  }

  renderingBuffer_.reset();
  renderingBuffer_ = std::make_unique<GltfMeshData::RenderingBuffer>();
  // position, normals, uv, colors are bound to corresponding attributes
  renderingBuffer_->mesh = Magnum::MeshTools::compile(*meshData_);

  buffersOnGPU_ = true;
}

Magnum::GL::Mesh* GltfMeshData::getMagnumGLMesh() {
  if (renderingBuffer_ == nullptr) {
    return nullptr;
  }

  return &(renderingBuffer_->mesh);
}

void GltfMeshData::setMeshData(Magnum::Trade::AbstractImporter& importer,
                               int meshID) {
  ASSERT(0 <= meshID && meshID < importer.mesh3DCount());
  meshData_ = importer.mesh3D(meshID);

  // bounding box
  bounding_box_coords_[0][0] = 
    bounding_box_coords_[0][1] = 
    bounding_box_coords_[0][2] = std::numeric_limits<float>::max();
  bounding_box_coords_[1][0] = 
    bounding_box_coords_[1][1] = 
    bounding_box_coords_[1][2] = std::numeric_limits<float>::lowest();
  const auto &indices = meshData_->indices();
  for (const auto &idx: indices) {
    const auto &positions = meshData_->positions(idx);
    for (const auto &position: positions) {
      bounding_box_coords_[0][0] = std::min(bounding_box_coords_[0][0], position.x());
      bounding_box_coords_[0][1] = std::min(bounding_box_coords_[0][1], position.y());
      bounding_box_coords_[0][2] = std::min(bounding_box_coords_[0][2], position.z());
      
      bounding_box_coords_[1][0] = std::max(bounding_box_coords_[1][0], position.x());
      bounding_box_coords_[1][1] = std::max(bounding_box_coords_[1][1], position.y());
      bounding_box_coords_[1][2] = std::max(bounding_box_coords_[1][2], position.z());
    }
    // TODO: handing indexed meshes
    break;
  }
}

}  // namespace assets
}  // namespace esp
