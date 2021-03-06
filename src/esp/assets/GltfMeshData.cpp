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
  
  size_t numPoints = 0;
  for (size_t idx = 0, len = meshData_->positionArrayCount(); idx < len; idx++) {
    const auto &positions = meshData_->positions(idx);
    numPoints += positions.size();
  }

  pointCloud_ = Eigen::Matrix3Xf(3, numPoints);
  size_t pointIdx = 0;

  for (size_t idx = 0, len = meshData_->positionArrayCount(); idx < len; idx++) {
    const auto &positions = meshData_->positions(idx);
    LOG(INFO) << "idx: " << idx << " positions: " << positions.size() << std::endl;
    
    for (const auto &position: positions) {
      pointCloud_(0, pointIdx) = position.x();
      pointCloud_(1, pointIdx) = position.y();
      pointCloud_(2, pointIdx) = position.z();
      pointIdx++;

      bounding_box_coords_[0][0] = std::min({bounding_box_coords_[0][0], position.x()});
      bounding_box_coords_[0][1] = std::min({bounding_box_coords_[0][1], position.y()});
      bounding_box_coords_[0][2] = std::min({bounding_box_coords_[0][2], position.z()});
      
      bounding_box_coords_[1][0] = std::max({bounding_box_coords_[1][0], position.x()});
      bounding_box_coords_[1][1] = std::max({bounding_box_coords_[1][1], position.y()});
      bounding_box_coords_[1][2] = std::max({bounding_box_coords_[1][2], position.z()});
    }
  }

  LOG(INFO) << "Mesh Bounding box coords: ("
            << bounding_box_coords_[0][0] << ", " 
            << bounding_box_coords_[0][1] << ", " 
            << bounding_box_coords_[0][2] << "), (" 
            << bounding_box_coords_[1][0] << ", " 
            << bounding_box_coords_[1][1] << ", " 
            << bounding_box_coords_[1][2] << ") "
            << std::endl;
}

}  // namespace assets
}  // namespace esp
