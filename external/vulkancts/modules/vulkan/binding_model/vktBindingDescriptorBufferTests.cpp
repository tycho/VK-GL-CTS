/*------------------------------------------------------------------------
 * Vulkan Conformance Tests
 * ------------------------
 *
 * Copyright (c) 2022 The Khronos Group Inc.
 * Copyright (C) 2022 Advanced Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *//*!
 * \file vktBindingDescriptorBufferTests.cpp
 * \brief Descriptor buffer (extension) tests
 *//*--------------------------------------------------------------------*/

#include "deSharedPtr.hpp"
#include "deUniquePtr.hpp"
#include "deSha1.h"
#include "deRandom.hpp"
#include "tcuCommandLine.hpp"
#include "vktBindingDescriptorBufferTests.hpp"
#include "vktTestCaseUtil.hpp"
#include "vktTestGroupUtil.hpp"
#include "vktCustomInstancesDevices.hpp"
#include "vkBuilderUtil.hpp"
#include "vkCmdUtil.hpp"
#include "vkMemUtil.hpp"
#include "vkObjUtil.hpp"
#include "vkQueryUtil.hpp"
#include "vkRefUtil.hpp"
#include "vkStrUtil.hpp"
#include "vkTypeUtil.hpp"

// The defines below can be changed for debugging purposes, otherwise keep them as is.

#define DEBUG_FORCE_STAGED_UPLOAD			false	// false - prefer direct write to device-local memory
#define DEBUG_MIX_DIRECT_AND_STAGED_UPLOAD	true	// true  - use some staged uploads to test new access flag

namespace vkt
{
namespace BindingModel
{
namespace
{
using namespace vk;
using de::MovePtr;
using de::UniquePtr;
using de::SharedPtr;

constexpr deUint32 INDEX_INVALID = ~0u;
constexpr deUint32 OFFSET_UNUSED = ~0u;

constexpr deUint32	ConstResultBufferDwords		= 0x4;		// uvec4
constexpr deUint32	ConstInlineBlockDwords		= 0x40;		// 256 B spec minimum
constexpr deUint32	ConstUniformBufferDwords	= 0x1000;	// 16 KiB spec minimum
constexpr deUint32	ConstMaxDescriptorArraySize	= 4;		// at most define N-element descriptor arrays

template<typename T>
inline deUint32 u32(const T& value)
{
	return static_cast<deUint32>(value);
}

template <typename T, typename... args_t>
inline de::MovePtr<T> newMovePtr(args_t&&... args)
{
	return de::MovePtr<T>(new T(::std::forward<args_t>(args)...));
}

template<typename T>
inline SharedPtr<UniquePtr<T> > makeSharedUniquePtr()
{
	return SharedPtr<UniquePtr<T> >(
		new UniquePtr<T>(
			new T()));
}

inline void* offsetPtr(void* ptr, VkDeviceSize offset)
{
	return reinterpret_cast<char*>(ptr) + offset;
}

inline const void* offsetPtr(const void* ptr, VkDeviceSize offset)
{
	return reinterpret_cast<const char*>(ptr) + offset;
}

// Calculate the byte offset of ptr from basePtr.
// This can be useful if an object at ptr is suballocated from a larger allocation at basePtr, for example.
inline std::size_t basePtrOffsetOf(const void* basePtr, const void* ptr)
{
	DE_ASSERT(basePtr <= ptr);
	return static_cast<std::size_t>(static_cast<const deUint8*>(ptr) - static_cast<const deUint8*>(basePtr));
}

// Used to distinguish different test implementations.
enum class TestVariant : deUint32
{
	SINGLE,								// basic sanity check for descriptor/shader combinations
	MULTIPLE,							// multiple buffer bindings with various descriptor types
	MAX,								// verify max(Sampler/Resource)DescriptorBufferBindings
	EMBEDDED_IMMUTABLE_SAMPLERS,		// various usages of embedded immutable samplers
	PUSH_DESCRIPTOR,					// use push descriptors and descriptor buffer at the same time
	PUSH_TEMPLATE,						// use push descriptor template and descriptor buffer at the same time
	ROBUSTNESS,							// TODO
	CAPTURE_REPLAY,						// TODO
};

// Optional; Used to add variations for a specific test case.
enum class SubCase : deUint32
{
	NONE,						// no sub case, i.e. a baseline test case
	IMMUTABLE_SAMPLERS,			// treat all samplers as immutable
	INCREMENTAL_BIND,			// call vkCmdBindDescriptorBuffersEXT/vkCmdSetDescriptorBufferOffsetsEXT multiple times to complete the full bind
};

// A simplified descriptor binding, used to define the test case behavior at a high level.
struct SimpleBinding
{
	deUint32			set;
	deUint32			binding;
	VkDescriptorType	type;
	deUint32			count;
	deUint32			inputAttachmentIndex;

	bool				isResultBuffer;				// binding used for compute buffer results
	bool				isEmbeddedImmutableSampler;	// binding used as immutable embedded sampler
};

// Scan simple bindings for the binding with the compute shader's result storage buffer.
deUint32 getComputeResultBufferIndex(const std::vector<SimpleBinding>& simpleBindings)
{
	bool	 found						= false;
	deUint32 computeResultBufferIndex	= 0;

	for (const auto& sb : simpleBindings)
	{
		if (sb.isResultBuffer)
		{
			found = true;

			break;
		}

		++computeResultBufferIndex;
	}

	if (!found)
	{
		computeResultBufferIndex = INDEX_INVALID;
	}

	return computeResultBufferIndex;
}

// The parameters for a test case (with the exclusion of simple bindings).
// Not all values are used by every test variant.
struct TestParams
{
	deUint32					hash;				// a value used to "salt" results in memory to get unique values per test case
	TestVariant					variant;			// general type of the test case
	SubCase						subcase;			// a variation of the specific test case
	VkShaderStageFlagBits		stage;				// which shader makes use of the bindings
	VkQueueFlagBits				queue;				// which queue to use for the access
	deUint32					bufferBindingCount;	// number of buffer bindings to create
	deUint32					setsPerBuffer;		// how may sets to put in one buffer binding

	// Basic test
	VkDescriptorType			descriptor;			// descriptor type to use in single descriptor tests

	// Max bindings test
	deUint32					samplerBufferBindingCount;
	deUint32					resourceBufferBindingCount;

	// Max embedded immutable samplers test
	deUint32					embeddedImmutableSamplerBufferBindingCount;
	deUint32					embeddedImmutableSamplersPerBuffer;

	// Push descriptors
	deUint32					pushDescriptorSetIndex;		// which descriptor set is updated with push descriptor/template

	bool isCompute() const
	{
		return stage == VK_SHADER_STAGE_COMPUTE_BIT;
	}

	bool isGraphics() const
	{
		return (stage & VK_SHADER_STAGE_ALL_GRAPHICS) != 0;
	}

	bool isGeometry() const
	{
		return stage == VK_SHADER_STAGE_GEOMETRY_BIT;
	}

	bool isTessellation() const
	{
		return (stage & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)) != 0;
	}

	bool isPushDescriptorTest() const
	{
		return (variant == TestVariant::PUSH_DESCRIPTOR) || (variant == TestVariant::PUSH_TEMPLATE);
	}

	// Update the hash field. Must be called after changing the value of any other parameters.
	void updateHash()
	{
		hash = 0;

		deSha1 sha1Hash;
		deSha1_compute(&sha1Hash, sizeof(*this), this);

		for (deUint32 i = 0; i < DE_LENGTH_OF_ARRAY(sha1Hash.hash); ++i)
		{
			hash ^= sha1Hash.hash[i];
		}
	}
};

// A convenience holder for a buffer-related data.
struct BufferAlloc
{
	VkDeviceSize			size			= 0;
	VkDeviceAddress			deviceAddress	= 0;	// non-zero if used
	VkBufferUsageFlags		usage			= 0;

	Move<VkBuffer>			buffer;
	MovePtr<Allocation>		alloc;

	BufferAlloc() = default;
	BufferAlloc(BufferAlloc&) = delete;

	void loadDeviceAddress(const DeviceInterface& vk, VkDevice device)
	{
		VkBufferDeviceAddressInfo bdaInfo = initVulkanStructure();
		bdaInfo.buffer = *buffer;

		deviceAddress = vk.getBufferDeviceAddress(device, &bdaInfo);
	}
};

using BufferAllocPtr = SharedPtr<BufferAlloc>;

// A convenience holder for image-related data.
struct ImageAlloc
{
	VkImageCreateInfo		info	  = {};
	VkDeviceSize			sizeBytes = 0;
	VkImageLayout			layout    = VK_IMAGE_LAYOUT_UNDEFINED;	// layout used when image is accessed

	Move<VkImage>			image;
	Move<VkImageView>		imageView;
	MovePtr<Allocation>		alloc;

	ImageAlloc() = default;
	ImageAlloc(ImageAlloc&) = delete;
};

using ImageAllocPtr = SharedPtr<ImageAlloc>;

// A descriptor binding with supporting data.
class Binding
{
public:
	VkDescriptorSetLayoutBinding	binding;
	VkDeviceSize					offset;
	deUint32                        inputAttachmentIndex;	// if used
	bool							isResultBuffer;			// used with compute shaders

	// Index into the vector of resources in the main test class, if used.
	// It's an array, because a binding may have several arrayed descriptors.
	deUint32						perBindingResourceIndex[ConstMaxDescriptorArraySize];

	// An array of immutable samplers, if used by the binding.
	VkSampler						immutableSamplers[ConstMaxDescriptorArraySize];

	Binding(const VkDescriptorSetLayoutBinding& inBinding)
		: binding(inBinding)
		, offset(0)
		, inputAttachmentIndex(0)
		, isResultBuffer(false)
	{
		for (deUint32 i = 0; i < DE_LENGTH_OF_ARRAY(perBindingResourceIndex); ++i)
		{
			perBindingResourceIndex[i]	= INDEX_INVALID;
			immutableSamplers[i]		= 0;
		}
	}
};

// Get an array of descriptor bindings.
std::vector<VkDescriptorSetLayoutBinding> getDescriptorSetLayoutBindings(const std::vector<Binding>& allBindings)
{
	std::vector<VkDescriptorSetLayoutBinding> result;
	result.reserve(allBindings.size());

	for (auto& binding : allBindings)
	{
		result.emplace_back(binding.binding);
	}

	return result;
}

// Descriptor data used with push descriptors (regular and templates).
struct PushDescriptorData
{
	VkDescriptorImageInfo	imageInfos		[ConstMaxDescriptorArraySize];
	VkDescriptorBufferInfo	bufferInfos		[ConstMaxDescriptorArraySize];
	VkBufferView			texelBufferViews[ConstMaxDescriptorArraySize];
};

// A convenience holder for a descriptor set layout and its bindings.
struct DescriptorSetLayoutHolder
{
	std::vector<Binding>			bindings;

	Move<VkDescriptorSetLayout>		layout;
	VkDeviceSize					size							= 0;
	deUint32						bufferIndex						= INDEX_INVALID;
	VkDeviceSize					bufferOffset					= 0;
	VkDeviceSize					stagingBufferOffset				= OFFSET_UNUSED;
	bool							hasEmbeddedImmutableSamplers	= false;
	bool							usePushDescriptors				= false;	// instead of descriptor buffer

	DescriptorSetLayoutHolder() = default;
	DescriptorSetLayoutHolder(DescriptorSetLayoutHolder&) = delete;
};

using DSLPtr = SharedPtr<UniquePtr<DescriptorSetLayoutHolder> >;

// Get an array of descriptor set layouts.
std::vector<VkDescriptorSetLayout> getDescriptorSetLayouts(const std::vector<DSLPtr>& dslPtrs)
{
	std::vector<VkDescriptorSetLayout> result;
	result.reserve(dslPtrs.size());

	for (auto& pDsl : dslPtrs)
	{
		result.emplace_back((**pDsl).layout.get());
	}

	return result;
}

// A helper struct to keep descriptor's underlying resource data.
// This is intended to be flexible and support a mix of buffer/image/sampler, depending on the binding type.
struct ResourceHolder
{
	BufferAlloc			buffer;
	ImageAlloc			image;
	Move<VkSampler>		sampler;
	Move<VkBufferView>	bufferView;

	ResourceHolder() = default;
	ResourceHolder(ResourceHolder&) = delete;
};

using ResourcePtr = SharedPtr<UniquePtr<ResourceHolder> >;

// Used in test case name generation.
std::string toString (VkQueueFlagBits queue)
{
	switch (queue)
	{
	case VK_QUEUE_GRAPHICS_BIT:		return "graphics";
	case VK_QUEUE_COMPUTE_BIT:		return "compute";

	default:
		DE_ASSERT(false);
		break;
	}
	return "";
}

// Used in test case name generation.
std::string toString (VkDescriptorType type)
{
	switch (type)
	{
	case VK_DESCRIPTOR_TYPE_SAMPLER:					return "sampler";
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:		return "combined_image_sampler";
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:				return "sampled_image";
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:				return "storage_image";
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:		return "uniform_texel_buffer";
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:		return "storage_texel_buffer";
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:				return "uniform_buffer";
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:				return "storage_buffer";
	case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:			return "input_attachment";
	case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:		return "inline_uniform_block";
	case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return "acceleration_structure";

	default:
		DE_ASSERT(false);
		break;
	}
	return "";
}

// Used in test case name generation.
std::string toString (VkShaderStageFlagBits stage)
{
	switch (stage)
	{
    case VK_SHADER_STAGE_VERTEX_BIT:					return "vert";
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:		return "tess_cont";
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:	return "tess_eval";
    case VK_SHADER_STAGE_GEOMETRY_BIT:                  return "geom";
    case VK_SHADER_STAGE_FRAGMENT_BIT:                  return "frag";
    case VK_SHADER_STAGE_COMPUTE_BIT:                   return "comp";
    case VK_SHADER_STAGE_RAYGEN_BIT_KHR:                return "raygen";
    case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:				return "anyhit";
    case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:           return "closehit";
    case VK_SHADER_STAGE_MISS_BIT_KHR:                  return "miss";
    case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:          return "intersect";
    case VK_SHADER_STAGE_CALLABLE_BIT_KHR:              return "callable";

	default:
		DE_ASSERT(false);
		break;
	}
	return "";
}

// Used in test case name generation.
std::string getCaseName(const TestParams& params)
{
	std::ostringstream str;

	str << toString(params.queue)
		<< "_"
		<< toString(params.stage);

	if (params.variant == TestVariant::SINGLE)
	{
		str << "_" << toString(params.descriptor);
	}
	else if (params.variant == TestVariant::MULTIPLE)
	{
		str << "_buffers" << params.bufferBindingCount
			<< "_sets" << params.setsPerBuffer;
	}
	else if (params.variant == TestVariant::MAX)
	{
		str << "_sampler" << params.samplerBufferBindingCount
			<< "_resource" << params.resourceBufferBindingCount;
	}
	else if (params.variant == TestVariant::EMBEDDED_IMMUTABLE_SAMPLERS)
	{
		str << "_buffers" << params.embeddedImmutableSamplerBufferBindingCount
			<< "_samplers" << params.embeddedImmutableSamplersPerBuffer;
	}
	else if (params.isPushDescriptorTest())
	{
		str << "_sets" << (params.bufferBindingCount + 1)
			<< "_push_set" << params.pushDescriptorSetIndex;
	}

	if (params.subcase == SubCase::IMMUTABLE_SAMPLERS)
	{
		str << "_imm_samplers";
	}
	else if (params.subcase == SubCase::INCREMENTAL_BIND)
	{
		str << "_incremental_bind";
	}

	return str.str();
}

// Used by shaders to identify a specific binding.
deUint32 packBindingArgs(deUint32 set, deUint32 binding, deUint32 arrayIndex)
{
	DE_ASSERT(set		 < 0x100);
	DE_ASSERT(binding	 < 0x100);
	DE_ASSERT(arrayIndex < 0x100);

	return (arrayIndex << 16) | ((set & 0xFFu) << 8) | (binding & 0xFFu);
}

// Used by shaders to identify a specific binding.
void unpackBindingArgs(deUint32 packed, deUint32* pOutSet, deUint32* pBinding, deUint32* pArrayIndex)
{
	if (pBinding != nullptr)
	{
		*pBinding = packed & 0xFFu;
	}
	if (pOutSet != nullptr)
	{
		*pOutSet = (packed >> 8) & 0xFFu;
	}
	if (pArrayIndex != nullptr)
	{
		*pArrayIndex = (packed >> 16) & 0xFFu;
	}
}

// The expected data read through a descriptor. Try to get a unique value per test and binding.
deUint32 getExpectedData(deUint32 hash, deUint32 set, deUint32 binding, deUint32 arrayIndex = 0)
{
	return hash ^ packBindingArgs(set, binding, arrayIndex);
}

// Used by shaders.
std::string glslFormat(deUint32 value)
{
	return std::to_string(value) + "u";
}

// Generate a unique shader resource name for a binding.
std::string glslResourceName(deUint32 set, deUint32 binding)
{
	// A generic name for any accessible shader binding.
	std::ostringstream str;
	str << "res_" << set << "_" << binding;
	return str.str();
}

// Generate GLSL that declares a descriptor binding.
std::string glslDeclareBinding(
	VkDescriptorType	type,
	deUint32			set,
	deUint32			binding,
	deUint32			count,
	deUint32			attachmentIndex,
	deUint32			bufferArraySize)
{
	std::ostringstream str;

	str << "layout(set = " << set << ", binding = " << binding;

	// Additional layout information
	switch (type)
	{
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
		str << ", r32ui) ";
		break;
	case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		str << ", input_attachment_index = " << attachmentIndex << ") ";
		break;
	default:
		str << ") ";
		break;
	}

	switch (type)
	{
	case VK_DESCRIPTOR_TYPE_SAMPLER:
		str << "uniform sampler ";
		break;
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		str << "uniform usampler2D ";
		break;
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		str << "uniform utexture2D ";
		break;
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		str << "uniform uimage2D ";
		break;
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		str << "uniform utextureBuffer ";
		break;
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
		str << "uniform uimageBuffer ";
		break;
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
		DE_ASSERT(bufferArraySize != 0);
		DE_ASSERT((bufferArraySize % 4) == 0);
		// std140 layout rules, each array element is aligned to 16 bytes.
		// Due to this, we will use uvec4 instead to access all dwords.
		str << "uniform Buffer_" << set << "_" << binding << " {\n"
			<< "    uvec4 data[" << (bufferArraySize / 4) << "];\n"
			<< "} ";
		break;
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
		DE_ASSERT(bufferArraySize != 0);
		str << "buffer Buffer_" << set << "_" << binding << " {\n"
			<< "    uint data[" << bufferArraySize << "];\n"
			<< "} ";
		break;
	case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		str << "uniform usubpassInput ";
		break;
	case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
		// TODO Shader must have "#extension GL_EXT_ray_tracing : require"
		str << "uniform accelerationStructureEXT ";
		break;
	default:
		DE_ASSERT(0);
		break;
	}

	str << glslResourceName(set, binding);

	if (count > 1)
	{
		str << "[" << count << "];\n";
	}
	else
	{
		str << ";\n";
	}

	return str.str();
}

// Generate all GLSL descriptor set/binding declarations.
std::string glslGlobalDeclarations(const TestParams& params, const std::vector<SimpleBinding>& simpleBindings)
{
	std::ostringstream str;

	if ((params.variant == TestVariant::SINGLE) ||
		(params.variant == TestVariant::MULTIPLE) ||
		(params.variant == TestVariant::MAX) ||
		(params.variant == TestVariant::EMBEDDED_IMMUTABLE_SAMPLERS) ||
		(params.variant == TestVariant::PUSH_DESCRIPTOR) ||
		(params.variant == TestVariant::PUSH_TEMPLATE))
	{
		for (const auto& sb : simpleBindings)
		{
			const deUint32 arraySize =
				sb.isResultBuffer ? ConstResultBufferDwords :
				(sb.type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) ? ConstInlineBlockDwords : ConstUniformBufferDwords;

			str << glslDeclareBinding(sb.type, sb.set, sb.binding, sb.count, sb.inputAttachmentIndex, arraySize);
		}
	}
	else
	{
		TCU_THROW(InternalError, "Not implemented");
	}

	return str.str();
}

// This function is used to return additional diagnostic information for a failed descriptor binding.
// For example, result Y is the packed binding information and result Z is the array index (for arrayed descriptors, or buffers).
std::string glslResultBlock(const std::string& indent, const std::string& resultY, const std::string& resultZ = "")
{
	std::ostringstream str;
	str << "{\n"
		<< indent << "	result.x += 1;\n"
		<< indent << "} else if (result.y == 0) {\n"
		<< indent << "	result.y = " << resultY << ";\n";

	if (!resultZ.empty())
	{
		str << indent << "	result.z = " << resultZ << ";\n";
	}

	str << indent << "}\n";
	return str.str();
}

// Generate GLSL that reads through the binding and compares the value.
// Successful reads increment a counter, while failed read will write back debug information.
std::string glslOutputVerification(const TestParams& params, const std::vector<SimpleBinding>& simpleBindings)
{
	std::ostringstream str;

	if ((params.variant == TestVariant::SINGLE) ||
		(params.variant == TestVariant::MULTIPLE) ||
		(params.variant == TestVariant::PUSH_DESCRIPTOR) ||
		(params.variant == TestVariant::PUSH_TEMPLATE))
	{
		// Read at least one value from a descriptor and compare it.
		// For buffers, verify every element.

		for (const auto& sb : simpleBindings)
		{
			deUint32 samplerIndex = INDEX_INVALID;

			if (sb.isResultBuffer)
			{
				// Used by other bindings.
				continue;
			}

			if (sb.type == VK_DESCRIPTOR_TYPE_SAMPLER)
			{
				// Used by sampled images.
				continue;
			}
			else if (sb.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
			{
				// Sampled images require a sampler to use.
				// Find a suitable sampler within the same descriptor set.

				bool found = false;
				samplerIndex = 0;

				for (const auto& sb1 : simpleBindings)
				{
					if ((sb.set == sb1.set) && (sb1.type == VK_DESCRIPTOR_TYPE_SAMPLER))
					{
						found = true;
						break;
					}

					++samplerIndex;
				}

				if (!found)
				{
					samplerIndex = INDEX_INVALID;
				}
			}

			deUint32 bufferLoopIterations = 0;

			switch (sb.type)
			{
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
				bufferLoopIterations = ConstUniformBufferDwords / 4;
				break;
			case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
				bufferLoopIterations = ConstInlineBlockDwords / 4;
				break;
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
				bufferLoopIterations = ConstUniformBufferDwords;
				break;
			default:
				// Ignored
				break;
			}

			for (deUint32 arrayIndex = 0; arrayIndex < sb.count; ++arrayIndex)
			{
				// Input attachment index increases with array index.
				const auto expectedData = glslFormat(getExpectedData(params.hash, sb.set, sb.binding, sb.inputAttachmentIndex + arrayIndex));
				const auto bindingArgs  = glslFormat(packBindingArgs(sb.set, sb.binding, sb.inputAttachmentIndex + arrayIndex));
				const auto& subscript   = (sb.count > 1) ? "[" + std::to_string(arrayIndex) + "]" : "";

				switch (sb.type)
				{
				case VK_DESCRIPTOR_TYPE_SAMPLER:
					TCU_THROW(InternalError, "Sampler is tested implicitly");
					break;

				case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
					// TODO
					TCU_THROW(InternalError, "Not implemented");
					break;

				case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
					str << "	if (subpassLoad(" << glslResourceName(sb.set, sb.binding) << subscript << ").r == " << expectedData << ") " << glslResultBlock("\t", bindingArgs);
					break;

				case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
				{
					DE_ASSERT(samplerIndex != INDEX_INVALID);
					const auto& samplerSb		 = simpleBindings[samplerIndex];
					const auto& samplerSubscript = (samplerSb.count > 1) ? "[" + std::to_string(arrayIndex % samplerSb.count) + "]" : "";

					// With samplers, verify the image color and the border color.

					std::stringstream samplerStr;
					samplerStr << "usampler2D("
						<< glslResourceName(sb.set, sb.binding)				  << subscript << ", "
						<< glslResourceName(samplerSb.set, samplerSb.binding) << samplerSubscript << ")";

					str << "	if ((textureLod(" << samplerStr.str() << ", vec2(0, 0), 0).r == " << expectedData << ") &&\n"
						<< "	    (textureLod(" << samplerStr.str() << ", vec2(-1, 0), 0) == uvec4(0, 0, 0, 1))) "
						<< glslResultBlock("\t", bindingArgs);
					break;
				}

				case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
					str << "	if ((textureLod(" << glslResourceName(sb.set, sb.binding) << subscript << ", vec2(0, 0), 0).r == " << expectedData << ") &&\n"
						<< "	    (textureLod(" << glslResourceName(sb.set, sb.binding) << subscript << ", vec2(-1, 0), 0) == uvec4(0, 0, 0, 1))) "
						<< glslResultBlock("\t", bindingArgs);
					break;

				case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
					str << "	if (imageLoad(" << glslResourceName(sb.set, sb.binding) << subscript << ", ivec2(0, 0)).r == " << expectedData << ") " << glslResultBlock("\t", bindingArgs);
					break;

				case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
					str << "	if (texelFetch(" << glslResourceName(sb.set, sb.binding) << subscript << ", 0).r == " << expectedData << ") " << glslResultBlock("\t", bindingArgs);
					break;

				case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
					str << "	if (imageLoad(" << glslResourceName(sb.set, sb.binding) << subscript << ", 0).r == " << expectedData << ") " << glslResultBlock("\t", bindingArgs);
					break;

				case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
				case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
					str << "	for (uint i = 0; i < " << glslFormat(bufferLoopIterations) << "; ++i) {\n"
						<< "		if (" << glslResourceName(sb.set, sb.binding) << subscript << ".data[i].x == (" << expectedData << " + 4 * i + 0)) " << glslResultBlock("\t\t", bindingArgs, "4 * i + 0")
						<< "		if (" << glslResourceName(sb.set, sb.binding) << subscript << ".data[i].y == (" << expectedData << " + 4 * i + 1)) " << glslResultBlock("\t\t", bindingArgs, "4 * i + 1")
						<< "		if (" << glslResourceName(sb.set, sb.binding) << subscript << ".data[i].z == (" << expectedData << " + 4 * i + 2)) " << glslResultBlock("\t\t", bindingArgs, "4 * i + 2")
						<< "		if (" << glslResourceName(sb.set, sb.binding) << subscript << ".data[i].w == (" << expectedData << " + 4 * i + 3)) " << glslResultBlock("\t\t", bindingArgs, "4 * i + 3")
						<< "	}\n";
					break;

				case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
					str << "	for (uint i = 0; i < " << glslFormat(bufferLoopIterations) << "; ++i) {\n"
						<< "		if (" << glslResourceName(sb.set, sb.binding) << subscript << ".data[i] == (" << expectedData << " + i)) " << glslResultBlock("\t\t", bindingArgs, "i")
						<< "	}\n";
					break;

				default:
					DE_ASSERT(0);
					break;
				}
			}
		}
	}
	else if (params.variant == TestVariant::MAX)
	{
		std::vector<deUint32> samplerIndices;
		std::vector<deUint32> imageIndices;

		for (deUint32 i = 0; i < u32(simpleBindings.size()); ++i)
		{
			const auto& binding = simpleBindings[i];

			if (binding.type == VK_DESCRIPTOR_TYPE_SAMPLER)
			{
				samplerIndices.emplace_back(i);
			}
			else if (binding.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
			{
				imageIndices.emplace_back(i);
			}
			// Ignore other descriptors, if any.
		}

		// Ensure that all samplers and images are accessed at least once. If we run out of one, simply reuse it.

		const auto maxIndex = deMaxu32(u32(samplerIndices.size()), u32(imageIndices.size()));

		for (deUint32 index = 0; index < maxIndex; ++index)
		{
			const auto& samplerBinding = simpleBindings[samplerIndices[index % samplerIndices.size()]];
			const auto& imageBinding   = simpleBindings[imageIndices[index % imageIndices.size()]];

			const auto expectedData		  = glslFormat(getExpectedData(params.hash, imageBinding.set, imageBinding.binding, 0));
			const auto imageBindingArgs   = glslFormat(packBindingArgs(imageBinding.set, imageBinding.binding, 0));
			const auto samplerBindingArgs = glslFormat(packBindingArgs(samplerBinding.set, samplerBinding.binding, 0));

			std::stringstream samplerStr;
			samplerStr << "usampler2D("
				<< glslResourceName(imageBinding.set, imageBinding.binding) << ", "
				<< glslResourceName(samplerBinding.set, samplerBinding.binding) << ")";

			str << "	if ((textureLod(" << samplerStr.str() << ", vec2(0, 0), 0).r == " << expectedData << ") &&\n"
				<< "	    (textureLod(" << samplerStr.str() << ", vec2(-1, 0), 0) == uvec4(0, 0, 0, 1))) "
				<< glslResultBlock("\t", imageBindingArgs, samplerBindingArgs);
		}
	}
	else if (params.variant == TestVariant::EMBEDDED_IMMUTABLE_SAMPLERS)
	{
		// The first few sets contain only samplers.
		// Then the last set contains only images (sampled or combined).
		// Optionally, the last binding of that set is the compute result buffer.

		deUint32 firstImageIndex = 0;
		deUint32 lastImageIndex = 0;

		for (deUint32 i = 0; i < u32(simpleBindings.size()); ++i)
		{
			const auto& binding = simpleBindings[i];

			if ((binding.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) ||
				(binding.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
			{
				if (firstImageIndex == 0)
				{
					firstImageIndex = i;
				}

				lastImageIndex = i;
			}
		}

		DE_ASSERT(firstImageIndex == (lastImageIndex + 1 - firstImageIndex));	// same number of images and samplers

		for (deUint32 imageIndex = firstImageIndex; imageIndex <= lastImageIndex; ++imageIndex)
		{
			const auto& imageBinding = simpleBindings[imageIndex];
			const auto expectedData	 = glslFormat(getExpectedData(params.hash, imageBinding.set, imageBinding.binding, 0));
			const auto bindingArgs	 = glslFormat(packBindingArgs(imageBinding.set, imageBinding.binding, 0));

			if (imageBinding.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
			{
				const auto& samplerBinding	  = simpleBindings[imageIndex - firstImageIndex];
				const auto samplerBindingArgs = glslFormat(packBindingArgs(samplerBinding.set, samplerBinding.binding, 0));

				std::stringstream samplerStr;
				samplerStr << "usampler2D("
					<< glslResourceName(imageBinding.set, imageBinding.binding) << ", "
					<< glslResourceName(samplerBinding.set, samplerBinding.binding) << ")";

				str << "	if ((textureLod(" << samplerStr.str() << ", vec2(0, 0), 0).r == " << expectedData << ") &&\n"
					<< "	    (textureLod(" << samplerStr.str() << ", vec2(-1, 0), 0) == uvec4(0, 0, 0, 1))) "
					<< glslResultBlock("\t", bindingArgs, samplerBindingArgs);
			}
			else if (imageBinding.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
			{
				str << "	if ((textureLod(" << glslResourceName(imageBinding.set, imageBinding.binding) << ", vec2(0, 0), 0).r == " << expectedData << ") &&\n"
					<< "	    (textureLod(" << glslResourceName(imageBinding.set, imageBinding.binding) << ", vec2(-1, 0), 0) == uvec4(0, 0, 0, 1))) "
					<< glslResultBlock("\t", bindingArgs);
			}
			else
			{
				DE_ASSERT(false);
			}
		}
	}
	else
	{
		TCU_THROW(InternalError, "Not implemented");
	}

	// Compute shaders write the result to a storage buffer.
	const deUint32 computeResultBufferIndex = getComputeResultBufferIndex(simpleBindings);

	if (computeResultBufferIndex != INDEX_INVALID)
	{
		DE_ASSERT(params.isCompute());
		const auto& resultSb = simpleBindings[computeResultBufferIndex];

		str << "	" << glslResourceName(resultSb.set, resultSb.binding) << ".data[0] = result.x;\n";
		str << "	" << glslResourceName(resultSb.set, resultSb.binding) << ".data[1] = result.y;\n";
		str << "	" << glslResourceName(resultSb.set, resultSb.binding) << ".data[2] = result.z;\n";
		str << "	" << glslResourceName(resultSb.set, resultSb.binding) << ".data[3] = result.w;\n";
	}

	return str.str();
}

// Base class for all test cases.
class DescriptorBufferTestCase : public TestCase
{
public:
	DescriptorBufferTestCase(
		tcu::TestContext& testCtx,
		const std::string& name,
		const std::string& description,
		const TestParams& params)

		: TestCase(testCtx, name, description)
		, m_params(params)
		, m_rng(params.hash)
	{
	}

	void			delayedInit		();
	void			initPrograms	(vk::SourceCollections& programCollection) const;
	TestInstance*	createInstance	(Context& context) const;
	void			checkSupport	(Context& context) const;

private:
	const TestParams			m_params;
	de::Random					m_rng;
	std::vector<SimpleBinding>	m_simpleBindings;
};

// Based on the basic test parameters, this function creates a number of sets/bindings that will be tested.
void DescriptorBufferTestCase::delayedInit()
{
	if (m_params.variant == TestVariant::SINGLE)
	{
		// Creates a single set with a single binding, unless additional helper resources are required.
		{
			SimpleBinding sb {};
			sb.set					= 0;
			sb.binding				= 0;
			sb.type					= m_params.descriptor;
			sb.count				= 1;

			// For inline uniforms we still use count = 1. The byte size is implicit in our tests.

			m_simpleBindings.emplace_back(sb);
		}

		// Sampled images require a sampler as well.
		if (m_params.descriptor == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
		{
			SimpleBinding sb {};
			sb.set					= 0;
			sb.binding				= u32(m_simpleBindings.size());
			sb.type					= VK_DESCRIPTOR_TYPE_SAMPLER;
			sb.count				= 1;

			m_simpleBindings.emplace_back(sb);
		}

		// For compute shaders add a result buffer as the last binding of the first set.
		if (m_params.isCompute())
		{
			SimpleBinding sb {};
			sb.set			  = 0;
			sb.binding		  = u32(m_simpleBindings.size());
			sb.type			  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			sb.count		  = 1;
			sb.isResultBuffer = true;

			m_simpleBindings.emplace_back(sb);
		}
	}
	else if ((m_params.variant == TestVariant::MULTIPLE) ||
			 (m_params.variant == TestVariant::PUSH_DESCRIPTOR) ||
			 (m_params.variant == TestVariant::PUSH_TEMPLATE))
	{
		// Generate a descriptor set for each descriptor buffer binding.
		// Within a set, add bindings for each descriptor type. Bindings may have 1-3 array elements.
		// In this test we include sampler descriptors, they will be used with sampled images, if needed.

		// NOTE: For implementation simplicity, this test doesn't limit the number of descriptors accessed
		// in the shaders, which may not work on some implementations.

		// Don't overcomplicate the test logic
		DE_ASSERT(!m_params.isPushDescriptorTest() || (m_params.setsPerBuffer == 1));

		// Add one more set for push descriptors (if used)
		const auto numSets = (m_params.bufferBindingCount * m_params.setsPerBuffer) + (m_params.isPushDescriptorTest() ? 1 : 0);

		deUint32 attachmentIndex = 0;

		// One set per buffer binding
		for (deUint32 set = 0; set < numSets; ++set)
		{
			std::vector<VkDescriptorType> choiceDescriptors;
			choiceDescriptors.emplace_back(VK_DESCRIPTOR_TYPE_SAMPLER);
			choiceDescriptors.emplace_back(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			choiceDescriptors.emplace_back(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
			choiceDescriptors.emplace_back(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
			choiceDescriptors.emplace_back(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
			choiceDescriptors.emplace_back(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
			choiceDescriptors.emplace_back(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
			choiceDescriptors.emplace_back(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

			if (!m_params.isPushDescriptorTest() || (set != m_params.pushDescriptorSetIndex))
			{
				choiceDescriptors.emplace_back(VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK);
			}

			if (m_params.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
			{
				choiceDescriptors.emplace_back(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
			}

			// Randomize the order
			m_rng.shuffle(choiceDescriptors.begin(), choiceDescriptors.end());

			for (deUint32 binding = 0; binding < u32(choiceDescriptors.size()); ++binding)
			{
				SimpleBinding sb {};
				sb.set		= set;
				sb.binding	= binding;
				sb.type		= choiceDescriptors[binding];
				sb.count	= 1 + ((sb.type != VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) ? m_rng.getUint32() % (ConstMaxDescriptorArraySize - 1) : 0);

				// For inline uniforms we still use count = 1. The byte size is implicit in our tests.

				if (sb.type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
				{
					sb.inputAttachmentIndex = attachmentIndex;
					attachmentIndex += sb.count;
				}

				m_simpleBindings.emplace_back(sb);
			}

			// For compute shaders add a result buffer as the last binding of the first set.
			if (m_params.isCompute() && (set == 0))
			{
				SimpleBinding sb {};
				sb.set			  = set;
				sb.binding		  = u32(m_simpleBindings.size());
				sb.type			  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				sb.count		  = 1;
				sb.isResultBuffer = true;

				m_simpleBindings.emplace_back(sb);
			}
		}
	}
	else if (m_params.variant == TestVariant::MAX)
	{
		// Create sampler- and resource-only sets, up to specified maxiumums.
		// Each set will get its own descriptor buffer binding.

		deUint32 set		  = 0;
		deUint32 samplerIndex = 0;
		deUint32 imageIndex	  = 0;

		for (;;)
		{
			SimpleBinding sb {};
			sb.binding	= 0;
			sb.count	= 1;
			sb.set      = set;	// save the original set index here

			if (samplerIndex < m_params.samplerBufferBindingCount)
			{
				sb.set  = set;
				sb.type	= VK_DESCRIPTOR_TYPE_SAMPLER;

				m_simpleBindings.emplace_back(sb);

				++set;
				++samplerIndex;
			}

			if (imageIndex < m_params.resourceBufferBindingCount)
			{
				sb.set  = set;
				sb.type	= VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;

				m_simpleBindings.emplace_back(sb);

				// Put the result buffer in the first resource set
				if (m_params.isCompute() && (imageIndex == 0))
				{
					sb.binding			= 1;
					sb.type				= VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
					sb.isResultBuffer	= true;

					m_simpleBindings.emplace_back(sb);
				}

				++set;
				++imageIndex;
			}

			if (sb.set == set)
			{
				// We didn't add a new set, so we must be done.
				break;
			}
		}
	}
	else if (m_params.variant == TestVariant::EMBEDDED_IMMUTABLE_SAMPLERS)
	{
		// Create a number of sampler-only sets across several descriptor buffers, they will be used as embedded
		// immutable sampler buffers. Finally, add a set with images that use these samplers.

		// Buffer index maps to a set with embedded immutable samplers
		for (deUint32 bufferIndex = 0; bufferIndex < m_params.embeddedImmutableSamplerBufferBindingCount; ++bufferIndex)
		{
			for (deUint32 samplerIndex = 0; samplerIndex < m_params.embeddedImmutableSamplersPerBuffer; ++samplerIndex)
			{
				SimpleBinding sb {};
				sb.set							= bufferIndex;
				sb.binding						= samplerIndex;
				sb.count						= 1;
				sb.type							= VK_DESCRIPTOR_TYPE_SAMPLER;
				sb.isEmbeddedImmutableSampler	= true;

				m_simpleBindings.emplace_back(sb);
			}
		}

		// After the samplers come the images
		if (!m_simpleBindings.empty())
		{
			SimpleBinding sb {};
			sb.set      = m_simpleBindings.back().set + 1;
			sb.count	= 1;

			const auto numSamplers = m_params.embeddedImmutableSamplerBufferBindingCount * m_params.embeddedImmutableSamplersPerBuffer;

			for (deUint32 samplerIndex = 0; samplerIndex < numSamplers; ++samplerIndex)
			{
				// Add a mix of sampled images and combined image samplers

				sb.type		= ((samplerIndex % 2) == 0) ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				sb.binding	= samplerIndex;

				m_simpleBindings.emplace_back(sb);
			}

			if (m_params.isCompute())
			{
				// Append the result buffer after the images
				sb.binding			+= 1;
				sb.type				= VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				sb.isResultBuffer	= true;

				m_simpleBindings.emplace_back(sb);
			}
		}
	}
}

// Initialize GLSL shaders used by all test cases.
void DescriptorBufferTestCase::initPrograms (vk::SourceCollections& programs) const
{
	// For vertex pipelines, a verification variable (in_result/out_result) is passed
	// through shader interfaces, until it can be output as a color write.
	//
	// Compute shaders still declare a "result" variable to help unify the verification logic.

	if (m_params.isGraphics())
	{
		std::string srcDeclarations;
		std::string srcVerification;

		if (m_params.stage == VK_SHADER_STAGE_VERTEX_BIT)
		{
			srcDeclarations = glslGlobalDeclarations(m_params, m_simpleBindings) + "\n";
			srcVerification = glslOutputVerification(m_params, m_simpleBindings) + "\n";
		}

		std::ostringstream str;
		str <<
			"#version 450 core\n"
			"\n"
			"layout(location = 0) out uvec4 out_result;\n"
			"\n"
			<< srcDeclarations <<
			"\n"
			"void main (void) {\n"
			"	switch(gl_VertexIndex) {\n"
			"		case 0: gl_Position = vec4(-1, -1, 0, 1); break;\n"
			"		case 1: gl_Position = vec4(-1,  1, 0, 1); break;\n"
			"		case 2: gl_Position = vec4( 1, -1, 0, 1); break;\n"
			"\n"
			"		case 3: gl_Position = vec4( 1,  1, 0, 1); break;\n"
			"		case 4: gl_Position = vec4( 1, -1, 0, 1); break;\n"
			"		case 5: gl_Position = vec4(-1,  1, 0, 1); break;\n"
			"	}\n"
			"\n"
			"	uvec4 result = uvec4(0);\n"
			"\n"
			<< srcVerification <<
			"\n"
			"	out_result = result;\n"
			"}\n";

		programs.glslSources.add("vert") << glu::VertexSource(str.str());
	}

	if (m_params.isGraphics())
	{
		std::string srcDeclarations;
		std::string srcVerification;

		if (m_params.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
		{
			srcDeclarations = glslGlobalDeclarations(m_params, m_simpleBindings) + "\n";
			srcVerification = glslOutputVerification(m_params, m_simpleBindings) + "\n";
		}

		std::ostringstream str;
		str <<
			"#version 450 core\n"
			"\n"
			"layout(location = 0) in flat uvec4 in_result;\n"
			"\n"
			"layout(location = 0) out uint out_color;\n"
			"\n"
			<< srcDeclarations <<
			"\n"
			"void main (void) {\n"
			"	uvec4 result = in_result;\n"
			"\n"
			<< srcVerification <<
			"\n"
			"   if (uint(gl_FragCoord.x) == 0)	out_color = result.x;\n"
			"   if (uint(gl_FragCoord.x) == 1)	out_color = result.y;\n"
			"   if (uint(gl_FragCoord.x) == 2)	out_color = result.z;\n"
			"   if (uint(gl_FragCoord.x) == 3)	out_color = result.w;\n"
			"}\n";

		programs.glslSources.add("frag") << glu::FragmentSource(str.str());
	}

	if (m_params.isGeometry())
	{
		std::ostringstream str;
		str <<
			"#version 450 core\n"
			"\n"
			"layout(triangles) in;\n"
			"layout(triangle_strip, max_vertices = 3) out;\n"
			"\n"
			"layout(location = 0) in  uvec4 in_result[];\n"
			"layout(location = 0) out uvec4 out_result;\n"
			"\n"
			<< glslGlobalDeclarations(m_params, m_simpleBindings) <<
			"\n"
			"void main (void) {\n"
			"   for (uint i = 0; i < gl_in.length(); ++i) {\n"
			"		gl_Position = gl_in[i].gl_Position;\n"
			"\n"
			"		uvec4 result = in_result[i];\n"
			"\n"
			<< glslOutputVerification(m_params, m_simpleBindings) <<
			"\n"
			"		out_result = result;\n"
			"\n"
			"		EmitVertex();\n"
			"	}\n"
			"}\n";

		programs.glslSources.add("geom") << glu::GeometrySource(str.str());
	}

	if (m_params.isTessellation())
	{
		std::string srcDeclarations;
		std::string srcVerification;

		if (m_params.stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
		{
			srcDeclarations = glslGlobalDeclarations(m_params, m_simpleBindings) + "\n";
			srcVerification = glslOutputVerification(m_params, m_simpleBindings) + "\n";
		}

		std::ostringstream str;
		str <<
			"#version 450 core\n"
			"\n"
			"layout(vertices = 3) out;\n"
			"\n"
			"layout(location = 0) in  uvec4 in_result[];\n"
			"layout(location = 0) out uvec4 out_result[];\n"
			"\n"
			<< srcDeclarations <<
			"\n"
			"void main (void) {\n"
			"	gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
			"	\n"
			"	gl_TessLevelOuter[0] = 1.0;\n"
			"	gl_TessLevelOuter[1] = 1.0;\n"
			"	gl_TessLevelOuter[2] = 1.0;\n"
			"	gl_TessLevelInner[0] = 1.0;\n"
			"\n"
			"   uvec4 result = in_result[gl_InvocationID];\n"
			"\n"
			<< srcVerification <<
			"\n"
			"	out_result[gl_InvocationID] = result;\n"
			"}\n";

		programs.glslSources.add("tess_cont") << glu::TessellationControlSource(str.str());
	}

	if (m_params.isTessellation())
	{
		std::string srcDeclarations;
		std::string srcVerification;

		if (m_params.stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
		{
			srcDeclarations = glslGlobalDeclarations(m_params, m_simpleBindings) + "\n";
			srcVerification = glslOutputVerification(m_params, m_simpleBindings) + "\n";
		}

		std::ostringstream str;
		str <<
			"#version 450 core\n"
			"\n"
			"layout(triangles) in;\n"
			"\n"
			"layout(location = 0) in  uvec4 in_result[];\n"
			"layout(location = 0) out uvec4 out_result;\n"
			"\n"
			<< srcDeclarations <<
			"\n"
			"void main (void) {\n"
			"	gl_Position.xyz = gl_TessCoord.x * gl_in[0].gl_Position.xyz +\n"
			"	                  gl_TessCoord.y * gl_in[1].gl_Position.xyz +\n"
			"	                  gl_TessCoord.z * gl_in[2].gl_Position.xyz;\n"
			"   gl_Position.w   = 1.0;\n"
			"\n"
			"   uvec4 result = in_result[0];\n"	// Use index 0, all vertices should have the same value
			"\n"
			<< srcVerification <<
			"\n"
			"	out_result = result;\n"
			"}\n";

		programs.glslSources.add("tess_eval") << glu::TessellationEvaluationSource(str.str());
	}

	if (m_params.isCompute())
	{
		std::ostringstream str;
		str <<
			"#version 450 core\n"
			"layout(local_size_x = 1) in;\n"
			"\n"
			<< glslGlobalDeclarations(m_params, m_simpleBindings) <<
			"\n"
			"void main (void) {\n"
			"   uvec4 result = uvec4(0);\n"
			"\n"
			<< glslOutputVerification(m_params, m_simpleBindings) <<
			"}\n";

		programs.glslSources.add("comp") << glu::ComputeSource(str.str());
	}
}

void DescriptorBufferTestCase::checkSupport (Context& context) const
{
	// Required to test the extension

	if (!context.isInstanceFunctionalitySupported("VK_KHR_get_physical_device_properties2"))
	{
		TCU_THROW(NotSupportedError, "VK_KHR_get_physical_device_properties2 is not supported");
	}

	if (!context.isDeviceFunctionalitySupported("VK_EXT_descriptor_buffer"))
	{
		TCU_THROW(NotSupportedError, "VK_EXT_descriptor_buffer is not supported");
	}

	if (!context.isDeviceFunctionalitySupported("VK_KHR_buffer_device_address"))
	{
		TCU_THROW(NotSupportedError, "VK_KHR_buffer_device_address is not supported");
	}

	if (!context.isDeviceFunctionalitySupported("VK_KHR_synchronization2"))
	{
		TCU_THROW(NotSupportedError, "VK_KHR_synchronization2 is not supported");
	}

	// Optional

	if ((m_params.descriptor == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) &&
		!context.isDeviceFunctionalitySupported("VK_EXT_inline_uniform_block"))
	{
		TCU_THROW(NotSupportedError, "VK_EXT_inline_uniform_block is not supported");
	}

	const auto& features = *findStructure<VkPhysicalDeviceDescriptorBufferFeaturesEXT>(&context.getDeviceFeatures2());
	const auto& props	 = *findStructure<VkPhysicalDeviceDescriptorBufferPropertiesEXT>(&context.getDeviceProperties2());

	if ((m_params.variant == TestVariant::CAPTURE_REPLAY) &&
		(features.descriptorBufferCaptureReplay == VK_FALSE))
	{
		TCU_THROW(NotSupportedError, "descriptorBufferCaptureReplay feature is not supported");
	}

	if (m_params.isTessellation() &&
		(context.getDeviceFeatures().tessellationShader == VK_FALSE))
	{
		TCU_THROW(NotSupportedError, "tessellationShader feature is not supported");
	}
	else if (m_params.isGeometry() &&
		(context.getDeviceFeatures().geometryShader == VK_FALSE))
	{
		TCU_THROW(NotSupportedError, "geometryShader feature is not supported");
	}

	// Test case specific

	if (m_params.isPushDescriptorTest())
	{
		if (!context.isDeviceFunctionalitySupported("VK_KHR_push_descriptor"))
		{
			TCU_THROW(NotSupportedError, "VK_KHR_push_descriptor is not supported");
		}
		else if (props.pushDescriptorsRequireBuffer == VK_TRUE)
		{
			DE_ASSERT(0);	// TODO
			TCU_THROW(NotSupportedError, "Test does not support pushDescriptorsRequireBuffer");
		}
	}

	if (m_params.bufferBindingCount > props.maxDescriptorBufferBindings)
	{
		TCU_THROW(NotSupportedError, "maxDescriptorBufferBindings is too small");
	}

	if (m_params.samplerBufferBindingCount > props.maxSamplerDescriptorBufferBindings)
	{
		TCU_THROW(NotSupportedError, "maxSamplerDescriptorBufferBindings is too small");
	}

	if (m_params.resourceBufferBindingCount > props.maxResourceDescriptorBufferBindings)
	{
		TCU_THROW(NotSupportedError, "maxResourceDescriptorBufferBindings is too small");
	}
}

// The base class for all test case implementations.
class DescriptorBufferTestInstance : public TestInstance
{
public:
	DescriptorBufferTestInstance(
		Context&							context,
		const TestParams&					params,
		const std::vector<SimpleBinding>&	simpleBindings);

	tcu::TestStatus	iterate() override;

	void createGraphicsPipeline();
	void createDescriptorSetLayouts();
	void createDescriptorBuffers();

	void initializeBinding(
		const DescriptorSetLayoutHolder&	dsl,
		deUint32							setIndex,
		Binding&							binding);

	void pushDescriptorSet(
		VkCommandBuffer						cmdBuf,
		VkPipelineBindPoint					bindPoint,
		const DescriptorSetLayoutHolder&	dsl,
		deUint32							setIndex) const;

	void bindDescriptorBuffers(
		VkCommandBuffer						cmdBuf,
		VkPipelineBindPoint					bindPoint) const;

	deUint32 addDescriptorSetLayout()
	{
		m_descriptorSetLayouts.emplace_back(makeSharedUniquePtr<DescriptorSetLayoutHolder>());
		return u32(m_descriptorSetLayouts.size()) - 1;
	}

	// The resources used by descriptors are tracked in a simple array and referenced by an index.
	deUint32 addResource()
	{
		m_resources.emplace_back(makeSharedUniquePtr<ResourceHolder>());
		return u32(m_resources.size()) - 1;
	}

	const ProgramBinary& getShaderBinary(const std::string& name) const
	{
		return m_context.getBinaryCollection().get(name);
	}

	// Descriptor size is used to determine the stride of a descriptor array (for bindings with multiple descriptors).
	VkDeviceSize getDescriptorSize(VkDescriptorType type) const
	{
		const auto isRobust = (m_params.variant == TestVariant::ROBUSTNESS);

		switch (type) {
		case VK_DESCRIPTOR_TYPE_SAMPLER:
			return m_descriptorBufferProperties.samplerDescriptorSize;

		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			return m_descriptorBufferProperties.combinedImageSamplerDescriptorSize;

		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
			return m_descriptorBufferProperties.sampledImageDescriptorSize;

		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			return m_descriptorBufferProperties.storageImageDescriptorSize;

		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
			return isRobust ? m_descriptorBufferProperties.robustUniformTexelBufferDescriptorSize
							: m_descriptorBufferProperties.uniformTexelBufferDescriptorSize;

		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			return isRobust ? m_descriptorBufferProperties.robustStorageTexelBufferDescriptorSize
							: m_descriptorBufferProperties.storageTexelBufferDescriptorSize;

		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			return isRobust ? m_descriptorBufferProperties.robustUniformBufferDescriptorSize
							: m_descriptorBufferProperties.uniformBufferDescriptorSize;

		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			return isRobust ? m_descriptorBufferProperties.robustStorageBufferDescriptorSize
							: m_descriptorBufferProperties.storageBufferDescriptorSize;

		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
			return m_descriptorBufferProperties.inputAttachmentDescriptorSize;

		case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
			return m_descriptorBufferProperties.accelerationStructureDescriptorSize;

		case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
			// Inline uniform block has no associated size. This is OK, because it can't be arrayed.
			break;

		default:
			DE_ASSERT(0);
			break;
		}

		return 0;
	}

protected:
	// Test cases using compute shaders always declare one binding with a result buffer.
	const BufferAlloc& getComputeResultBuffer() const
	{
		DE_ASSERT(m_params.isCompute());

		const deUint32 computeResultBufferIndex = getComputeResultBufferIndex(m_simpleBindings);
		const auto& sb = m_simpleBindings[computeResultBufferIndex];

		const auto binding = std::find_if(
			(**m_descriptorSetLayouts[sb.set]).bindings.begin(),
			(**m_descriptorSetLayouts[sb.set]).bindings.end(),
			[&sb](const Binding& it){ return it.binding.binding == sb.binding; });

		DE_ASSERT(binding->binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

		// There's only one result buffer at this binding
		return (**m_resources[binding->perBindingResourceIndex[0]]).buffer;
	}


	const TestParams					m_params;
	const std::vector<SimpleBinding>	m_simpleBindings;

	Move<VkDevice>						m_device;
	MovePtr<DeviceDriver>				m_deviceInterface;
	VkQueue								m_queue;
	deUint32							m_queueFamilyIndex;
	MovePtr<Allocator>					m_allocator;

	VkPhysicalDeviceMemoryProperties				m_memoryProperties {};
	VkPhysicalDeviceDescriptorBufferFeaturesEXT		m_descriptorBufferFeatures {};
	VkPhysicalDeviceDescriptorBufferPropertiesEXT	m_descriptorBufferProperties {};

	Move<VkPipeline>					m_pipeline;
	Move<VkPipelineLayout>				m_pipelineLayout;

	// Optional, for graphics pipelines
	Move<VkFramebuffer>					m_framebuffer;
	Move<VkRenderPass>					m_renderPass;
	VkRect2D							m_renderArea = makeRect2D(0, 0, 4, 1);
	ImageAlloc							m_colorImage;
	BufferAlloc							m_colorBuffer;	// for copying back to host visible memory

	std::vector<DSLPtr>					m_descriptorSetLayouts;
	std::vector<BufferAllocPtr>			m_descriptorBuffers;
	BufferAlloc							m_descriptorStagingBuffer;

	std::vector<ResourcePtr>			m_resources;	// various resources used to test the descriptors
};

DescriptorBufferTestInstance::DescriptorBufferTestInstance(
	Context&							context,
	const TestParams&					params,
	const std::vector<SimpleBinding>&	simpleBindings)

	: TestInstance(context)
	, m_params(params)
	, m_simpleBindings(simpleBindings)
{
	// Need to create a new device because:
	// - We want to test graphics and compute queues,
	// - We must exclude VK_AMD_shader_fragment_mask from the enabled extensions.

	auto& inst		 = context.getInstanceInterface();
	auto  physDevice = context.getPhysicalDevice();

	auto queueProps = getPhysicalDeviceQueueFamilyProperties(inst, physDevice);

	m_queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	for (deUint32 i = 0; i < queueProps.size(); ++i)
	{
		if (params.queue == VK_QUEUE_GRAPHICS_BIT)
		{
			if ((queueProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
			{
				m_queueFamilyIndex = i;

				break;
			}
		}
		else if (params.queue == VK_QUEUE_COMPUTE_BIT)
		{
			if (((queueProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) &&
				((queueProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0))
			{
				m_queueFamilyIndex = i;
			}
		}
	}

	if (m_queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED)
	{
		TCU_THROW(NotSupportedError, "Queue not supported");
	}

	const float priority[1] = { 0.5f };

	VkDeviceQueueCreateInfo queueInfo = initVulkanStructure();
	queueInfo.queueFamilyIndex = m_queueFamilyIndex;
	queueInfo.queueCount       = 1;
	queueInfo.pQueuePriorities = priority;

	// NOTE: VK_AMD_shader_fragment_mask must not be enabled
	std::vector<const char*> extensions;
	extensions.push_back("VK_EXT_descriptor_buffer");
	extensions.push_back("VK_KHR_buffer_device_address");
	extensions.push_back("VK_KHR_synchronization2");

	if (params.descriptor == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
	{
		extensions.push_back("VK_EXT_inline_uniform_block");
	}

	if (params.isPushDescriptorTest())
	{
		extensions.push_back("VK_KHR_push_descriptor");
	}

	auto features2                = *(&context.getDeviceFeatures2());
	auto features13               = *findStructure<VkPhysicalDeviceVulkan13Features>(&features2);
	auto descriptorBufferFeatures = *findStructure<VkPhysicalDeviceDescriptorBufferFeaturesEXT>(&features2);

	// Skip unused features
	features2.pNext                = &features13;
	features13.pNext               = &descriptorBufferFeatures;
	descriptorBufferFeatures.pNext = nullptr;

	m_descriptorBufferFeatures		 = descriptorBufferFeatures;
	m_descriptorBufferFeatures.pNext = nullptr;

	m_descriptorBufferProperties = *findStructure<VkPhysicalDeviceDescriptorBufferPropertiesEXT>(&context.getDeviceProperties2());
	m_descriptorBufferProperties.pNext = nullptr;

	if (params.variant == TestVariant::ROBUSTNESS)
	{
		features2.features.robustBufferAccess = VK_TRUE;
	}

	// Should be enabled by default
	DE_ASSERT(descriptorBufferFeatures.descriptorBuffer);
	DE_ASSERT(features13.synchronization2);
	DE_ASSERT(features13.inlineUniformBlock);

	VkDeviceCreateInfo createInfo = initVulkanStructure(&features2);
	createInfo.pEnabledFeatures		   = DE_NULL;
	createInfo.enabledExtensionCount   = u32(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();
	createInfo.queueCreateInfoCount    = 1;
	createInfo.pQueueCreateInfos       = &queueInfo;

	m_device = createCustomDevice(
		false,
		context.getPlatformInterface(),
		context.getInstance(),
		inst,
		physDevice,
		&createInfo);

	context.getDeviceInterface().getDeviceQueue(
		*m_device,
		m_queueFamilyIndex,
		0,
		&m_queue);

	m_deviceInterface = newMovePtr<DeviceDriver>(context.getPlatformInterface(), context.getInstance(), *m_device);

	m_memoryProperties = vk::getPhysicalDeviceMemoryProperties(inst, physDevice);
	m_allocator = newMovePtr<SimpleAllocator>(*m_deviceInterface, *m_device, m_memoryProperties);
}

void DescriptorBufferTestInstance::createDescriptorSetLayouts()
{
	for (auto& dslPtr : m_descriptorSetLayouts)
	{
		auto& dsl = **dslPtr;

		DE_ASSERT(!dsl.bindings.empty());

		const auto bindingsCopy = getDescriptorSetLayoutBindings(dsl.bindings);

		VkDescriptorSetLayoutCreateInfo createInfo = initVulkanStructure();
		createInfo.bindingCount = u32(dsl.bindings.size());
		createInfo.pBindings    = bindingsCopy.data();
		createInfo.flags		= VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;

		if (dsl.hasEmbeddedImmutableSamplers)
		{
			createInfo.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_EMBEDDED_IMMUTABLE_SAMPLERS_BIT_EXT;
		}
		else if (dsl.usePushDescriptors)
		{
			createInfo.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
		}

		dsl.layout = createDescriptorSetLayout(*m_deviceInterface, *m_device, &createInfo);

		VK_CHECK(m_deviceInterface->getDescriptorSetLayoutSizeEXT(*m_device, *dsl.layout, &dsl.size));

		for (auto& binding : dsl.bindings)
		{
			VK_CHECK(m_deviceInterface->getDescriptorSetLayoutBindingOffsetEXT(
				*m_device,
				*dsl.layout,
				binding.binding.binding,
				&binding.offset));
		}
	}
}

// The test may create a variable number of descriptor buffers, based on the parameters.
void DescriptorBufferTestInstance::createDescriptorBuffers()
{
	DE_ASSERT(m_descriptorBuffers.empty());

	bool			allocateStagingBuffer = false;		// determined after descriptors are created
	VkDeviceSize	stagingBufferDescriptorSetOffset = 0;

	// Data tracked per buffer creation
	struct {
		deUint32			firstSet;
		deUint32			numSets;
		VkBufferUsageFlags	usage;
		VkDeviceSize		setOffset;
	} currentBuffer;

	currentBuffer = {};

	for (deUint32 setIndex = 0; setIndex < u32(m_descriptorSetLayouts.size()); ++setIndex)
	{
		auto& dsl = **m_descriptorSetLayouts[setIndex];

		if (dsl.hasEmbeddedImmutableSamplers || dsl.usePushDescriptors)
		{
			// Embedded immutable samplers aren't backed by a descriptor buffer.
			// Same goes for the set used with push descriptors.

			// We musn't have started adding sets to the next buffer yet.
			DE_ASSERT(currentBuffer.numSets == 0);
			++currentBuffer.firstSet;

			continue;
		}

		// Required for binding
		currentBuffer.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		for (const auto& binding : dsl.bindings)
		{
			if (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER)
			{
				currentBuffer.usage |= VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
			}
			else if (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
			{
				currentBuffer.usage |= VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
									   VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
			}
			else
			{
				currentBuffer.usage |= VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
			}
		}

		// Assign this descriptor set to a new buffer
		dsl.bufferIndex  = u32(m_descriptorBuffers.size());
		dsl.bufferOffset = currentBuffer.setOffset;

		currentBuffer.numSets   += 1;
		currentBuffer.setOffset += deAlignSize(
			static_cast<std::size_t>(dsl.size),
			static_cast<std::size_t>(m_descriptorBufferProperties.descriptorBufferOffsetAlignment));

		// We've reached the limit of sets for this descriptor buffer.
		if (currentBuffer.numSets == m_params.setsPerBuffer)
		{
			auto bufferCreateInfo = makeBufferCreateInfo(currentBuffer.setOffset, currentBuffer.usage);

			m_descriptorBuffers.emplace_back(new BufferAlloc());
			auto& bufferAlloc = *m_descriptorBuffers.back();

			bufferAlloc.buffer = vk::createBuffer(*m_deviceInterface, *m_device, &bufferCreateInfo);
			bufferAlloc.size   = bufferCreateInfo.size;
			bufferAlloc.usage  = bufferCreateInfo.usage;

			auto bufferMemReqs = getBufferMemoryRequirements(*m_deviceInterface, *m_device, *bufferAlloc.buffer);
			bool useStagedUpload = false;	// write directly to device-local memory, if possible

			if (DEBUG_FORCE_STAGED_UPLOAD)
			{
				useStagedUpload = true;
			}
			else if (DEBUG_MIX_DIRECT_AND_STAGED_UPLOAD)
			{
				// To avoid adding yet another test case permutation (which may be redundant on some implementations),
				// we are going to always test a mix of direct and staged uploads.
				useStagedUpload = ((dsl.bufferIndex % 2) == 1);
			}

			if (!useStagedUpload)
			{
				auto memReqs = MemoryRequirement::Local | MemoryRequirement::HostVisible;
				auto compatMask = bufferMemReqs.memoryTypeBits & getCompatibleMemoryTypes(m_memoryProperties, memReqs);

				if (compatMask != 0)
				{
					bufferAlloc.alloc = m_allocator->allocate(bufferMemReqs, memReqs);
				}
				else
				{
					// No suitable memory type, fall back to a staged upload
					useStagedUpload = true;
				}
			}

			if (useStagedUpload)
			{
				DE_ASSERT(!bufferAlloc.alloc);

				bufferAlloc.alloc = m_allocator->allocate(bufferMemReqs, MemoryRequirement::Local);
				allocateStagingBuffer = true;

				// Update staging buffer offsets for all sets in this buffer
				for (deUint32 i = currentBuffer.firstSet; i < currentBuffer.firstSet + currentBuffer.numSets; ++i)
				{
					(**m_descriptorSetLayouts[i]).stagingBufferOffset = stagingBufferDescriptorSetOffset;
					stagingBufferDescriptorSetOffset += (**m_descriptorSetLayouts[i]).size;
				}
			}

			VK_CHECK(m_deviceInterface->bindBufferMemory(
				*m_device,
				*bufferAlloc.buffer,
				bufferAlloc.alloc->getMemory(),
				bufferAlloc.alloc->getOffset()));

			bufferAlloc.loadDeviceAddress(*m_deviceInterface, *m_device);

			// Start with a new buffer
			currentBuffer = {};
			currentBuffer.firstSet = setIndex + 1;
		}
	}

	if (allocateStagingBuffer)
	{
		DE_ASSERT(!m_descriptorStagingBuffer.alloc);

		auto bufferCreateInfo = makeBufferCreateInfo(stagingBufferDescriptorSetOffset, (VkBufferUsageFlags)0);

		m_descriptorStagingBuffer.buffer = vk::createBuffer(*m_deviceInterface, *m_device, &bufferCreateInfo);
		m_descriptorStagingBuffer.size = bufferCreateInfo.size;

		auto bufferMemReqs = getBufferMemoryRequirements(*m_deviceInterface, *m_device, *m_descriptorStagingBuffer.buffer);

		m_descriptorStagingBuffer.alloc = m_allocator->allocate(bufferMemReqs, MemoryRequirement::HostVisible);

		VK_CHECK(m_deviceInterface->bindBufferMemory(
			*m_device,
			*m_descriptorStagingBuffer.buffer,
			m_descriptorStagingBuffer.alloc->getMemory(),
			m_descriptorStagingBuffer.alloc->getOffset()));
	}
}

void DescriptorBufferTestInstance::bindDescriptorBuffers(VkCommandBuffer cmdBuf, VkPipelineBindPoint bindPoint) const
{
	std::vector<deUint32>							bufferIndices;
	std::vector<VkDeviceSize>						bufferOffsets;
	std::vector<VkDescriptorBufferBindingInfoEXT>	bufferBindingInfos;

	deUint32 bindLimit	 = 0;	// max number of descriptor buffers to bind in one API call
	deUint32 setLimit	 = 0;	// max number of descriptor set offsets to set in one API call
	deUint32 nextBuffer  = 0;	// index of the next buffer to bind
	deUint32 firstBuffer = 0;
	deUint32 firstSet    = 0;

	if (m_params.subcase == SubCase::INCREMENTAL_BIND)
	{
		// Artificially break up the bind/offset commands to ensure that calling them multiple times is also working.
		bindLimit = (m_descriptorBuffers.size() > 2) ? 2 : 1;
		setLimit  = deMaxu32(1, m_params.setsPerBuffer / 2);
	}
	else
	{
		bindLimit = u32(m_descriptorBuffers.size());
		setLimit  = u32(m_descriptorSetLayouts.size());
	}

	if (m_params.variant == TestVariant::EMBEDDED_IMMUTABLE_SAMPLERS)
	{
		// These sampler sets are ordered first, so we can bind them now and increment the firstSet index.
		for (deUint32 setIndex = firstSet; setIndex < u32(m_descriptorSetLayouts.size()); ++setIndex)
		{
			const auto& dsl = **m_descriptorSetLayouts[setIndex];

			if (dsl.hasEmbeddedImmutableSamplers)
			{
				m_deviceInterface->cmdBindDescriptorBufferEmbeddedSamplersEXT(
					cmdBuf,
					bindPoint,
					*m_pipelineLayout,
					setIndex);

				// No gaps between sets.
				DE_ASSERT(firstSet == setIndex);

				firstSet = setIndex + 1;
			}
		}
	}

	for (;;)
	{
		const bool isWithinBufferLimit = (u32(bufferBindingInfos.size()) < bindLimit);
		const bool hasMoreBuffers	   = (nextBuffer < u32(m_descriptorBuffers.size()));

		if (isWithinBufferLimit && hasMoreBuffers)
		{
			const auto& buffer = m_descriptorBuffers[nextBuffer];

			VkDescriptorBufferBindingInfoEXT info = initVulkanStructure();

			info.address = buffer->deviceAddress;
			info.usage   = buffer->usage;

			bufferBindingInfos.emplace_back(info);

			++nextBuffer;
		}
		else
		{
			m_deviceInterface->cmdBindDescriptorBuffersEXT(
				cmdBuf,
				bindPoint,
				firstBuffer,
				u32(bufferBindingInfos.size()),
				bufferBindingInfos.data());

			bufferBindingInfos.clear();

			firstBuffer = nextBuffer;

			// Proceed to setting the offsets for the bound buffers.

			for (deUint32 setIndex = firstSet; setIndex < u32(m_descriptorSetLayouts.size()); ++setIndex)
			{
				const auto& dsl		  = **m_descriptorSetLayouts[setIndex];
				const bool  isBound	  = (dsl.bufferIndex < nextBuffer);
				const bool  isLastSet = ((setIndex + 1) == u32(m_descriptorSetLayouts.size()));

				bool isWithinLimit = (u32(bufferIndices.size()) < setLimit);
				bool isAddedSet	   = false;

				if (isBound && isWithinLimit)
				{
					bufferIndices.emplace_back(dsl.bufferIndex);
					bufferOffsets.emplace_back(dsl.bufferOffset);

					isWithinLimit = (u32(bufferIndices.size()) < setLimit);
					isAddedSet	  = true;
				}

				if (!isAddedSet || isLastSet || !isWithinLimit)
				{
					if (!bufferIndices.empty())
					{
						m_deviceInterface->cmdSetDescriptorBufferOffsetsEXT(
							cmdBuf,
							bindPoint,
							*m_pipelineLayout,
							firstSet,
							u32(bufferIndices.size()),
							bufferIndices.data(),
							bufferOffsets.data());

						bufferIndices.clear();
						bufferOffsets.clear();

						firstSet = setIndex + (isAddedSet ? 1 : 0);
					}

					if (dsl.bufferIndex == INDEX_INVALID)
					{
						// This set doesn't use buffer binding, skip it.
						++firstSet;
					}
					else if (!isBound)
					{
						// This set and subsequent sets aren't bound yet. Exit early and try again.
						break;
					}
				}
			}

			if (nextBuffer >= u32(m_descriptorBuffers.size()))
			{
				// We've bound all buffers.
				break;
			}
		}
	}
}

VkPipelineShaderStageCreateInfo makeShaderStageCreateInfo(VkShaderStageFlagBits stage, VkShaderModule shaderModule)
{
	VkPipelineShaderStageCreateInfo createInfo = initVulkanStructure();
	createInfo.stage				= stage;
	createInfo.module				= shaderModule;
	createInfo.pName				= "main";
	createInfo.pSpecializationInfo	= nullptr;
	return createInfo;
}

// The graphics pipeline is very simple for this test.
// The number of shader stages is configurable. There's no vertex input, a single triangle covers the entire viewport.
// The color target uses R32_UINT format and is used to save the verifcation result.
void DescriptorBufferTestInstance::createGraphicsPipeline()
{
	std::vector<VkImageView> framebufferAttachments;

	{
		DE_ASSERT(!m_colorImage.alloc);

		m_colorImage.info = initVulkanStructure();
		m_colorImage.info.flags					= 0;
		m_colorImage.info.imageType				= VK_IMAGE_TYPE_2D;
		m_colorImage.info.format				= VK_FORMAT_R32_UINT;
		m_colorImage.info.extent.width			= m_renderArea.extent.width;
		m_colorImage.info.extent.height			= m_renderArea.extent.height;
		m_colorImage.info.extent.depth			= 1;
		m_colorImage.info.mipLevels				= 1;
		m_colorImage.info.arrayLayers			= 1;
		m_colorImage.info.samples				= VK_SAMPLE_COUNT_1_BIT;
		m_colorImage.info.tiling				= VK_IMAGE_TILING_OPTIMAL;
		m_colorImage.info.usage					= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		m_colorImage.info.sharingMode			= VK_SHARING_MODE_EXCLUSIVE;
		m_colorImage.info.queueFamilyIndexCount	= 0;
		m_colorImage.info.pQueueFamilyIndices	= nullptr;
		m_colorImage.info.initialLayout			= VK_IMAGE_LAYOUT_UNDEFINED;

		m_colorImage.image = createImage(*m_deviceInterface, *m_device, &m_colorImage.info);

		auto memReqs = getImageMemoryRequirements(*m_deviceInterface, *m_device, *m_colorImage.image);
		m_colorImage.sizeBytes = memReqs.size;
		m_colorImage.alloc	   = m_allocator->allocate(memReqs, MemoryRequirement::Local);

		VK_CHECK(m_deviceInterface->bindImageMemory(
			*m_device,
			*m_colorImage.image,
			m_colorImage.alloc->getMemory(),
			m_colorImage.alloc->getOffset()));
	}
	{
		auto createInfo = makeBufferCreateInfo(
			m_colorImage.sizeBytes,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT);

		m_colorBuffer.buffer = createBuffer(*m_deviceInterface, *m_device, &createInfo);

		auto memReqs = getBufferMemoryRequirements(*m_deviceInterface, *m_device, *m_colorBuffer.buffer);

		m_colorBuffer.alloc = m_allocator->allocate(memReqs, MemoryRequirement::HostVisible);
		VK_CHECK(m_deviceInterface->bindBufferMemory(
			*m_device,
			*m_colorBuffer.buffer,
			m_colorBuffer.alloc->getMemory(),
			m_colorBuffer.alloc->getOffset()));
	}
	{
		VkImageViewCreateInfo createInfo = initVulkanStructure();
		createInfo.image			= *m_colorImage.image;
		createInfo.viewType			= VK_IMAGE_VIEW_TYPE_2D;
		createInfo.format			= m_colorImage.info.format;
		createInfo.components		= makeComponentMappingRGBA();
		createInfo.subresourceRange = makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);

		m_colorImage.imageView = createImageView(*m_deviceInterface, *m_device, &createInfo);
	}

	framebufferAttachments.push_back(*m_colorImage.imageView);

	{
		std::vector<VkAttachmentDescription> attachments;
		std::vector<VkAttachmentReference>	 colorRefs;
		std::vector<VkAttachmentReference>	 inputRefs;

		{
			VkAttachmentDescription colorAttachment {};
			colorAttachment.format			= VK_FORMAT_R32_UINT;
			colorAttachment.samples			= VK_SAMPLE_COUNT_1_BIT;
			colorAttachment.loadOp			= VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp			= VK_ATTACHMENT_STORE_OP_STORE;
			colorAttachment.stencilLoadOp	= VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.stencilStoreOp	= VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.initialLayout	= VK_IMAGE_LAYOUT_UNDEFINED;
			colorAttachment.finalLayout		= VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

			colorRefs.emplace_back(makeAttachmentReference(u32(attachments.size()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
			attachments.emplace_back(colorAttachment);
		}

		for (deUint32 setIndex = 0; setIndex < u32(m_descriptorSetLayouts.size()); ++setIndex)
		{
			const auto& dsl = **m_descriptorSetLayouts[setIndex];

			for (deUint32 bindingIndex = 0; bindingIndex < u32(dsl.bindings.size()); ++bindingIndex)
			{
				const auto& binding = dsl.bindings[bindingIndex];

				if (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
				{
					for (deUint32 arrayIndex = 0; arrayIndex < binding.binding.descriptorCount; ++arrayIndex)
					{
						VkAttachmentDescription inputAttachment {};
						inputAttachment.format			= VK_FORMAT_R32_UINT;
						inputAttachment.samples			= VK_SAMPLE_COUNT_1_BIT;
						inputAttachment.loadOp			= VK_ATTACHMENT_LOAD_OP_LOAD;
						inputAttachment.storeOp			= VK_ATTACHMENT_STORE_OP_DONT_CARE;
						inputAttachment.stencilLoadOp	= VK_ATTACHMENT_LOAD_OP_DONT_CARE;
						inputAttachment.stencilStoreOp	= VK_ATTACHMENT_STORE_OP_DONT_CARE;
						inputAttachment.initialLayout	= VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
						inputAttachment.finalLayout		= VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

						inputRefs.emplace_back(makeAttachmentReference(u32(attachments.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
						attachments.emplace_back(inputAttachment);

						const auto inputAttachmentResourceIndex = binding.perBindingResourceIndex[arrayIndex];
						framebufferAttachments.push_back(*(**m_resources[inputAttachmentResourceIndex]).image.imageView);
					}
				}
			}
		}

		VkSubpassDescription subpass {};
		subpass.pipelineBindPoint		= VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.inputAttachmentCount	= u32(inputRefs.size());
		subpass.pInputAttachments		= inputRefs.data();
		subpass.colorAttachmentCount	= u32(colorRefs.size());
		subpass.pColorAttachments		= colorRefs.data();
		subpass.pResolveAttachments		= nullptr;
		subpass.pDepthStencilAttachment	= nullptr;
		subpass.preserveAttachmentCount	= 0;
		subpass.pPreserveAttachments	= nullptr;

		VkRenderPassCreateInfo createInfo = initVulkanStructure();
		// No explicit dependencies
		createInfo.attachmentCount	= u32(attachments.size());
		createInfo.pAttachments		= attachments.data();
		createInfo.subpassCount		= 1;
		createInfo.pSubpasses		= &subpass;

		m_renderPass = createRenderPass(*m_deviceInterface, *m_device, &createInfo);
	}
	{
		VkFramebufferCreateInfo createInfo = initVulkanStructure();
		createInfo.renderPass		= *m_renderPass;
		createInfo.attachmentCount	= u32(framebufferAttachments.size());
		createInfo.pAttachments		= framebufferAttachments.data();
		createInfo.width			= m_renderArea.extent.width;
		createInfo.height			= m_renderArea.extent.height;
		createInfo.layers			= 1;

		m_framebuffer = createFramebuffer(*m_deviceInterface, *m_device, &createInfo);
	}

	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;

	Move<VkShaderModule> vertModule;
	Move<VkShaderModule> tessControlModule;
	Move<VkShaderModule> tessEvalModule;
	Move<VkShaderModule> geomModule;
	Move<VkShaderModule> fragModule;

	vertModule = createShaderModule(*m_deviceInterface, *m_device, getShaderBinary("vert"), 0u);
	fragModule = createShaderModule(*m_deviceInterface, *m_device, getShaderBinary("frag"), 0u);

	shaderStages.emplace_back(makeShaderStageCreateInfo(VK_SHADER_STAGE_VERTEX_BIT, *vertModule));
	shaderStages.emplace_back(makeShaderStageCreateInfo(VK_SHADER_STAGE_FRAGMENT_BIT, *fragModule));

	if (m_params.isTessellation())
	{
		tessControlModule = createShaderModule(*m_deviceInterface, *m_device, getShaderBinary("tess_cont"), 0u);
		tessEvalModule	  = createShaderModule(*m_deviceInterface, *m_device, getShaderBinary("tess_eval"), 0u);

		shaderStages.emplace_back(makeShaderStageCreateInfo(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, *tessControlModule));
		shaderStages.emplace_back(makeShaderStageCreateInfo(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, *tessEvalModule));
	}
	else if (m_params.isGeometry())
	{
		geomModule = createShaderModule(*m_deviceInterface, *m_device, getShaderBinary("geom"), 0u);

		shaderStages.emplace_back(makeShaderStageCreateInfo(VK_SHADER_STAGE_GEOMETRY_BIT, *geomModule));
	}

	VkPipelineVertexInputStateCreateInfo vertexInputState = initVulkanStructure();
	// No vertex input

	VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = initVulkanStructure();
	inputAssemblyState.topology = !!tessControlModule ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineTessellationStateCreateInfo tesselationState = initVulkanStructure();
	tesselationState.patchControlPoints = 3;

	VkViewport viewport = makeViewport(m_renderArea.extent);

	VkPipelineViewportStateCreateInfo viewportState = initVulkanStructure();
	viewportState.viewportCount = 1;
	viewportState.pViewports	= &viewport;
	viewportState.scissorCount	= 1;
	viewportState.pScissors		= &m_renderArea;

	VkPipelineRasterizationStateCreateInfo rasterizationState = initVulkanStructure();
    rasterizationState.depthClampEnable			= VK_FALSE;
    rasterizationState.rasterizerDiscardEnable  = VK_FALSE;
    rasterizationState.polygonMode				= VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode					= VK_CULL_MODE_NONE;
    rasterizationState.frontFace				= VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationState.depthBiasEnable			= VK_FALSE;
    rasterizationState.depthBiasConstantFactor	= 0.0f;
    rasterizationState.depthBiasClamp			= 0.0f;
    rasterizationState.depthBiasSlopeFactor		= 0.0f;
    rasterizationState.lineWidth				= 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampleState = initVulkanStructure();
	// Everything else disabled/default
	multisampleState.rasterizationSamples	= VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencilState = initVulkanStructure();
	// Everything else disabled/default
    depthStencilState.minDepthBounds = 0.0f;
    depthStencilState.maxDepthBounds = 1.0f;

	VkPipelineColorBlendAttachmentState colorAttachment {};
	// Everything else disabled/default
	colorAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo colorBlendState = initVulkanStructure();
	// Everything else disabled/default
    colorBlendState.attachmentCount	= 1;
    colorBlendState.pAttachments	= &colorAttachment;

	{
		VkGraphicsPipelineCreateInfo createInfo = initVulkanStructure();
		createInfo.stageCount			= u32(shaderStages.size());
		createInfo.pStages				= shaderStages.data();
		createInfo.pVertexInputState	= &vertexInputState;
		createInfo.pInputAssemblyState	= &inputAssemblyState;
		createInfo.pTessellationState	= &tesselationState;
		createInfo.pViewportState		= &viewportState;
		createInfo.pRasterizationState	= &rasterizationState;
		createInfo.pMultisampleState	= &multisampleState;
		createInfo.pDepthStencilState	= &depthStencilState;
		createInfo.pColorBlendState		= &colorBlendState;
		createInfo.pDynamicState		= nullptr;
		createInfo.layout				= *m_pipelineLayout;
		createInfo.renderPass			= *m_renderPass;
		createInfo.subpass				= 0;
		createInfo.basePipelineHandle	= DE_NULL;
		createInfo.basePipelineIndex	= -1;

		m_pipeline = vk::createGraphicsPipeline(
			*m_deviceInterface,
			*m_device,
			DE_NULL, // pipeline cache
			&createInfo);
	}
}

void DescriptorBufferTestInstance::initializeBinding(
	const DescriptorSetLayoutHolder&	dsl,
	deUint32							setIndex,
	Binding&							binding)
{
	const auto arrayCount = (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) ?
		1 : binding.binding.descriptorCount;

	const bool mustSplitCombinedImageSampler =
		(arrayCount > 1) &&
		(binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
		(m_descriptorBufferProperties.splitCombinedImageSamplers == VK_TRUE);

	for (deUint32 arrayIndex = 0; arrayIndex < arrayCount; ++arrayIndex)
	{
		VkDescriptorGetInfoEXT		descGetInfo = initVulkanStructure();
		VkDescriptorAddressInfoEXT	addressInfo = initVulkanStructure();
		VkDescriptorImageInfo		imageInfo {0, 0, VK_IMAGE_LAYOUT_UNDEFINED};	// must be explicitly initialized due to CTS handles inside

		if ((binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) ||
			(binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER))
		{
			DE_ASSERT(binding.perBindingResourceIndex[arrayIndex] == INDEX_INVALID);
			binding.perBindingResourceIndex[arrayIndex] = addResource();
			auto& bufferResource = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).buffer;

			const VkBufferUsageFlags usage =
				(binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT :
				(binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0;
			DE_ASSERT(usage);

			bufferResource.size = sizeof(deUint32) * (binding.isResultBuffer ? ConstResultBufferDwords : ConstUniformBufferDwords);
			auto createInfo = makeBufferCreateInfo(bufferResource.size, usage);

			bufferResource.buffer = createBuffer(*m_deviceInterface, *m_device, &createInfo);

			auto memReqs = getBufferMemoryRequirements(*m_deviceInterface, *m_device, *bufferResource.buffer);

			bufferResource.alloc = m_allocator->allocate(memReqs, MemoryRequirement::HostVisible);
			VK_CHECK(m_deviceInterface->bindBufferMemory(
				*m_device,
				*bufferResource.buffer,
				bufferResource.alloc->getMemory(),
				bufferResource.alloc->getOffset()));

			bufferResource.loadDeviceAddress(*m_deviceInterface, *m_device);

			deUint32* pBufferData = static_cast<deUint32*>(bufferResource.alloc->getHostPtr());

			if (binding.isResultBuffer)
			{
				// The second binding is the verification buffer, so zero it.
				deMemset(pBufferData, 0, static_cast<std::size_t>(bufferResource.size));
			}
			else
			{
				const auto data = getExpectedData(m_params.hash, setIndex, binding.binding.binding, arrayIndex);

				for (deUint32 i = 0; i < ConstUniformBufferDwords; ++i)
				{
					pBufferData[i] = data + i;
				}
			}

			addressInfo.address = bufferResource.deviceAddress;
			addressInfo.range   = bufferResource.size;
			addressInfo.format  = VK_FORMAT_UNDEFINED;

			descGetInfo.type = binding.binding.descriptorType;
			descGetInfo.data.pUniformBuffer = &addressInfo;	// and pStorageBuffer
		}
		else if (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
		{
			// Inline uniforms don't use a backing buffer.
			DE_ASSERT(binding.perBindingResourceIndex[arrayIndex] == INDEX_INVALID);
		}
		else if ((binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) ||
				 (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER))
		{
			DE_ASSERT(binding.perBindingResourceIndex[arrayIndex] == INDEX_INVALID);
			binding.perBindingResourceIndex[arrayIndex] = addResource();
			auto& bufferResource = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).buffer;

			const VkBufferUsageFlags usage =
				(binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) ? VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT :
				(binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : 0;
			DE_ASSERT(usage);

			bufferResource.size = sizeof(deUint32);
			auto createInfo = makeBufferCreateInfo(bufferResource.size, usage);

			bufferResource.buffer = createBuffer(*m_deviceInterface, *m_device, &createInfo);

			auto memReqs = getBufferMemoryRequirements(*m_deviceInterface, *m_device, *bufferResource.buffer);

			bufferResource.alloc = m_allocator->allocate(memReqs, MemoryRequirement::HostVisible);
			VK_CHECK(m_deviceInterface->bindBufferMemory(
				*m_device,
				*bufferResource.buffer,
				bufferResource.alloc->getMemory(),
				bufferResource.alloc->getOffset()));

			bufferResource.loadDeviceAddress(*m_deviceInterface, *m_device);

			if (m_params.isPushDescriptorTest())
			{
				// Push descriptors use buffer views.
				auto& bufferViewResource = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).bufferView;

				bufferViewResource = makeBufferView(
					*m_deviceInterface,
					*m_device,
					*bufferResource.buffer,
					VK_FORMAT_R32_UINT,
					0,
					bufferResource.size);
			}

			deUint32* pBufferData = static_cast<deUint32*>(bufferResource.alloc->getHostPtr());
			*pBufferData = getExpectedData(m_params.hash, setIndex, binding.binding.binding, arrayIndex);

			addressInfo.address = bufferResource.deviceAddress;
			addressInfo.range	= bufferResource.size;
			addressInfo.format	= VK_FORMAT_R32_UINT;

			descGetInfo.type = binding.binding.descriptorType;
			descGetInfo.data.pUniformTexelBuffer = &addressInfo; // and pStorageTexelBuffer
		}
		else if ((binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ||
				 (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) ||
				 (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) ||
				 (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
		{
			// Check if we had already added the resource while handling samplers.
			if (binding.perBindingResourceIndex[arrayIndex] == INDEX_INVALID)
			{
				binding.perBindingResourceIndex[arrayIndex] = addResource();
			}
			auto& imageResource = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).image;
			auto& stagingBuffer = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).buffer;

			{
				VkImageLayout	  layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				VkImageUsageFlags usage  = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

				if (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
				{
					usage |= VK_IMAGE_USAGE_STORAGE_BIT;
					layout = VK_IMAGE_LAYOUT_GENERAL;
				}
				else if (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
				{
					usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
				}
				else
				{
					usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
				}

				// We ensure the extent matches the render area, for the sake of input attachment case.
				imageResource.info = initVulkanStructure();
				imageResource.info.flags					= 0;
				imageResource.info.imageType				= VK_IMAGE_TYPE_2D;
				imageResource.info.format					= VK_FORMAT_R32_UINT;
				imageResource.info.extent.width				= m_renderArea.extent.width;
				imageResource.info.extent.height			= m_renderArea.extent.height;
				imageResource.info.extent.depth				= 1;
				imageResource.info.mipLevels				= 1;
				imageResource.info.arrayLayers				= 1;
				imageResource.info.samples					= VK_SAMPLE_COUNT_1_BIT;
				imageResource.info.tiling					= VK_IMAGE_TILING_OPTIMAL;
				imageResource.info.usage					= usage;
				imageResource.info.sharingMode				= VK_SHARING_MODE_EXCLUSIVE;
				imageResource.info.queueFamilyIndexCount	= 0;
				imageResource.info.pQueueFamilyIndices		= nullptr;
				imageResource.info.initialLayout			= VK_IMAGE_LAYOUT_UNDEFINED;

				imageResource.image = createImage(*m_deviceInterface, *m_device, &imageResource.info);

				auto memReqs = getImageMemoryRequirements(*m_deviceInterface, *m_device, *imageResource.image);
				imageResource.sizeBytes	= memReqs.size;
				imageResource.alloc		= m_allocator->allocate(memReqs, MemoryRequirement::Local);

				VK_CHECK(m_deviceInterface->bindImageMemory(
					*m_device,
					*imageResource.image,
					imageResource.alloc->getMemory(),
					imageResource.alloc->getOffset()));

				VkImageViewCreateInfo createInfo = initVulkanStructure();
				createInfo.image			= *imageResource.image;
				createInfo.viewType			= VK_IMAGE_VIEW_TYPE_2D;
				createInfo.format			= imageResource.info.format;
				createInfo.components		= makeComponentMappingRGBA();
				createInfo.subresourceRange = makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);

				imageResource.layout	= layout;
				imageResource.imageView = createImageView(*m_deviceInterface, *m_device, &createInfo);

				imageInfo.imageLayout = layout;
				imageInfo.imageView   = *imageResource.imageView;

				descGetInfo.type = binding.binding.descriptorType;
				descGetInfo.data.pStorageImage = &imageInfo;
			}
			{
				const auto numPixels = m_renderArea.extent.width * m_renderArea.extent.height;
				stagingBuffer.size = sizeof(deUint32) * numPixels;
				auto createInfo = makeBufferCreateInfo(stagingBuffer.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

				stagingBuffer.buffer = createBuffer(*m_deviceInterface, *m_device, &createInfo);

				auto memReqs = getBufferMemoryRequirements(*m_deviceInterface, *m_device, *stagingBuffer.buffer);

				stagingBuffer.alloc = m_allocator->allocate(memReqs, MemoryRequirement::HostVisible);
				VK_CHECK(m_deviceInterface->bindBufferMemory(
					*m_device,
					*stagingBuffer.buffer,
					stagingBuffer.alloc->getMemory(),
					stagingBuffer.alloc->getOffset()));

				// Fill the whole image uniformly
				deUint32* pBufferData = static_cast<deUint32*>(stagingBuffer.alloc->getHostPtr());
				deUint32 expectedData;

				if (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
				{
					expectedData = getExpectedData(m_params.hash, setIndex, binding.binding.binding, binding.inputAttachmentIndex + arrayIndex);
				}
				else
				{
					expectedData = getExpectedData(m_params.hash, setIndex, binding.binding.binding, arrayIndex);
				}

				std::fill(pBufferData, pBufferData + numPixels, expectedData);
			}

			if (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
			{
				if (m_params.variant != TestVariant::EMBEDDED_IMMUTABLE_SAMPLERS)
				{
					DE_ASSERT(binding.perBindingResourceIndex[arrayIndex] != INDEX_INVALID);
					auto& resourceSampler = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).sampler;

					imageInfo.sampler = *resourceSampler;
				}
			}
		}
		else if (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER)
		{
			if (m_params.variant != TestVariant::EMBEDDED_IMMUTABLE_SAMPLERS)
			{
				DE_ASSERT(binding.perBindingResourceIndex[arrayIndex] != INDEX_INVALID);
				auto& resourceSampler = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).sampler;

				descGetInfo.type = binding.binding.descriptorType;
				descGetInfo.data.pSampler = &*resourceSampler;
			}
		}
		else
		{
			TCU_THROW(InternalError, "Not implemented");
		}

		if (dsl.usePushDescriptors)
		{
			// Push descriptors don't rely on descriptor buffers, move to the next binding.
			continue;
		}

		// Check if we have anything to write (pSampler is aliasing all other pointers).
		if ((descGetInfo.data.pSampler != nullptr) ||
			(binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK))
		{
			void* bindingHostPtr = nullptr;
			Allocation* pAlloc = nullptr;
			const auto arrayOffset = arrayIndex * getDescriptorSize(binding.binding.descriptorType);

			if (dsl.stagingBufferOffset == OFFSET_UNUSED)
			{
				const auto& descriptorBuffer = *m_descriptorBuffers[dsl.bufferIndex];
				const auto bufferHostPtr = offsetPtr(descriptorBuffer.alloc->getHostPtr(), dsl.bufferOffset);

				bindingHostPtr = offsetPtr(bufferHostPtr, binding.offset);
				pAlloc = descriptorBuffer.alloc.get();
			}
			else
			{
				bindingHostPtr = offsetPtr(
					m_descriptorStagingBuffer.alloc->getHostPtr(),
					dsl.stagingBufferOffset + binding.offset);

				pAlloc = m_descriptorStagingBuffer.alloc.get();
			}

			if (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
			{
				DE_ASSERT(arrayIndex == 0);

				// Inline uniform data is written in descriptor buffer directly.
				const auto numDwords = binding.binding.descriptorCount / sizeof(deUint32);
				const auto data		 = getExpectedData(m_params.hash, setIndex, binding.binding.binding, arrayIndex);

				deUint32* pInlineData = static_cast<deUint32*>(bindingHostPtr);

				for (deUint32 i = 0; i < numDwords; ++i)
				{
					pInlineData[i] = data + i;
				}
			}
			else
			{
				VK_CHECK(m_deviceInterface->getDescriptorEXT(*m_device, &descGetInfo, offsetPtr(bindingHostPtr, arrayOffset)));
			}

			// After writing the last array element, rearrange the split combined image sampler data.
			if (mustSplitCombinedImageSampler && ((arrayIndex + 1) == arrayCount))
			{
				// We determined the size of the descriptor set layout on the VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER type,
				// so it's expected the following holds true.
				DE_ASSERT((m_descriptorBufferProperties.sampledImageDescriptorSize + m_descriptorBufferProperties.samplerDescriptorSize) ==
					m_descriptorBufferProperties.combinedImageSamplerDescriptorSize);

				std::vector<deUint8> scratchSpace(
					arrayCount * m_descriptorBufferProperties.combinedImageSamplerDescriptorSize);

				const auto descriptorArraySize = static_cast<std::size_t>(arrayCount * m_descriptorBufferProperties.combinedImageSamplerDescriptorSize);

				deMemcpy(scratchSpace.data(), bindingHostPtr, descriptorArraySize);
				deMemset(bindingHostPtr, 0, descriptorArraySize);

				const void*	combinedReadPtr = scratchSpace.data();
				void*		imageWritePtr   = bindingHostPtr;
				void*		samplerWritePtr = offsetPtr(bindingHostPtr, arrayCount * m_descriptorBufferProperties.sampledImageDescriptorSize);

				for (deUint32 i = 0; i < arrayCount; ++i)
				{
					deMemcpy(imageWritePtr,		offsetPtr(combinedReadPtr, 0),														 m_descriptorBufferProperties.sampledImageDescriptorSize);
					deMemcpy(samplerWritePtr,	offsetPtr(combinedReadPtr, m_descriptorBufferProperties.sampledImageDescriptorSize), m_descriptorBufferProperties.samplerDescriptorSize);

					combinedReadPtr	= offsetPtr(combinedReadPtr, m_descriptorBufferProperties.combinedImageSamplerDescriptorSize);
					imageWritePtr	= offsetPtr(imageWritePtr,   m_descriptorBufferProperties.sampledImageDescriptorSize);
					samplerWritePtr	= offsetPtr(samplerWritePtr, m_descriptorBufferProperties.samplerDescriptorSize);
				}
			}

			flushAlloc(*m_deviceInterface, *m_device, *pAlloc);
		}
	}
}

void DescriptorBufferTestInstance::pushDescriptorSet(
		VkCommandBuffer						cmdBuf,
		VkPipelineBindPoint					bindPoint,
		const DescriptorSetLayoutHolder&	dsl,
		deUint32							setIndex) const
{
	std::vector<PushDescriptorData>		descriptorData(dsl.bindings.size());	// Allocate empty elements upfront
	std::vector<VkWriteDescriptorSet>	descriptorWrites;

	descriptorWrites.reserve(dsl.bindings.size());

	// Fill in the descriptor data structure. It can be used by the regular and templated update path.

	for (deUint32 bindingIndex = 0; bindingIndex < u32(dsl.bindings.size()); ++bindingIndex)
	{
		const auto& binding = dsl.bindings[bindingIndex];

		VkWriteDescriptorSet write = initVulkanStructure();
		write.dstSet			= DE_NULL;	// ignored with push descriptors
		write.dstBinding		= bindingIndex;
		write.dstArrayElement	= 0;
		write.descriptorCount	= binding.binding.descriptorCount;
		write.descriptorType	= binding.binding.descriptorType;

		for (deUint32 arrayIndex = 0; arrayIndex < write.descriptorCount; ++arrayIndex)
		{
			DE_ASSERT(binding.perBindingResourceIndex[arrayIndex] != INDEX_INVALID);

			if ((binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) ||
				(binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER))
			{
				const auto& bufferResource = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).buffer;

				auto pInfo = &descriptorData[bindingIndex].bufferInfos[arrayIndex];
				pInfo->buffer = *bufferResource.buffer;
				pInfo->offset = 0;
				pInfo->range  = bufferResource.size;

				if (arrayIndex == 0)
				{
					write.pBufferInfo = pInfo;
				}
			}
			else if ((binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) ||
					 (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER))
			{
				const auto& bufferViewResource = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).bufferView;

				auto pBufferView = &descriptorData[bindingIndex].texelBufferViews[arrayIndex];
				*pBufferView = *bufferViewResource;

				if (arrayIndex == 0)
				{
					write.pTexelBufferView = pBufferView;
				}
			}
			else if ((binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ||
					 (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) ||
					 (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) ||
					 (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
					 (binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER))
			{
				const auto& imageResource   = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).image;
				const auto& samplerResource = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).sampler;

				// Dereferencing unused resources will return null handles, so we can treat all these descriptors uniformly.

				auto pInfo = &descriptorData[bindingIndex].imageInfos[arrayIndex];
				pInfo->imageView   = *imageResource.imageView;
				pInfo->imageLayout = imageResource.layout;
				pInfo->sampler	   = *samplerResource;

				if (arrayIndex == 0)
				{
					write.pImageInfo = pInfo;
				}
			}
			else
			{
				TCU_THROW(InternalError, "Not implemented");
			}
		}

		if (m_params.variant == TestVariant::PUSH_DESCRIPTOR)
		{
			descriptorWrites.emplace_back(write);
		}
	}

	if (m_params.variant == TestVariant::PUSH_DESCRIPTOR)
	{
		m_deviceInterface->cmdPushDescriptorSetKHR(
			cmdBuf,
			bindPoint,
			*m_pipelineLayout,
			setIndex,
			u32(descriptorWrites.size()),
			descriptorWrites.data());
	}
	else if (m_params.variant == TestVariant::PUSH_TEMPLATE)
	{
		std::vector<VkDescriptorUpdateTemplateEntry> updateEntries(descriptorData.size());	// preallocate

		const auto dataBasePtr = reinterpret_cast<deUint8*>(descriptorData.data());

		for (deUint32 bindingIndex = 0; bindingIndex < u32(dsl.bindings.size()); ++bindingIndex)
		{
			const auto& binding = dsl.bindings[bindingIndex].binding;
			const auto& data    = descriptorData[bindingIndex];

			auto& entry = updateEntries[bindingIndex];
			entry.dstBinding		= binding.binding;
			entry.dstArrayElement	= 0;
			entry.descriptorCount	= binding.descriptorCount;
			entry.descriptorType	= binding.descriptorType;

			switch(binding.descriptorType)
			{
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
				entry.offset = basePtrOffsetOf(dataBasePtr, data.bufferInfos);
				entry.stride = sizeof(data.bufferInfos[0]);
				break;

			case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
			case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
				entry.offset = basePtrOffsetOf(dataBasePtr, data.texelBufferViews);
				entry.stride = sizeof(data.texelBufferViews[0]);
				break;

			case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
			case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
			case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			case VK_DESCRIPTOR_TYPE_SAMPLER:
				entry.offset = basePtrOffsetOf(dataBasePtr, data.imageInfos);
				entry.stride = sizeof(data.imageInfos[0]);
				break;

			default:
				DE_ASSERT(0);
				break;
			}
		}

		VkDescriptorUpdateTemplateCreateInfo createInfo = initVulkanStructure();
		createInfo.templateType					= VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
		createInfo.descriptorSetLayout			= *dsl.layout;
		createInfo.pipelineBindPoint			= bindPoint;
		createInfo.pipelineLayout				= *m_pipelineLayout;
		createInfo.set							= setIndex;
		createInfo.descriptorUpdateEntryCount	= u32(updateEntries.size());
		createInfo.pDescriptorUpdateEntries		= updateEntries.data();

		auto descriptorUpdateTemplate = createDescriptorUpdateTemplate(
			*m_deviceInterface,
			*m_device,
			&createInfo);

		m_deviceInterface->cmdPushDescriptorSetWithTemplateKHR(
			cmdBuf,
			*descriptorUpdateTemplate,
			*m_pipelineLayout,
			setIndex,
			dataBasePtr);
	}
}

// Perform the test accoring to the parameters. At high level, all tests perform these steps:
//
// - Create a new device and queues, query extension properties.
// - Fill descriptor set layouts and bindings, based on SimpleBinding's.
// - Create samplers, if needed. Set immutable samplers in bindings.
// - Create descriptor set layouts.
// - Create descriptor buffers.
// - Iterate over all bindings to:
//   - Create their resources (images, buffers) and initialize them
//   - Write bindings to descriptor buffer memory
//   - Fix combined image samplers for arrayed bindings (if applicable)
// - Create the pipeline layout, shaders, and the pipeline
// - Create the command buffer and record the commands (barriers omitted for brevity):
//   - Bind the pipeline and the descriptor buffers
//   - Upload descriptor buffer data (with staged uploads)
//   - Upload image data (if images are used)
//   - Push descriptors (if used)
//   - Dispatch or draw
//   - Submit the commands
//   - Map the result buffer to a host pointer
//   - Verify the result and log diagnostic on a failure
//
// Verification logic is very simple.
//
// Each successful binding read will increment the result counter. If the shader got an unexpected value, the counter
// will be less than expected. Additionally, the first failed set/binding/array index will be recorded.
//
tcu::TestStatus	DescriptorBufferTestInstance::iterate()
{
	DE_ASSERT(m_params.bufferBindingCount <= m_descriptorBufferProperties.maxDescriptorBufferBindings);

	const auto& vk = *m_deviceInterface;

	{
		deUint32 currentSet = INDEX_INVALID;

		for (const auto& sb : m_simpleBindings)
		{
			if ((currentSet == INDEX_INVALID) || (currentSet < sb.set))
			{
				currentSet = sb.set;

				addDescriptorSetLayout();
			}

			auto& dsl = **m_descriptorSetLayouts.back();

			deUint32 descriptorCount;

			if (sb.type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
			{
				descriptorCount = sizeof(deUint32) * ConstInlineBlockDwords;
			}
			else
			{
				descriptorCount = sb.count;
			}

			dsl.bindings.emplace_back(
				makeDescriptorSetLayoutBinding(
					sb.binding,
					sb.type,
					descriptorCount,			// descriptor array size / inline uniform block size
					m_params.stage,				// where accessible
					nullptr));					// immutable sampler, may have to be patched later

			auto& binding = dsl.bindings.back();

			binding.inputAttachmentIndex = sb.inputAttachmentIndex;
			binding.isResultBuffer		 = sb.isResultBuffer;

			// We create samplers before creating the descriptor set layouts, in case we need to use
			// immutable (or embedded) samplers.

			if ((sb.type == VK_DESCRIPTOR_TYPE_SAMPLER) ||
				(sb.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
			{
				for (deUint32 arrayIndex = 0; arrayIndex < sb.count; ++arrayIndex)
				{
					DE_ASSERT(binding.perBindingResourceIndex[arrayIndex] == INDEX_INVALID);
					binding.perBindingResourceIndex[arrayIndex] = addResource();

					auto& resourceSampler = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).sampler;

					// Use CLAMP_TO_BORDER to verify that sampling outside the image will make use of the sampler's
					// properties. The border color used must match the one in glslOutputVerification().

					VkSamplerCreateInfo createInfo = initVulkanStructure();
					createInfo.magFilter				= VK_FILTER_NEAREST;
					createInfo.minFilter				= VK_FILTER_NEAREST;
					createInfo.mipmapMode				= VK_SAMPLER_MIPMAP_MODE_NEAREST;
					createInfo.addressModeU				= VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
					createInfo.addressModeV				= VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
					createInfo.addressModeW				= VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
					createInfo.mipLodBias				= 0.0f;
					createInfo.anisotropyEnable			= VK_FALSE;
					createInfo.maxAnisotropy			= 1.0f;
					createInfo.compareEnable			= VK_FALSE;
					createInfo.compareOp				= VK_COMPARE_OP_NEVER;
					createInfo.minLod					= 0.0;
					createInfo.maxLod					= 0.0;
					createInfo.borderColor				= VK_BORDER_COLOR_INT_OPAQUE_BLACK;
					createInfo.unnormalizedCoordinates	= VK_FALSE;

					resourceSampler = createSampler(vk, *m_device, &createInfo);

					if (sb.isEmbeddedImmutableSampler)
					{
						dsl.hasEmbeddedImmutableSamplers = true;
					}
				}
			}
		}

		if ((m_params.variant == TestVariant::EMBEDDED_IMMUTABLE_SAMPLERS) ||
			(m_params.subcase == SubCase::IMMUTABLE_SAMPLERS))
		{
			// Patch immutable sampler pointers, now that all memory has been allocated and pointers won't move.

			for (deUint32 setIndex = 0; setIndex < u32(m_descriptorSetLayouts.size()); ++setIndex)
			{
				auto& dsl = **m_descriptorSetLayouts[setIndex];

				for (deUint32 bindingIndex = 0; bindingIndex < u32(dsl.bindings.size()); ++bindingIndex)
				{
					auto& binding = dsl.bindings[bindingIndex];

					for (deUint32 resourceIndex = 0; resourceIndex < DE_LENGTH_OF_ARRAY(binding.perBindingResourceIndex); ++resourceIndex)
					{
						if (binding.perBindingResourceIndex[resourceIndex] != INDEX_INVALID)
						{
							const auto& resources = **m_resources[binding.perBindingResourceIndex[resourceIndex]];

							if (resources.sampler)
							{
								DE_ASSERT(resourceIndex < DE_LENGTH_OF_ARRAY(binding.immutableSamplers));

								binding.immutableSamplers[resourceIndex] = *resources.sampler;
							}
						}
					}

					binding.binding.pImmutableSamplers = binding.immutableSamplers;
				}
			}
		}
		else if (m_params.isPushDescriptorTest())
		{
			DE_ASSERT(m_params.pushDescriptorSetIndex < m_descriptorSetLayouts.size());

			(**m_descriptorSetLayouts[m_params.pushDescriptorSetIndex]).usePushDescriptors = true;
		}

		createDescriptorSetLayouts();
		createDescriptorBuffers();
	}

	for (deUint32 setIndex = 0; setIndex < u32(m_descriptorSetLayouts.size()); ++setIndex)
	{
		auto& dsl = **m_descriptorSetLayouts[setIndex];

		if (dsl.hasEmbeddedImmutableSamplers)
		{
			// Embedded samplers are not written to the descriptor buffer directly.
			continue;
		}

		for (deUint32 bindingIndex = 0; bindingIndex < u32(dsl.bindings.size()); ++bindingIndex)
		{
			initializeBinding(dsl, setIndex, dsl.bindings[bindingIndex]);
		}
	}

	{
		VkPipelineLayoutCreateInfo createInfo = initVulkanStructure();
		const auto dslCopy = getDescriptorSetLayouts(m_descriptorSetLayouts);
		createInfo.setLayoutCount = u32(dslCopy.size());
		createInfo.pSetLayouts = dslCopy.data();

		m_pipelineLayout = createPipelineLayout(vk, *m_device, &createInfo);
	}

	if (m_params.isCompute())
	{
		const auto shaderModule	= createShaderModule(vk, *m_device, m_context.getBinaryCollection().get("comp"), 0u);
		m_pipeline = makeComputePipeline(vk, *m_device, *m_pipelineLayout, *shaderModule);
	}
	else
	{
		createGraphicsPipeline();
	}

	{
		auto cmdPool = makeCommandPool(vk, *m_device, m_queueFamilyIndex);
		auto cmdBuf	 = allocateCommandBuffer(vk, *m_device, *cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);

		const auto bindPoint = m_params.isCompute() ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;

		beginCommandBuffer(vk, *cmdBuf);

		vk.cmdBindPipeline(*cmdBuf, bindPoint, *m_pipeline);

		bindDescriptorBuffers(*cmdBuf, bindPoint);

		// Check if we need any staged descriptor set uploads or push descriptors.

		for (deUint32 setIndex = 0; setIndex < m_descriptorSetLayouts.size(); ++setIndex)
		{
			const auto& dsl = **m_descriptorSetLayouts[setIndex];

			if (dsl.usePushDescriptors)
			{
				pushDescriptorSet(*cmdBuf, bindPoint, dsl, setIndex);
			}
			else if (dsl.stagingBufferOffset != OFFSET_UNUSED)
			{
				VkBufferCopy copy {};
				copy.srcOffset = dsl.stagingBufferOffset;
				copy.dstOffset = dsl.bufferOffset;
				copy.size      = dsl.size;

				VkBuffer descriptorBuffer = *m_descriptorBuffers[dsl.bufferIndex]->buffer;

				vk.cmdCopyBuffer(
					*cmdBuf,
					*m_descriptorStagingBuffer.buffer,
					descriptorBuffer,
					1,	// copy regions
					&copy);

				VkBufferMemoryBarrier2 barrier = initVulkanStructure();
				barrier.srcStageMask		= VK_PIPELINE_STAGE_2_COPY_BIT;
				barrier.srcAccessMask		= VK_ACCESS_2_TRANSFER_WRITE_BIT;
				barrier.dstStageMask		= m_params.isCompute() ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
				barrier.dstAccessMask		= VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.buffer				= descriptorBuffer;
				barrier.offset				= 0;
				barrier.size				= VK_WHOLE_SIZE;

				VkDependencyInfo depInfo = initVulkanStructure();
				depInfo.bufferMemoryBarrierCount	= 1;
				depInfo.pBufferMemoryBarriers		= &barrier;

				vk.cmdPipelineBarrier2(*cmdBuf, &depInfo);
			}
		}

		// Upload image data

		for (deUint32 setIndex = 0; setIndex < u32(m_descriptorSetLayouts.size()); ++setIndex)
		{
			const auto& dsl = **m_descriptorSetLayouts[setIndex];

			for (deUint32 bindingIndex = 0; bindingIndex < u32(dsl.bindings.size()); ++bindingIndex)
			{
				const auto& binding = dsl.bindings[bindingIndex];

				if ((binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ||
					(binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) ||
					(binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) ||
					(binding.binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
				{
					for (deUint32 arrayIndex = 0; arrayIndex < binding.binding.descriptorCount; ++arrayIndex)
					{
						// Need to upload the image data from a staging buffer
						const auto& dstImage  = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).image;
						const auto& srcBuffer = (**m_resources[binding.perBindingResourceIndex[arrayIndex]]).buffer;

						{
							VkImageMemoryBarrier2 barrier = initVulkanStructure();
							barrier.srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
							barrier.srcAccessMask		= VK_ACCESS_2_NONE;
							barrier.dstStageMask		= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
							barrier.dstAccessMask		= VK_ACCESS_2_TRANSFER_WRITE_BIT;
							barrier.oldLayout			= VK_IMAGE_LAYOUT_UNDEFINED;
							barrier.newLayout			= VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
							barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							barrier.image				= *dstImage.image;
							barrier.subresourceRange	= makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);

							VkDependencyInfo depInfo = initVulkanStructure();
							depInfo.imageMemoryBarrierCount	    = 1;
							depInfo.pImageMemoryBarriers		= &barrier;

							vk.cmdPipelineBarrier2(*cmdBuf, &depInfo);
						}
						{
							VkBufferImageCopy region {};
							// Use default buffer settings
							region.imageSubresource		= makeImageSubresourceLayers(VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1);
							region.imageOffset			= makeOffset3D(0, 0, 0);
							region.imageExtent			= makeExtent3D(m_renderArea.extent.width, m_renderArea.extent.height, 1);

							vk.cmdCopyBufferToImage(
								*cmdBuf,
								*srcBuffer.buffer,
								*dstImage.image,
								VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
								1,	// region count
								&region);
						}
						{
							VkImageMemoryBarrier2 barrier = initVulkanStructure();
							barrier.srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
							barrier.srcAccessMask		= VK_ACCESS_2_TRANSFER_WRITE_BIT;
							barrier.dstStageMask		= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;	// beginning of the shader pipeline
							barrier.dstAccessMask		= VK_ACCESS_2_SHADER_READ_BIT;
							barrier.oldLayout			= VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
							barrier.newLayout			= dstImage.layout;
							barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							barrier.image				= *dstImage.image;
							barrier.subresourceRange	= makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);

							VkDependencyInfo depInfo = initVulkanStructure();
							depInfo.imageMemoryBarrierCount	    = 1;
							depInfo.pImageMemoryBarriers		= &barrier;

							vk.cmdPipelineBarrier2(*cmdBuf, &depInfo);
						}
					}
				}
			}
		}

		if (m_params.isCompute())
		{
			vk.cmdDispatch(*cmdBuf, 1, 1, 1);

			{
				auto& resultBuffer = getComputeResultBuffer();

				VkBufferMemoryBarrier2 barrier = initVulkanStructure();
				barrier.srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
				barrier.srcAccessMask		= VK_ACCESS_2_SHADER_WRITE_BIT;
				barrier.dstStageMask		= VK_PIPELINE_STAGE_2_HOST_BIT;
				barrier.dstAccessMask		= VK_ACCESS_2_HOST_READ_BIT;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.buffer				= *resultBuffer.buffer;
				barrier.offset				= 0;
				barrier.size				= VK_WHOLE_SIZE;

				VkDependencyInfo depInfo = initVulkanStructure();
				depInfo.bufferMemoryBarrierCount	= 1;
				depInfo.pBufferMemoryBarriers		= &barrier;

				vk.cmdPipelineBarrier2(*cmdBuf, &depInfo);
			}
		}
		else
		{
			beginRenderPass(vk, *cmdBuf, *m_renderPass, *m_framebuffer, m_renderArea, tcu::Vec4());

			vk.cmdDraw(*cmdBuf, 6, 1, 0, 0);

			endRenderPass(vk, *cmdBuf);

			// Copy the rendered image to a host-visible buffer.

			{
				VkImageMemoryBarrier2 barrier = initVulkanStructure();
				barrier.srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
				barrier.srcAccessMask		= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
				barrier.dstStageMask		= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
				barrier.dstAccessMask		= VK_ACCESS_2_TRANSFER_READ_BIT;
				barrier.oldLayout			= VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				barrier.newLayout			= VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.image				= *m_colorImage.image;
				barrier.subresourceRange	= makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);

				VkDependencyInfo depInfo = initVulkanStructure();
				depInfo.imageMemoryBarrierCount	    = 1;
				depInfo.pImageMemoryBarriers		= &barrier;

				vk.cmdPipelineBarrier2(*cmdBuf, &depInfo);
			}
			{
				VkBufferImageCopy region {};
				// Use default buffer settings
				region.imageSubresource		= makeImageSubresourceLayers(VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1);
				region.imageOffset			= makeOffset3D(0, 0, 0);
				region.imageExtent			= m_colorImage.info.extent;

				vk.cmdCopyImageToBuffer(
					*cmdBuf,
					*m_colorImage.image,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					*m_colorBuffer.buffer,
					1,	// region count
					&region);
			}
			{
				VkBufferMemoryBarrier2 barrier = initVulkanStructure();
				barrier.srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
				barrier.srcAccessMask		= VK_ACCESS_2_TRANSFER_WRITE_BIT;
				barrier.dstStageMask		= VK_PIPELINE_STAGE_2_HOST_BIT;
				barrier.dstAccessMask		= VK_ACCESS_2_HOST_READ_BIT;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.buffer				= *m_colorBuffer.buffer;
				barrier.offset				= 0;
				barrier.size				= VK_WHOLE_SIZE;

				VkDependencyInfo depInfo = initVulkanStructure();
				depInfo.bufferMemoryBarrierCount	= 1;
				depInfo.pBufferMemoryBarriers		= &barrier;

				vk.cmdPipelineBarrier2(*cmdBuf, &depInfo);
			}
		}

		endCommandBuffer(vk, *cmdBuf);
		submitCommandsAndWait(vk, *m_device, m_queue, *cmdBuf);
	}

	// Verification
	{
		const tcu::UVec4* pResultData = nullptr;

		if (m_params.isCompute())
		{
			auto& resultBuffer = getComputeResultBuffer();

			invalidateAlloc(vk, *m_device, *resultBuffer.alloc);

			pResultData = static_cast<const tcu::UVec4*>(resultBuffer.alloc->getHostPtr());
		}
		else
		{
			pResultData = static_cast<const tcu::UVec4*>(m_colorBuffer.alloc->getHostPtr());
		}

		const auto	actual	 = pResultData->x();
		deUint32	expected = 0;

		for (const auto& sb : m_simpleBindings)
		{
			if (!sb.isResultBuffer)
			{
				if (m_params.variant == TestVariant::MAX)
				{
					// We test enough (image, sampler) pairs to access each one at least once.
					expected = deMaxu32(m_params.samplerBufferBindingCount, m_params.resourceBufferBindingCount);
				}
				else
				{
					if (sb.type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
					{
						expected += ConstInlineBlockDwords;
					}
					else if ((sb.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) ||
							 (sb.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER))
					{
						expected += ConstUniformBufferDwords * sb.count;
					}
					// Samplers are tested implicitly via sampled images
					else if (sb.type != VK_DESCRIPTOR_TYPE_SAMPLER)
					{
						expected += sb.count;
					}
				}
			}
		}

		if (actual != expected)
		{
			deUint32 badSet			= 0;
			deUint32 badBinding		= 0;
			deUint32 badArrayIndex	= 0;

			unpackBindingArgs(pResultData->y(), &badSet, &badBinding, &badArrayIndex);

			std::ostringstream msg;
			msg << "Wrong value in result buffer. Expected (" << expected << ") but got (" << actual << ").";
			msg << " The first wrong binding is (set = " << badSet << ", binding = " << badBinding << ")";

			if (m_params.variant == TestVariant::MAX)
			{
				deUint32 badSamplerSet		= 0;
				deUint32 badSamplerBinding	= 0;

				unpackBindingArgs(pResultData->z(), &badSamplerSet, &badSamplerBinding, nullptr);

				msg << " which used a sampler (set = " << badSamplerSet << ", binding = " << badSamplerBinding << ")";
			}
			else if (badArrayIndex > 0)
			{
				msg << " at array index " << badArrayIndex;
			}

			msg << ".";

			return tcu::TestStatus::fail(msg.str());
		}
	}

	return tcu::TestStatus::pass("Pass");
}

TestInstance* DescriptorBufferTestCase::createInstance (Context& context) const
{
	// Currently most tests follow the same basic execution logic.
	if ((m_params.variant == TestVariant::SINGLE) ||
		(m_params.variant == TestVariant::MULTIPLE) ||
		(m_params.variant == TestVariant::MAX) ||
		(m_params.variant == TestVariant::EMBEDDED_IMMUTABLE_SAMPLERS) ||
		(m_params.variant == TestVariant::PUSH_DESCRIPTOR) ||
		(m_params.variant == TestVariant::PUSH_TEMPLATE))
	{
		return new DescriptorBufferTestInstance(context, m_params, m_simpleBindings);
	}
	else
	{
		TCU_THROW(InternalError, "Not implemented");
	}
}

// This simple tests verifies extension properties against the spec limits.
//
tcu::TestStatus testLimits(Context& context)
{
#define CHECK_MIN_LIMIT(_struct_, _field_, _limit_) \
	if (_struct_._field_ < _limit_) { TCU_THROW(TestError, #_field_ " is less than " #_limit_); }

// Max implicitly checks nonzero too
#define CHECK_MAX_LIMIT(_struct_, _field_, _limit_) \
	if (_struct_._field_ == 0)      { TCU_THROW(TestError, #_field_ " is 0"); } \
	if (_struct_._field_ > _limit_) { TCU_THROW(TestError, #_field_ " is greater than " #_limit_); }


	if (context.isDeviceFunctionalitySupported("VK_EXT_descriptor_buffer"))
	{
		const auto& features = *findStructure<VkPhysicalDeviceDescriptorBufferFeaturesEXT>(&context.getDeviceFeatures2());
		const auto& props	 = *findStructure<VkPhysicalDeviceDescriptorBufferPropertiesEXT>(&context.getDeviceProperties2());
		const bool  hasRT    = context.isDeviceFunctionalitySupported("VK_KHR_ray_tracing_pipeline") ||
							   context.isDeviceFunctionalitySupported("VK_KHR_ray_query") ;

		DE_ASSERT(features.descriptorBuffer == VK_TRUE);

		if (context.getDeviceFeatures2().features.robustBufferAccess)
		{
			CHECK_MAX_LIMIT(props, robustUniformTexelBufferDescriptorSize,	64);
			CHECK_MAX_LIMIT(props, robustStorageTexelBufferDescriptorSize,	128);
			CHECK_MAX_LIMIT(props, robustUniformBufferDescriptorSize,		64);
			CHECK_MAX_LIMIT(props, robustStorageBufferDescriptorSize,		128);
		}

		if (features.descriptorBufferCaptureReplay)
		{
			CHECK_MAX_LIMIT(props, bufferCaptureReplayDescriptorDataSize,		64);
			CHECK_MAX_LIMIT(props, imageCaptureReplayDescriptorDataSize,		64);
			CHECK_MAX_LIMIT(props, imageViewCaptureReplayDescriptorDataSize,	64);
			CHECK_MAX_LIMIT(props, samplerCaptureReplayDescriptorDataSize,		64);

			if (hasRT)
			{
				CHECK_MAX_LIMIT(props, accelerationStructureCaptureReplayDescriptorDataSize,	64);
			}
		}

		if (hasRT)
		{
			CHECK_MAX_LIMIT(props, accelerationStructureDescriptorSize,	64);
		}

		CHECK_MAX_LIMIT(props, descriptorBufferOffsetAlignment,	256);

		CHECK_MIN_LIMIT(props, maxDescriptorBufferBindings,				3);
		CHECK_MIN_LIMIT(props, maxResourceDescriptorBufferBindings,		1);
		CHECK_MIN_LIMIT(props, maxSamplerDescriptorBufferBindings,		1);
		CHECK_MIN_LIMIT(props, maxEmbeddedImmutableSamplerBindings,		1);
		CHECK_MIN_LIMIT(props, maxEmbeddedImmutableSamplers,			2032);

		CHECK_MAX_LIMIT(props, samplerDescriptorSize,				64);
		CHECK_MAX_LIMIT(props, combinedImageSamplerDescriptorSize,	128);
		CHECK_MAX_LIMIT(props, sampledImageDescriptorSize,			64);
		CHECK_MAX_LIMIT(props, storageImageDescriptorSize,			64);
		CHECK_MAX_LIMIT(props, uniformTexelBufferDescriptorSize,	64);
		CHECK_MAX_LIMIT(props, storageTexelBufferDescriptorSize,	128);
		CHECK_MAX_LIMIT(props, uniformBufferDescriptorSize,			64);
		CHECK_MAX_LIMIT(props, storageBufferDescriptorSize,			128);
		CHECK_MAX_LIMIT(props, inputAttachmentDescriptorSize,		64);

		CHECK_MIN_LIMIT(props, maxSamplerDescriptorBufferRange,				(1u << 27));
		CHECK_MIN_LIMIT(props, maxResourceDescriptorBufferRange,			(1u << 27));
		CHECK_MIN_LIMIT(props, resourceDescriptorBufferAddressSpaceSize,	(1u << 27));
		CHECK_MIN_LIMIT(props, samplerDescriptorBufferAddressSpaceSize,		(1u << 27));
		CHECK_MIN_LIMIT(props, descriptorBufferAddressSpaceSize,			(1u << 27));

		// The following requirement ensures that for split combined image sampler arrays:
		// - there's no unnecessary padding at the end, or
		// - there's no risk of overrun (if somehow the sum of image and sampler was greater).

		if ((props.splitCombinedImageSamplers == VK_TRUE) &&
			((props.sampledImageDescriptorSize + props.samplerDescriptorSize) != props.combinedImageSamplerDescriptorSize))
		{
			return tcu::TestStatus::fail("For splitCombinedImageSamplers, it is expected that the sampled image size "
				"and the sampler size add up to combinedImageSamplerDescriptorSize.");
		}
	}
	else
	{
		TCU_THROW(NotSupportedError, "VK_EXT_descriptor_buffer is not supported");
	}

	return tcu::TestStatus::pass("Pass");

#undef CHECK_MIN_LIMIT
#undef CHECK_MAX_LIMIT
}

void populateDescriptorBufferTests (tcu::TestCaseGroup* topGroup)
{
	tcu::TestContext& testCtx = topGroup->getTestContext();

	const VkQueueFlagBits choiceQueues[] {
		VK_QUEUE_GRAPHICS_BIT,
		VK_QUEUE_COMPUTE_BIT,
	};

	const VkShaderStageFlagBits choiceStages[] {
		VK_SHADER_STAGE_VERTEX_BIT,
		VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
		VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
		VK_SHADER_STAGE_GEOMETRY_BIT,
		VK_SHADER_STAGE_FRAGMENT_BIT,
		VK_SHADER_STAGE_COMPUTE_BIT,
		// TODO ray tracing
		// TODO mesh shading
	};

	{
		MovePtr<tcu::TestCaseGroup> subGroup(new tcu::TestCaseGroup(testCtx, "basic", "Basic tests"));

		addFunctionCase(subGroup.get(), "limits", "Check basic device properties and limits", testLimits);

		topGroup->addChild(subGroup.release());
	}

	{
		//
		// Basic single descriptor cases -- a sanity check.
		//
		MovePtr<tcu::TestCaseGroup> subGroup(new tcu::TestCaseGroup(testCtx, "single", "Single binding tests"));

		// VK_DESCRIPTOR_TYPE_SAMPLER is tested implicitly by sampled image case.
		// *_BUFFER_DYNAMIC are not allowed with descriptor buffers.
		//
		const VkDescriptorType choiceDescriptors[] {
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
			VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,
		};

		for (auto pQueue = choiceQueues; pQueue < DE_ARRAY_END(choiceQueues); ++pQueue)
		for (auto pStage = choiceStages; pStage < DE_ARRAY_END(choiceStages); ++pStage)
		for (auto pDescriptor = choiceDescriptors; pDescriptor < DE_ARRAY_END(choiceDescriptors); ++pDescriptor)
		{
			if ((*pQueue == VK_QUEUE_COMPUTE_BIT) && (*pStage != VK_SHADER_STAGE_COMPUTE_BIT))
			{
				// Compute queue can only use compute shaders.
				continue;
			}

			if ((*pDescriptor == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) && (*pStage != VK_SHADER_STAGE_FRAGMENT_BIT))
			{
				// Subpass loads are only valid in fragment stage.
				continue;
			}

			TestParams params {};
			params.variant				= TestVariant::SINGLE;
			params.subcase				= SubCase::NONE;
			params.stage				= *pStage;
			params.queue				= *pQueue;
			params.descriptor			= *pDescriptor;
			params.bufferBindingCount	= 1;
			params.setsPerBuffer		= 1;

			params.updateHash();

			subGroup->addChild(new DescriptorBufferTestCase(testCtx, getCaseName(params), "", params));
		}

		topGroup->addChild(subGroup.release());
	}

	{
		//
		// More complex cases. Multiple sets and bindings per buffer. Immutable samplers.
		//
		MovePtr<tcu::TestCaseGroup> subGroup(new tcu::TestCaseGroup(testCtx, "multiple", "Multiple bindings tests"));

		const struct {
			deUint32 bufferBindingCount;
			deUint32 setsPerBuffer;
			bool	 addIncrementalBindSubcase;

		} caseOptions[] = {
			{  1,  3, false },
			{  2,  4, true  },
			{  3,  1, true  },		// 3 buffer bindings is spec minimum
			{  8,  1, false },
			{ 16,  1, false },
			{ 32,  1, false },
		};

		for (auto pQueue = choiceQueues; pQueue < DE_ARRAY_END(choiceQueues); ++pQueue)
		for (auto pStage = choiceStages; pStage < DE_ARRAY_END(choiceStages); ++pStage)
		for (auto pOptions = caseOptions; pOptions < DE_ARRAY_END(caseOptions); ++pOptions)
		{
			if ((*pQueue == VK_QUEUE_COMPUTE_BIT) && (*pStage != VK_SHADER_STAGE_COMPUTE_BIT))
			{
				// Compute queue can only use compute shaders.
				continue;
			}

			TestParams params {};
			params.variant				= TestVariant::MULTIPLE;
			params.subcase				= SubCase::NONE;
			params.stage				= *pStage;
			params.queue				= *pQueue;
			params.bufferBindingCount	= pOptions->bufferBindingCount;
			params.setsPerBuffer		= pOptions->setsPerBuffer;

			params.updateHash();

			subGroup->addChild(new DescriptorBufferTestCase(testCtx, getCaseName(params), "", params));

			if (pOptions->bufferBindingCount < 4)
			{
				// For the smaller binding counts add a subcase with immutable samplers.

				params.subcase = SubCase::IMMUTABLE_SAMPLERS;

				params.updateHash();

				subGroup->addChild(new DescriptorBufferTestCase(testCtx, getCaseName(params), "", params));
			}

			if (pOptions->addIncrementalBindSubcase)
			{
				// Add a case that binds descriptor buffers (and offsets) over more than a one API call.
				DE_ASSERT(params.bufferBindingCount > 1);

				params.subcase = SubCase::INCREMENTAL_BIND;

				params.updateHash();

				subGroup->addChild(new DescriptorBufferTestCase(testCtx, getCaseName(params), "", params));
			}
		}

		topGroup->addChild(subGroup.release());
	}

	{
		//
		// These cases exercise buffers of single usage (samplers only and resources only) and tries to use
		// all available buffer bindings.
		//
		MovePtr<tcu::TestCaseGroup> subGroup(new tcu::TestCaseGroup(testCtx, "max", "Max sampler/resource bindings tests"));

		const struct {
			deUint32 samplerBufferBindingCount;
			deUint32 resourceBufferBindingCount;

		} caseOptions[] = {
			{  1,   1 },
			{  2,   2 },
			{  4,   4 },
			{  8,   8 },
			{ 16,  16 },
			{  1,   7 },
			{  1,  15 },
			{  1,  31 },
			{  7,   1 },
			{ 15,   1 },
			{ 31,   1 },
		};

		for (auto pQueue = choiceQueues; pQueue < DE_ARRAY_END(choiceQueues); ++pQueue)
		for (auto pStage = choiceStages; pStage < DE_ARRAY_END(choiceStages); ++pStage)
		for (auto pOptions = caseOptions; pOptions < DE_ARRAY_END(caseOptions); ++pOptions)
		{
			if ((*pQueue == VK_QUEUE_COMPUTE_BIT) && (*pStage != VK_SHADER_STAGE_COMPUTE_BIT))
			{
				// Compute queue can only use compute shaders.
				continue;
			}

			TestParams params {};
			params.variant					  = TestVariant::MAX;
			params.subcase					  = SubCase::NONE;
			params.stage					  = *pStage;
			params.queue					  = *pQueue;
			params.samplerBufferBindingCount  = pOptions->samplerBufferBindingCount;
			params.resourceBufferBindingCount = pOptions->resourceBufferBindingCount;
			params.bufferBindingCount		  = pOptions->samplerBufferBindingCount + pOptions->resourceBufferBindingCount;
			params.setsPerBuffer			  = 1;

			params.updateHash();

			subGroup->addChild(new DescriptorBufferTestCase(testCtx, getCaseName(params), "", params));
		}

		topGroup->addChild(subGroup.release());
	}

	{
		//
		// Check embedded immutable sampler buffers/bindings.
		//
		MovePtr<tcu::TestCaseGroup> subGroup(new tcu::TestCaseGroup(testCtx, "embedded_imm_samplers", "Max embedded immutable samplers tests"));

		const struct {
			deUint32 bufferBindingCount;
			deUint32 samplersPerBuffer;

		} caseOptions[] = {
			{  1,  1 },
			{  1,  2 },
			{  1,  4 },
			{  1,  8 },
			{  1, 16 },
			{  2,  1 },
			{  2,  2 },
			{  3,  1 },
			{  3,  3 },
			{  8,  1 },
			{  8,  4 },
		};

		for (auto pQueue = choiceQueues; pQueue < DE_ARRAY_END(choiceQueues); ++pQueue)
		for (auto pStage = choiceStages; pStage < DE_ARRAY_END(choiceStages); ++pStage)
		for (auto pOptions = caseOptions; pOptions < DE_ARRAY_END(caseOptions); ++pOptions)
		{
			if ((*pQueue == VK_QUEUE_COMPUTE_BIT) && (*pStage != VK_SHADER_STAGE_COMPUTE_BIT))
			{
				// Compute queue can only use compute shaders.
				continue;
			}

			TestParams params {};
			params.variant										= TestVariant::EMBEDDED_IMMUTABLE_SAMPLERS;
			params.subcase										= SubCase::NONE;
			params.stage										= *pStage;
			params.queue										= *pQueue;
			params.bufferBindingCount							= pOptions->bufferBindingCount + 1;
			params.setsPerBuffer								= 1;
			params.embeddedImmutableSamplerBufferBindingCount	= pOptions->bufferBindingCount;
			params.embeddedImmutableSamplersPerBuffer			= pOptions->samplersPerBuffer;

			params.updateHash();

			subGroup->addChild(new DescriptorBufferTestCase(testCtx, getCaseName(params), "", params));
		}

		topGroup->addChild(subGroup.release());
	}

	{
		//
		// Check push descriptors and push descriptors with template updates
		//
		MovePtr<tcu::TestCaseGroup> subGroupPush		(new tcu::TestCaseGroup(testCtx, "push_descriptor", "Use push descriptors in addition to descriptor buffer"));
		MovePtr<tcu::TestCaseGroup> subGroupPushTemplate(new tcu::TestCaseGroup(testCtx, "push_template", "Use descriptor update template with push descriptors in addition to descriptor buffer"));

		const struct {
			deUint32 pushDescriptorSetIndex;
			deUint32 bufferBindingCount;

			// The total number of descriptor sets will be bufferBindingCount + 1, where the additional set is used for push descriptors.

		} caseOptions[] = {
			{  0,  1 },
			{  0,  3 },
			{  1,  1 },
			{  0,  2 },
			{  1,  2 },
			{  2,  2 },		// index = 2 means 3 sets, where the first two are used with descriptor buffer and the last with push descriptors
			{  3,  3 },
		};

		for (auto pQueue = choiceQueues; pQueue < DE_ARRAY_END(choiceQueues); ++pQueue)
		for (auto pStage = choiceStages; pStage < DE_ARRAY_END(choiceStages); ++pStage)
		for (auto pOptions = caseOptions; pOptions < DE_ARRAY_END(caseOptions); ++pOptions)
		{
			if ((*pQueue == VK_QUEUE_COMPUTE_BIT) && (*pStage != VK_SHADER_STAGE_COMPUTE_BIT))
			{
				// Compute queue can only use compute shaders.
				continue;
			}

			TestParams params {};
			params.variant					= TestVariant::PUSH_DESCRIPTOR;
			params.subcase					= SubCase::NONE;
			params.stage					= *pStage;
			params.queue					= *pQueue;
			params.bufferBindingCount		= pOptions->bufferBindingCount;
			params.setsPerBuffer			= 1;
			params.pushDescriptorSetIndex	= pOptions->pushDescriptorSetIndex;

			params.updateHash();

			subGroupPush->addChild(new DescriptorBufferTestCase(testCtx, getCaseName(params), "", params));

			params.variant					= TestVariant::PUSH_TEMPLATE;

			params.updateHash();

			subGroupPushTemplate->addChild(new DescriptorBufferTestCase(testCtx, getCaseName(params), "", params));
		}

		topGroup->addChild(subGroupPush.release());
		topGroup->addChild(subGroupPushTemplate.release());
	}
}

} // anonymous

tcu::TestCaseGroup* createDescriptorBufferTests (tcu::TestContext& testCtx)
{
	return createTestGroup(testCtx, "descriptor_buffer", "Descriptor buffer tests.", populateDescriptorBufferTests);
}

} // BindingModel
} // vkt
