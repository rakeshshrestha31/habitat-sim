// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "Mp3dInstanceMeshData.h"

#include <fstream>
#include <sstream>
#include <vector>
#include <limits>

#include <sophus/so3.hpp>

#include "esp/core/esp.h"
#include "esp/geo/geo.h"
#include "esp/io/io.h"

namespace esp {
namespace assets {

bool Mp3dInstanceMeshData::loadMp3dPLY(const std::string& plyFile) {
  std::ifstream ifs(plyFile);
  if (!ifs.good()) {
    LOG(ERROR) << "Cannot open file at " << plyFile;
    return false;
  }

  std::string line, token;
  std::istringstream iss;
  std::getline(ifs, line);
  if (line != "ply") {
    LOG(ERROR) << "Invalid ply file header";
    return false;
  }
  std::getline(ifs, line);
  if (line != "format binary_little_endian 1.0") {
    LOG(ERROR) << "Invalid ply file header";
    return false;
  }

  // element vertex nVertex
  std::getline(ifs, line);
  iss.str(line);
  iss >> token;
  if (token != "element") {
    LOG(ERROR) << "Invalid element vertex header line";
    return false;
  }
  int nVertex;
  iss >> token >> nVertex;

  // we know the header is fixed so skip until face count
  do {
    std::getline(ifs, line);
  } while ((line.substr(0, 12) != "element face") && !ifs.eof());
  std::stringstream iss2;
  iss2.str(line);
  iss2 >> token;
  if (token != "element") {
    LOG(ERROR) << "Invalid element face header line";
    return false;
  }
  int nFace;
  iss2 >> token >> nFace;

  // ignore rest of header
  do {
    std::getline(ifs, line);
  } while ((line != "end_header") && !ifs.eof());

  cpu_cbo_.clear();
  cpu_cbo_.reserve(nVertex);
  cpu_vbo_.clear();
  cpu_vbo_.reserve(nVertex);
  cpu_ibo_.clear();
  cpu_ibo_.reserve(nFace);
  
  bounding_box_coords_[0][0] = 
    bounding_box_coords_[0][1] = 
    bounding_box_coords_[0][2] = std::numeric_limits<float>::max();
  bounding_box_coords_[1][0] = 
    bounding_box_coords_[1][1] = 
    bounding_box_coords_[1][2] = std::numeric_limits<float>::lowest();

  for (int i = 0; i < nVertex; ++i) {
    vec4f position;
    vec3f normal;
    vec2f texCoords;
    vec3uc rgb;

    ifs.read(reinterpret_cast<char*>(position.data()), 3 * sizeof(float));
    ifs.read(reinterpret_cast<char*>(normal.data()), 3 * sizeof(float));
    ifs.read(reinterpret_cast<char*>(texCoords.data()), 2 * sizeof(float));
    ifs.read(reinterpret_cast<char*>(rgb.data()), 3 * sizeof(uint8_t));
    cpu_vbo_.emplace_back(position);
    cpu_cbo_.emplace_back(rgb);

    // bounding box computation
    bounding_box_coords_[0][0] = std::min(bounding_box_coords_[0][0], position(0));
    bounding_box_coords_[0][1] = std::min(bounding_box_coords_[0][1], position(1));
    bounding_box_coords_[0][2] = std::min(bounding_box_coords_[0][2], position(2));
    
    bounding_box_coords_[1][0] = std::max(bounding_box_coords_[1][0], position(0));
    bounding_box_coords_[1][1] = std::max(bounding_box_coords_[1][1], position(1));
    bounding_box_coords_[1][2] = std::max(bounding_box_coords_[1][2], position(2));
    
    if (i == 0) {
        LOG(INFO) << "Bounding box coords: ("
                  << bounding_box_coords_[0][0] << ", " 
                  << bounding_box_coords_[0][1] << ", " 
                  << bounding_box_coords_[0][2] << "), (" 
                  << bounding_box_coords_[1][0] << ", " 
                  << bounding_box_coords_[1][1] << ", " 
                  << bounding_box_coords_[1][2] << ") ";
    }
  }

  for (int i = 0; i < nFace; ++i) {
    uint8_t nIndices;
    vec3i indices;
    int32_t materialId;
    int32_t segmentId;
    int32_t categoryId;

    ifs.read(reinterpret_cast<char*>(&nIndices), sizeof(nIndices));
    ASSERT(nIndices == 3);
    ifs.read(reinterpret_cast<char*>(indices.data()), 3 * sizeof(int));
    ifs.read(reinterpret_cast<char*>(&materialId), sizeof(materialId));
    ifs.read(reinterpret_cast<char*>(&segmentId), sizeof(segmentId));
    ifs.read(reinterpret_cast<char*>(&categoryId), sizeof(categoryId));
    cpu_ibo_.emplace_back(indices);
    materialIds_.emplace_back(materialId);
    segmentIds_.emplace_back(segmentId);
    categoryIds_.emplace_back(categoryId);

    // also store segmentIds in vertex position[3]
    for (int iVertex : indices) {
      cpu_vbo_[iVertex][3] = static_cast<float>(segmentId);
    }
  }

  return true;
}

Magnum::GL::Mesh* Mp3dInstanceMeshData::getMagnumGLMesh() {
  if (renderingBuffer_ == nullptr) {
    return nullptr;
  }
  return &(renderingBuffer_->mesh);
}

void Mp3dInstanceMeshData::uploadBuffersToGPU(bool forceReload) {
  if (forceReload) {
    buffersOnGPU_ = false;
  }
  if (buffersOnGPU_) {
    return;
  }

  renderingBuffer_.reset();
  renderingBuffer_ = std::make_unique<Mp3dInstanceMeshData::RenderingBuffer>();

  // create uint32 ibo
  const size_t nTris = cpu_ibo_.size();
  std::vector<uint32_t> tri_ibo(nTris * 3);
  for (int iTri = 0; iTri < nTris; ++iTri) {
    const int iBase = 3 * iTri;
    const vec3i& indices = cpu_ibo_[iTri];
    tri_ibo[iBase + 0] = static_cast<uint32_t>(indices[0]);
    tri_ibo[iBase + 1] = static_cast<uint32_t>(indices[1]);
    tri_ibo[iBase + 2] = static_cast<uint32_t>(indices[2]);
  }

  // convert uchar rgb to float rgb
  std::vector<float> cbo_float(cpu_cbo_.size() * 3);
  for (int iVert = 0; iVert < cpu_cbo_.size(); ++iVert) {
    const uint32_t idx = 3 * iVert;
    cbo_float[idx + 0] = cpu_cbo_[iVert][0] / 255.0f;
    cbo_float[idx + 1] = cpu_cbo_[iVert][1] / 255.0f;
    cbo_float[idx + 2] = cpu_cbo_[iVert][2] / 255.0f;
  }
  renderingBuffer_->vbo.setData(cpu_vbo_, Magnum::GL::BufferUsage::StaticDraw);
  renderingBuffer_->cbo.setData(cbo_float, Magnum::GL::BufferUsage::StaticDraw);
  renderingBuffer_->ibo.setData(tri_ibo, Magnum::GL::BufferUsage::StaticDraw);
  renderingBuffer_->mesh.setPrimitive(Magnum::GL::MeshPrimitive::Triangles)
      .setCount(tri_ibo.size())
      .addVertexBuffer(renderingBuffer_->vbo, 0,
                       Magnum::GL::Attribute<0, Magnum::Vector4>{})
      .addVertexBuffer(renderingBuffer_->cbo, 0,
                       Magnum::GL::Attribute<1, Magnum::Color3>{})
      .setIndexBuffer(renderingBuffer_->ibo, 0,
                      Magnum::GL::MeshIndexType::UnsignedInt);

  buffersOnGPU_ = true;
}

bool Mp3dInstanceMeshData::saveSemMeshPLY(
    const std::string& plyFile,
    const std::unordered_map<int, int>& segmentIdToObjectIdMap) {
  const int nVertex = cpu_vbo_.size();
  const int nFace = cpu_ibo_.size();

  std::ofstream f(plyFile, std::ios::out | std::ios::binary);
  f << "ply" << std::endl;
  f << "format binary_little_endian 1.0" << std::endl;
  f << "element vertex " << nVertex << std::endl;
  f << "property float x" << std::endl;
  f << "property float y" << std::endl;
  f << "property float z" << std::endl;
  f << "property uchar red" << std::endl;
  f << "property uchar green" << std::endl;
  f << "property uchar blue" << std::endl;
  f << "element face " << nFace << std::endl;
  f << "property list uchar int vertex_indices" << std::endl;
  f << "property int object_id" << std::endl;
  f << "end_header" << std::endl;

  for (int iVertex = 0; iVertex < nVertex; ++iVertex) {
    const vec3f& xyz = cpu_vbo_[iVertex].head<3>();
    const vec3uc& rgb = cpu_cbo_[iVertex];
    f.write(reinterpret_cast<const char*>(xyz.data()), 3 * sizeof(float));
    f.write(reinterpret_cast<const char*>(rgb.data()), 3 * sizeof(uint8_t));
  }

  for (int iFace = 0; iFace < cpu_ibo_.size(); ++iFace) {
    const uint8_t nIndices = 3;
    const vec3i& indices = cpu_ibo_[iFace];
    const int32_t segmentId = segmentIds_[iFace];
    int32_t objectId = ID_UNDEFINED;
    if (segmentId >= 0) {
      objectId = segmentIdToObjectIdMap.at(segmentId);
    }
    f.write(reinterpret_cast<const char*>(&nIndices), sizeof(nIndices));
    f.write(reinterpret_cast<const char*>(indices.data()),
            3 * sizeof(uint32_t));
    f.write(reinterpret_cast<const char*>(&objectId), sizeof(objectId));
  }
  f.close();

  return true;
}

bool Mp3dInstanceMeshData::loadSemMeshPLY(const std::string& plyFile) {
  std::ifstream ifs(plyFile);
  if (!ifs.good()) {
    LOG(ERROR) << "Cannot open file at " << plyFile;
    return false;
  }

  std::string line, token;
  std::istringstream iss;
  std::getline(ifs, line);
  if (line != "ply") {
    LOG(ERROR) << "Invalid ply file header";
    return false;
  }
  std::getline(ifs, line);
  if (line != "format binary_little_endian 1.0") {
    LOG(ERROR) << "Invalid ply file header";
    return false;
  }

  // element vertex nVertex
  std::getline(ifs, line);
  iss.str(line);
  iss >> token;
  if (token != "element") {
    LOG(ERROR) << "Invalid element vertex header line";
    return false;
  }
  int nVertex;
  iss >> token >> nVertex;

  // we know the header is fixed so skip until face count
  do {
    std::getline(ifs, line);
  } while ((line.substr(0, 12) != "element face") && !ifs.eof());
  std::stringstream iss2;
  iss2.str(line);
  iss2 >> token;
  if (token != "element") {
    LOG(ERROR) << "Invalid element face header line";
    return false;
  }
  int nFace;
  iss2 >> token >> nFace;

  // ignore rest of header
  do {
    std::getline(ifs, line);
  } while ((line != "end_header") && !ifs.eof());

  cpu_cbo_.clear();
  cpu_cbo_.reserve(nVertex);
  cpu_vbo_.clear();
  cpu_vbo_.reserve(nVertex);
  cpu_ibo_.clear();
  cpu_ibo_.reserve(nFace);

  for (int iVertex = 0; iVertex < nVertex; ++iVertex) {
    vec4f position;
    vec3uc rgb;

    ifs.read(reinterpret_cast<char*>(position.data()), 3 * sizeof(float));
    ifs.read(reinterpret_cast<char*>(rgb.data()), 3 * sizeof(uint8_t));
    cpu_vbo_.emplace_back(position);
    cpu_cbo_.emplace_back(rgb);
  }

  for (int iFace = 0; iFace < nFace; ++iFace) {
    uint8_t nIndices;
    vec3i indices;
    int32_t objectId;
    ifs.read(reinterpret_cast<char*>(&nIndices), sizeof(nIndices));
    ASSERT(nIndices == 3);
    ifs.read(reinterpret_cast<char*>(indices.data()), 3 * sizeof(int));
    cpu_ibo_.emplace_back(indices);
    ifs.read(reinterpret_cast<char*>(&objectId), sizeof(objectId));
    // store objectId in position[3] of each vertex
    for (int iVertex : indices) {
      vec4f& position = cpu_vbo_[iVertex];
      position[3] = static_cast<float>(objectId);
    }
  }

  // MP3D semantic PLY meshes have -Z gravity
  const quatf T_esp_scene =
      quatf::FromTwoVectors(-vec3f::UnitZ(), geo::ESP_GRAVITY);

  for (auto& xyzid : cpu_vbo_) {
    const vec3f xyz_scene = xyzid.head<3>();
    const vec3f xyz_esp = T_esp_scene * xyz_scene;
    xyzid.head<3>() = xyz_esp;
  }
  return true;
}

}  // namespace assets
}  // namespace esp
