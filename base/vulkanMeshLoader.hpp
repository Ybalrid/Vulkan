/*
* Simple wrapper for getting an index buffer and vertices out of an assimp mesh
*
* Copyright (C) 2016 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#pragma once

#include <stdlib.h>
#include <string>
#include <fstream>
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <map>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#endif

#include "vulkan/vulkan.h"
#include "vulkantools.h"

#include <assimp/Importer.hpp> 
#include <assimp/scene.h>     
#include <assimp/postprocess.h>
#include <assimp/cimport.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#if defined(__ANDROID__)
#include <android/asset_manager.h>
#endif

namespace vkMeshLoader 
{
	typedef enum VertexLayout {
		VERTEX_LAYOUT_POSITION = 0x0,
		VERTEX_LAYOUT_NORMAL = 0x1,
		VERTEX_LAYOUT_COLOR = 0x2,
		VERTEX_LAYOUT_UV = 0x3,
		VERTEX_LAYOUT_TANGENT = 0x4,
		VERTEX_LAYOUT_BITANGENT = 0x5,
		VERTEX_LAYOUT_DUMMY_FLOAT = 0x6,
		VERTEX_LAYOUT_DUMMY_VEC4 = 0x7
	} VertexLayout;

	struct MeshBufferInfo 
	{
		vk::Buffer buf;
		vk::DeviceMemory mem;
		size_t size = 0;
	};

	struct MeshBuffer 
	{
		MeshBufferInfo vertices;
		MeshBufferInfo indices;
		uint32_t indexCount;
		glm::vec3 dim;
	};

	// Get vertex size from vertex layout
	static uint32_t vertexSize(std::vector<vkMeshLoader::VertexLayout> layout)
	{
		uint32_t vSize = 0;
		for (auto& layoutDetail : layout)
		{
			switch (layoutDetail)
			{
			// UV only has two components
			case VERTEX_LAYOUT_UV: 
				vSize += 2 * sizeof(float);
				break;
			default:
				vSize += 3 * sizeof(float);
			}
		}
		return vSize;
	}

	// Stores some additonal info and functions for 
	// specifying pipelines, vertex bindings, etc.
	class Mesh
	{
	public:
		MeshBuffer buffers;

		vk::PipelineLayout pipelineLayout;
		vk::Pipeline pipeline;
		vk::DescriptorSet descriptorSet;

		uint32_t vertexBufferBinding = 0;

		vk::PipelineVertexInputStateCreateInfo vertexInputState;
		vk::VertexInputBindingDescription bindingDescription;
		std::vector<vk::VertexInputAttributeDescription> attributeDescriptions;

		void setupVertexInputState(std::vector<vkMeshLoader::VertexLayout> layout)
		{
			bindingDescription = vkTools::initializers::vertexInputBindingDescription(
				vertexBufferBinding,
				vertexSize(layout),
				vk::VertexInputRate::eVertex);

			attributeDescriptions.clear();
			uint32_t offset = 0;
			uint32_t binding = 0;
			for (auto& layoutDetail : layout)
			{
				// Format (layout)
				vk::Format format = (layoutDetail == VERTEX_LAYOUT_UV) ? vk::Format::eR32G32Sfloat : vk::Format::eR32G32B32Sfloat;

				attributeDescriptions.push_back(
					vkTools::initializers::vertexInputAttributeDescription(
						vertexBufferBinding,
						binding,
						format,
						offset));

				// Offset
				offset += (layoutDetail == VERTEX_LAYOUT_UV) ? (2 * sizeof(float)) : (3 * sizeof(float));
				binding++;
			}

			vertexInputState = vk::PipelineVertexInputStateCreateInfo();
			vertexInputState.vertexBindingDescriptionCount = 1;
			vertexInputState.pVertexBindingDescriptions = &bindingDescription;
			vertexInputState.vertexAttributeDescriptionCount = attributeDescriptions.size();
			vertexInputState.pVertexAttributeDescriptions = attributeDescriptions.data();
		}

		void drawIndexed(vk::CommandBuffer cmdBuffer)
		{
			if (pipeline)
			{
				cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
			}
			if ((pipelineLayout) && (descriptorSet))
			{
				cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, descriptorSet, nullptr);
			}
			cmdBuffer.bindVertexBuffers(vertexBufferBinding, buffers.vertices.buf, vk::DeviceSize());
			cmdBuffer.bindIndexBuffer(buffers.indices.buf, 0, vk::IndexType::eUint32);
			cmdBuffer.drawIndexed(buffers.indexCount, 1, 0, 0, 0);
		}
	};

	static void freeMeshBufferResources(vk::Device device, vkMeshLoader::MeshBuffer *meshBuffer)
	{
		device.destroyBuffer(meshBuffer->vertices.buf);
		device.freeMemory(meshBuffer->vertices.mem);
		if (meshBuffer->indices.buf)
		{
			device.destroyBuffer(meshBuffer->indices.buf);
			device.freeMemory(meshBuffer->indices.mem);
		}
	}
}

// Simple mesh class for getting all the necessary stuff from models loaded via ASSIMP
class VulkanMeshLoader {
private:

	struct Vertex
	{
		glm::vec3 m_pos;
		glm::vec2 m_tex;
		glm::vec3 m_normal;
		glm::vec3 m_color;
		glm::vec3 m_tangent;
		glm::vec3 m_binormal;

		Vertex() {}

		Vertex(const glm::vec3& pos, const glm::vec2& tex, const glm::vec3& normal, const glm::vec3& tangent, const glm::vec3& bitangent, const glm::vec3& color)
		{
			m_pos = pos;
			m_tex = tex;
			m_normal = normal;
			m_color = color;
			m_tangent = tangent;
			m_binormal = bitangent;
		}
	};

	struct MeshEntry {
		uint32_t NumIndices;
		uint32_t MaterialIndex;
		uint32_t vertexBase;
		std::vector<Vertex> Vertices;
		std::vector<unsigned int> Indices;
	};

	vk::Bool32 getMemoryType(vk::PhysicalDeviceMemoryProperties deviceMemoryProperties, uint32_t typeBits, vk::MemoryPropertyFlags properties)
	{
		for (uint32_t i = 0; i < 32; i++)
		{
			if ((typeBits & 1) == 1)
			{
				if ((deviceMemoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
				{
					return i;
				}
			}
			typeBits >>= 1;
		}

		// todo : throw error
		return 0;
	}

public:
#if defined(__ANDROID__)
	AAssetManager* assetManager = nullptr;
#endif

	std::vector<MeshEntry> m_Entries;

	struct Dimension 
	{
		glm::vec3 min = glm::vec3(FLT_MAX);
		glm::vec3 max = glm::vec3(-FLT_MAX);
		glm::vec3 size;
	} dim;

	uint32_t numVertices = 0;

	// Optional
	struct
	{
		vk::Buffer buf;
		vk::DeviceMemory mem;
	} vertexBuffer;

	struct {
		vk::Buffer buf;
		vk::DeviceMemory mem;
		uint32_t count;
	} indexBuffer;

	vk::PipelineVertexInputStateCreateInfo vi;
	std::vector<vk::VertexInputBindingDescription> bindingDescriptions;
	std::vector<vk::VertexInputAttributeDescription> attributeDescriptions;
	vk::Pipeline pipeline;

	Assimp::Importer Importer;
	const aiScene* pScene;

	~VulkanMeshLoader()
	{
		m_Entries.clear();
	}

	// Loads the mesh with some default flags
	bool LoadMesh(const std::string& filename) 
	{
		int flags = aiProcess_FlipWindingOrder | aiProcess_Triangulate | aiProcess_PreTransformVertices | aiProcess_CalcTangentSpace | aiProcess_GenSmoothNormals;

		return LoadMesh(filename, flags);
	}

	// Load the mesh with custom flags
	bool LoadMesh(const std::string& filename, int flags)
	{
#if defined(__ANDROID__)
		// Meshes are stored inside the apk on Android (compressed)
		// So they need to be loaded via the asset manager

		AAsset* asset = AAssetManager_open(assetManager, filename.c_str(), AASSET_MODE_STREAMING);
		assert(asset);
		size_t size = AAsset_getLength(asset);

		assert(size > 0);
		
		void *meshData = malloc(size);
		AAsset_read(asset, meshData, size);
		AAsset_close(asset);

		pScene = Importer.ReadFileFromMemory(meshData, size, flags);

		free(meshData);
#else
		pScene = Importer.ReadFile(filename.c_str(), flags);
#endif

		if (pScene)
		{
			return InitFromScene(pScene, filename);
		}
		else 
		{
			printf("Error parsing '%s': '%s'\n", filename.c_str(), Importer.GetErrorString());
#if defined(__ANDROID__)
			LOGE("Error parsing '%s': '%s'", filename.c_str(), Importer.GetErrorString());
#endif
			return false;
		}
	}

	bool InitFromScene(const aiScene* pScene, const std::string& Filename)
	{
		m_Entries.resize(pScene->mNumMeshes);

		// Counters
		for (unsigned int i = 0; i < m_Entries.size(); i++)
		{
			m_Entries[i].vertexBase = numVertices;
			numVertices += pScene->mMeshes[i]->mNumVertices;
		}

		// Initialize the meshes in the scene one by one
		for (unsigned int i = 0; i < m_Entries.size(); i++) 
		{
			const aiMesh* paiMesh = pScene->mMeshes[i];
			InitMesh(i, paiMesh, pScene);
		}

		return true;
	}

	void InitMesh(unsigned int index, const aiMesh* paiMesh, const aiScene* pScene)
	{
		m_Entries[index].MaterialIndex = paiMesh->mMaterialIndex;

		aiColor3D pColor(0.f, 0.f, 0.f);
		pScene->mMaterials[paiMesh->mMaterialIndex]->Get(AI_MATKEY_COLOR_DIFFUSE, pColor);

		aiVector3D Zero3D(0.0f, 0.0f, 0.0f);

		for (unsigned int i = 0; i < paiMesh->mNumVertices; i++) {
			aiVector3D* pPos = &(paiMesh->mVertices[i]);
			aiVector3D* pNormal = &(paiMesh->mNormals[i]);
			aiVector3D *pTexCoord;
			if (paiMesh->HasTextureCoords(0))
			{
				pTexCoord = &(paiMesh->mTextureCoords[0][i]);
			}
			else {
				pTexCoord = &Zero3D;
			}
			aiVector3D* pTangent = (paiMesh->HasTangentsAndBitangents()) ? &(paiMesh->mTangents[i]) : &Zero3D;
			aiVector3D* pBiTangent = (paiMesh->HasTangentsAndBitangents()) ? &(paiMesh->mBitangents[i]) : &Zero3D;

			Vertex v(glm::vec3(pPos->x, -pPos->y, pPos->z), 
				glm::vec2(pTexCoord->x , pTexCoord->y),
				glm::vec3(pNormal->x, pNormal->y, pNormal->z),
				glm::vec3(pTangent->x, pTangent->y, pTangent->z),
				glm::vec3(pBiTangent->x, pBiTangent->y, pBiTangent->z),
				glm::vec3(pColor.r, pColor.g, pColor.b)
				);

			dim.max.x = fmax(pPos->x, dim.max.x);
			dim.max.y = fmax(pPos->y, dim.max.y);
			dim.max.z = fmax(pPos->z, dim.max.z);

			dim.min.x = fmin(pPos->x, dim.min.x);
			dim.min.y = fmin(pPos->y, dim.min.y);
			dim.min.z = fmin(pPos->z, dim.min.z);

			m_Entries[index].Vertices.push_back(v);
		}

		dim.size = dim.max - dim.min;

		for (unsigned int i = 0; i < paiMesh->mNumFaces; i++) 
		{
			const aiFace& Face = paiMesh->mFaces[i];
			if (Face.mNumIndices != 3)
				continue;
			m_Entries[index].Indices.push_back(Face.mIndices[0]);
			m_Entries[index].Indices.push_back(Face.mIndices[1]);
			m_Entries[index].Indices.push_back(Face.mIndices[2]);
		}
	}

	// Clean up vulkan resources used by a mesh
	static void freeVulkanResources(vk::Device device, VulkanMeshLoader *mesh)
	{
		device.destroyBuffer(mesh->vertexBuffer.buf);
		device.freeMemory(mesh->vertexBuffer.mem);
		device.destroyBuffer(mesh->indexBuffer.buf);
		device.freeMemory(mesh->indexBuffer.mem);
	}

	vk::Result createBuffer(
		vk::Device device, 
		vk::PhysicalDeviceMemoryProperties deviceMemoryProperties,
		vk::BufferUsageFlags usageFlags, 
		vk::MemoryPropertyFlags memoryPropertyFlags, 
		vk::DeviceSize size, 
		vk::Buffer &buffer, 
		vk::DeviceMemory &memory)
	{
		vk::MemoryAllocateInfo memAllocInfo;	
		vk::MemoryRequirements memReqs;

		vk::BufferCreateInfo bufferInfo = vkTools::initializers::bufferCreateInfo(usageFlags, size);
		buffer = device.createBuffer(bufferInfo);
		memReqs = device.getBufferMemoryRequirements(buffer);
		memAllocInfo.allocationSize = memReqs.size;
		memAllocInfo.memoryTypeIndex = getMemoryType(deviceMemoryProperties, memReqs.memoryTypeBits, memoryPropertyFlags);
		memory = device.allocateMemory(memAllocInfo);
		device.bindBufferMemory(buffer, memory, 0);

		return vk::Result::eSuccess;
	}

	// Create vertex and index buffer with given layout
	// Note : Only does staging if a valid command buffer and transfer queue are passed
	void createBuffers(
		vk::Device device,
		vk::PhysicalDeviceMemoryProperties deviceMemoryProperties,
		vkMeshLoader::MeshBuffer *meshBuffer,
		std::vector<vkMeshLoader::VertexLayout> layout,
		float scale,
		bool useStaging,
		vk::CommandBuffer copyCmd,
		vk::Queue copyQueue)
	{
		std::vector<float> vertexBuffer;
		for (int m = 0; m < m_Entries.size(); m++)
		{
			for (int i = 0; i < m_Entries[m].Vertices.size(); i++)
			{
				// Push vertex data depending on layout
				for (auto& layoutDetail : layout)
				{
					// Position
					if (layoutDetail == vkMeshLoader::VERTEX_LAYOUT_POSITION)
					{
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_pos.x * scale);
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_pos.y * scale);
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_pos.z * scale);
					}
					// Normal
					if (layoutDetail == vkMeshLoader::VERTEX_LAYOUT_NORMAL)
					{
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_normal.x);
						vertexBuffer.push_back(-m_Entries[m].Vertices[i].m_normal.y);
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_normal.z);
					}
					// Texture coordinates
					if (layoutDetail == vkMeshLoader::VERTEX_LAYOUT_UV)
					{
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_tex.s);
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_tex.t);
					}
					// Color
					if (layoutDetail == vkMeshLoader::VERTEX_LAYOUT_COLOR)
					{
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_color.r);
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_color.g);
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_color.b);
					}
					// Tangent
					if (layoutDetail == vkMeshLoader::VERTEX_LAYOUT_TANGENT)
					{
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_tangent.x);
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_tangent.y);
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_tangent.z);
					}
					// Bitangent
					if (layoutDetail == vkMeshLoader::VERTEX_LAYOUT_BITANGENT)
					{
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_binormal.x);
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_binormal.y);
						vertexBuffer.push_back(m_Entries[m].Vertices[i].m_binormal.z);
					}
					// Dummy layout components for padding
					if (layoutDetail == vkMeshLoader::VERTEX_LAYOUT_DUMMY_FLOAT)
					{
						vertexBuffer.push_back(0.0f);
					}
					if (layoutDetail == vkMeshLoader::VERTEX_LAYOUT_DUMMY_VEC4)
					{
						vertexBuffer.push_back(0.0f);
						vertexBuffer.push_back(0.0f);
						vertexBuffer.push_back(0.0f);
						vertexBuffer.push_back(0.0f);
					}
				}
			}
		}
		meshBuffer->vertices.size = vertexBuffer.size() * sizeof(float);

		dim.min *= scale;
		dim.max *= scale;
		dim.size *= scale;

		std::vector<uint32_t> indexBuffer;
		for (uint32_t m = 0; m < m_Entries.size(); m++)
		{
			uint32_t indexBase = (uint32_t)indexBuffer.size();
			for (uint32_t i = 0; i < m_Entries[m].Indices.size(); i++)
			{
				indexBuffer.push_back(m_Entries[m].Indices[i] + indexBase);
			}
		}
		meshBuffer->indices.size = indexBuffer.size() * sizeof(uint32_t);

		meshBuffer->indexCount = (uint32_t)indexBuffer.size();

		void* data;

		// Use staging buffer to move vertex and index buffer to device local memory
		if (useStaging && copyQueue && copyCmd)
		{
			// Create staging buffers
			struct {
				vk::Buffer buffer;
				vk::DeviceMemory memory;
			} vertexStaging, indexStaging;

			// Vertex buffer
			createBuffer(
				device,
				deviceMemoryProperties,
				vk::BufferUsageFlagBits::eTransferSrc,
				vk::MemoryPropertyFlagBits::eHostVisible,
				meshBuffer->vertices.size,
				vertexStaging.buffer,
				vertexStaging.memory);

			data = device.mapMemory(vertexStaging.memory, 0, VK_WHOLE_SIZE, vk::MemoryMapFlags());
			memcpy(data, vertexBuffer.data(), meshBuffer->vertices.size);
			device.unmapMemory(vertexStaging.memory);

			// Index buffer
			createBuffer(
				device,
				deviceMemoryProperties,
				vk::BufferUsageFlagBits::eTransferSrc,
				vk::MemoryPropertyFlagBits::eHostVisible,
				meshBuffer->indices.size,
				indexStaging.buffer,
				indexStaging.memory);

			data = device.mapMemory(indexStaging.memory, 0, VK_WHOLE_SIZE, vk::MemoryMapFlags());
			memcpy(data, indexBuffer.data(), meshBuffer->indices.size);
			device.unmapMemory(indexStaging.memory);

			// Create device local target buffers
			// Vertex buffer
			createBuffer(
				device,
				deviceMemoryProperties,
				vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eDeviceLocal,
				meshBuffer->vertices.size,
				meshBuffer->vertices.buf,
				meshBuffer->vertices.mem);

			// Index buffer
			createBuffer(
				device,
				deviceMemoryProperties,
				vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eDeviceLocal,
				meshBuffer->indices.size,
				meshBuffer->indices.buf,
				meshBuffer->indices.mem);

			// Copy from staging buffers
			vk::CommandBufferBeginInfo cmdBufInfo;
			copyCmd.begin(cmdBufInfo);

			vk::BufferCopy copyRegion;

			copyRegion.size = meshBuffer->vertices.size;
			copyCmd.copyBuffer(vertexStaging.buffer, meshBuffer->vertices.buf, copyRegion);

			copyRegion.size = meshBuffer->indices.size;
			copyCmd.copyBuffer(indexStaging.buffer, meshBuffer->indices.buf, copyRegion);

			copyCmd.end();

			vk::SubmitInfo submitInfo;
			submitInfo.commandBufferCount = 1;
			submitInfo.pCommandBuffers = &copyCmd;

			copyQueue.submit(submitInfo, VK_NULL_HANDLE);
			copyQueue.waitIdle();

			device.destroyBuffer(vertexStaging.buffer);
			device.freeMemory(vertexStaging.memory);
			device.destroyBuffer(indexStaging.buffer);
			device.freeMemory(indexStaging.memory);
		}
		else
		{
			// Generate vertex buffer
			createBuffer(
				device,
				deviceMemoryProperties,
				vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferSrc,
				vk::MemoryPropertyFlagBits::eHostVisible,
				meshBuffer->vertices.size,
				meshBuffer->vertices.buf,
				meshBuffer->vertices.mem);

			data = device.mapMemory(meshBuffer->vertices.mem, 0, meshBuffer->vertices.size, vk::MemoryMapFlags());
			memcpy(data, vertexBuffer.data(), meshBuffer->vertices.size);
			device.unmapMemory(meshBuffer->vertices.mem);

			// Generate index buffer
			createBuffer(
				device,
				deviceMemoryProperties,
				vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferSrc,
				vk::MemoryPropertyFlagBits::eHostVisible,
				meshBuffer->indices.size,
				meshBuffer->indices.buf,
				meshBuffer->indices.mem);

			data = device.mapMemory(meshBuffer->indices.mem, 0, meshBuffer->indices.size, vk::MemoryMapFlags());
			memcpy(data, indexBuffer.data(), meshBuffer->indices.size);
			device.unmapMemory(meshBuffer->indices.mem);
		}
	}

	// Create vertex and index buffer with given layout
	void createVulkanBuffers(
		vk::Device device, 
		vk::PhysicalDeviceMemoryProperties deviceMemoryProperties,
		vkMeshLoader::MeshBuffer *meshBuffer, 
		std::vector<vkMeshLoader::VertexLayout> layout, 
		float scale)
	{
		createBuffers(
			device,
			deviceMemoryProperties,
			meshBuffer,
			layout,
			scale,
			false,
			VK_NULL_HANDLE,
			VK_NULL_HANDLE);
	}
};
