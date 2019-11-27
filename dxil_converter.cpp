/*
 * Copyright 2019 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "dxil_converter.hpp"
#include "SpvBuilder.h"
#include "cfg_structurizer.hpp"
#include "logging.hpp"
#include "node.hpp"
#include "node_pool.hpp"

#include <utility>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>

namespace DXIL2SPIRV
{
struct Converter::Impl
{
	Impl(DXILContainerParser container_parser_, LLVMBCParser bitcode_parser_, SPIRVModule &module_)
	    : container_parser(std::move(container_parser_))
	    , bitcode_parser(std::move(bitcode_parser_))
	    , spirv_module(module_)
	{
	}

	DXILContainerParser container_parser;
	LLVMBCParser bitcode_parser;
	SPIRVModule &spirv_module;

	struct BlockMeta
	{
		explicit BlockMeta(llvm::BasicBlock *bb_)
		    : bb(bb_)
		{
		}

		llvm::BasicBlock *bb;
		CFGNode *node = nullptr;
	};
	std::vector<std::unique_ptr<BlockMeta>> metas;
	std::unordered_map<llvm::BasicBlock *, BlockMeta *> bb_map;
	std::unordered_map<const llvm::Value *, spv::Id> value_map;

	ConvertedFunction convert_entry_point();
	spv::Id get_id_for_value(const llvm::Value &value, unsigned forced_integer_width = 0);
	spv::Id get_id_for_constant(const llvm::Constant &constant, unsigned forced_width);
	spv::Id get_id_for_undef(const llvm::UndefValue &undef);

	void emit_stage_input_variables();
	void emit_stage_output_variables();

	void emit_resources();
	void emit_srvs(const llvm::MDNode *srvs);
	void emit_uavs(const llvm::MDNode *uavs);
	void emit_cbvs(const llvm::MDNode *cbvs);
	void emit_samplers(const llvm::MDNode *samplers);

	std::vector<spv::Id> srv_index_to_id;
	std::vector<spv::Id> uav_index_to_id;
	std::vector<spv::Id> cbv_index_to_id;
	std::vector<spv::Id> sampler_index_to_id;
	std::unordered_map<const llvm::Value *, spv::Id> handle_to_ptr_id;
	std::unordered_map<spv::Id, spv::Id> id_to_type;

	spv::Id get_type_id(unsigned element_type, unsigned rows, unsigned cols);
	spv::Id get_type_id(const llvm::Type &type);
	spv::Id get_type_id(spv::Id id) const;

	std::unordered_map<uint32_t, spv::Id> input_elements_ids;
	std::unordered_map<uint32_t, spv::Id> output_elements_ids;
	void emit_builtin_decoration(spv::Id id, DXIL::Semantic semantic);

	void emit_instruction(CFGNode *block, const llvm::Instruction &instruction);

	// Native LLVM instructions.
	void emit_phi_instruction(CFGNode *block, const llvm::PHINode &instruction);
	void emit_binary_instruction(CFGNode *block, const llvm::BinaryOperator &instruction);
	void emit_unary_instruction(CFGNode *block, const llvm::UnaryOperator &instruction);
	void emit_cast_instruction(CFGNode *block, const llvm::CastInst &instruction);
	void emit_getelementptr_instruction(CFGNode *block, const llvm::GetElementPtrInst &instruction);
	void emit_load_instruction(CFGNode *block, const llvm::LoadInst &instruction);
	void emit_store_instruction(CFGNode *block, const llvm::StoreInst &instruction);
	void emit_compare_instruction(CFGNode *block, const llvm::CmpInst &instruction);
	void emit_extract_value_instruction(CFGNode *block, const llvm::ExtractValueInst &instruction);
	void emit_alloca_instruction(CFGNode *block, const llvm::AllocaInst &instruction);
	void emit_select_instruction(CFGNode *block, const llvm::SelectInst &instruction);

	// DXIL intrinsics.
	void emit_builtin_instruction(CFGNode *block, const llvm::CallInst &instruction);

	void emit_load_input_instruction(CFGNode *block, const llvm::CallInst &instruction);
	void emit_store_output_instruction(CFGNode *block, const llvm::CallInst &instruction);
	void emit_create_handle_instruction(CFGNode *block, const llvm::CallInst &instruction);
	void emit_cbuffer_load_legacy_instruction(CFGNode *block, const llvm::CallInst &instruction);
	void emit_sample_instruction(DXIL::Op op, CFGNode *block, const llvm::CallInst &instruction);

	static uint32_t get_constant_operand(const llvm::CallInst &value, unsigned index);
	spv::Id build_sampled_image(CFGNode *block, spv::Id image_id, spv::Id sampler_id, bool comparison);
	spv::Id build_vector(CFGNode *block, spv::Id element_type, spv::Id *elements, unsigned count);
};

Converter::Converter(DXILContainerParser container_parser_, LLVMBCParser bitcode_parser_, SPIRVModule &module_)
{
	impl = std::make_unique<Impl>(std::move(container_parser_), std::move(bitcode_parser_), module_);
}

Converter::~Converter()
{
}

ConvertedFunction Converter::convert_entry_point()
{
	return impl->convert_entry_point();
}

template <typename T = uint32_t>
static T get_constant_metadata(const llvm::MDNode *node, unsigned index)
{
	return T(
	    llvm::cast<llvm::ConstantAsMetadata>(node->getOperand(index))->getValue()->getUniqueInteger().getSExtValue());
}

static std::string get_string_metadata(const llvm::MDNode *node, unsigned index)
{
	return llvm::cast<llvm::MDString>(node->getOperand(index))->getString();
}

static spv::Dim image_dimension_from_resource_kind(DXIL::ResourceKind kind)
{
	switch (kind)
	{
	case DXIL::ResourceKind::Texture1D:
	case DXIL::ResourceKind::Texture1DArray:
		return spv::Dim1D;
	case DXIL::ResourceKind::Texture2D:
	case DXIL::ResourceKind::Texture2DMS:
	case DXIL::ResourceKind::Texture2DArray:
	case DXIL::ResourceKind::Texture2DMSArray:
		return spv::Dim2D;
	case DXIL::ResourceKind::Texture3D:
		return spv::Dim3D;
	case DXIL::ResourceKind::TextureCube:
	case DXIL::ResourceKind::TextureCubeArray:
		return spv::DimCube;

	case DXIL::ResourceKind::TypedBuffer:
	case DXIL::ResourceKind::StructuredBuffer:
	case DXIL::ResourceKind::RawBuffer:
		return spv::DimBuffer;

	default:
		return spv::DimMax;
	}
}

static bool image_dimension_is_arrayed(DXIL::ResourceKind kind)
{
	switch (kind)
	{
	case DXIL::ResourceKind::Texture1DArray:
	case DXIL::ResourceKind::Texture2DArray:
	case DXIL::ResourceKind::Texture2DMSArray:
	case DXIL::ResourceKind::TextureCubeArray:
		return true;

	default:
		return false;
	}
}

static bool image_dimension_is_multisampled(DXIL::ResourceKind kind)
{
	switch (kind)
	{
	case DXIL::ResourceKind::Texture2DMS:
	case DXIL::ResourceKind::Texture2DMSArray:
		return true;

	default:
		return false;
	}
}

void Converter::Impl::emit_srvs(const llvm::MDNode *srvs)
{
	auto &builder = spirv_module.get_builder();
	unsigned num_srvs = srvs->getNumOperands();

	for (unsigned i = 0; i < num_srvs; i++)
	{
		auto *srv = llvm::cast<llvm::MDNode>(srvs->getOperand(i));
		unsigned index = get_constant_metadata(srv, 0);
		auto name = get_string_metadata(srv, 2);
		unsigned bind_space = get_constant_metadata(srv, 3);
		unsigned bind_register = get_constant_metadata(srv, 4);
		// range_size = 5

		auto resource_kind = static_cast<DXIL::ResourceKind>(get_constant_metadata(srv, 6));

		auto *tags = srv->getNumOperands() >= 9 ? llvm::dyn_cast<llvm::MDNode>(srv->getOperand(8)) : nullptr;
		assert(tags);

		spv::Id sampled_type_id = 0;
		if (get_constant_metadata(tags, 0) == 0)
		{
			// Sampled format.
			sampled_type_id = get_type_id(get_constant_metadata(tags, 1), 1, 1);
		}
		else
		{
			// Structured/Raw buffers, just use uint for good measure, we'll bitcast as needed.
			// Field 1 is stride, but we don't care about that unless we will support an SSBO path.
			sampled_type_id = builder.makeUintType(32);
		}

		spv::Id type_id =
		    builder.makeImageType(sampled_type_id, image_dimension_from_resource_kind(resource_kind), false,
		                          image_dimension_is_arrayed(resource_kind),
		                          image_dimension_is_multisampled(resource_kind), 1, spv::ImageFormatUnknown);

		spv::Id var_id =
		    builder.createVariable(spv::StorageClassUniformConstant, type_id, name.empty() ? nullptr : name.c_str());

		builder.addDecoration(var_id, spv::DecorationDescriptorSet, bind_space);
		builder.addDecoration(var_id, spv::DecorationBinding, bind_register);

		srv_index_to_id.resize(std::max(srv_index_to_id.size(), size_t(index + 1)));
		srv_index_to_id[index] = var_id;
	}
}

void Converter::Impl::emit_uavs(const llvm::MDNode *uavs)
{
}

void Converter::Impl::emit_cbvs(const llvm::MDNode *cbvs)
{
	auto &builder = spirv_module.get_builder();

	unsigned num_cbvs = cbvs->getNumOperands();
	for (unsigned i = 0; i < num_cbvs; i++)
	{
		auto *cbv = llvm::cast<llvm::MDNode>(cbvs->getOperand(i));
		unsigned index = get_constant_metadata(cbv, 0);
		auto name = get_string_metadata(cbv, 2);
		unsigned bind_space = get_constant_metadata(cbv, 3);
		unsigned bind_register = get_constant_metadata(cbv, 4);
		// range_size = 5
		unsigned cbv_size = get_constant_metadata(cbv, 6);

		unsigned vec4_length = (cbv_size + 15) / 16;

		// It seems like we will have to bitcast ourselves away from vec4 here after loading.
		spv::Id member_array_type = builder.makeArrayType(builder.makeVectorType(builder.makeFloatType(32), 4),
		                                                  builder.makeUintConstant(vec4_length, false), 16);

		builder.addDecoration(member_array_type, spv::DecorationArrayStride, 16);

		spv::Id type_id = builder.makeStructType({ member_array_type }, name.c_str());
		builder.addMemberDecoration(type_id, 0, spv::DecorationOffset, 0);
		builder.addDecoration(type_id, spv::DecorationBlock);
		spv::Id var_id =
		    builder.createVariable(spv::StorageClassUniform, type_id, name.empty() ? nullptr : name.c_str());

		builder.addDecoration(var_id, spv::DecorationDescriptorSet, bind_space);
		builder.addDecoration(var_id, spv::DecorationBinding, bind_register);

		cbv_index_to_id.resize(std::max(cbv_index_to_id.size(), size_t(index + 1)));
		cbv_index_to_id[index] = var_id;
	}
}

void Converter::Impl::emit_samplers(const llvm::MDNode *samplers)
{
	auto &builder = spirv_module.get_builder();
	unsigned num_samplers = samplers->getNumOperands();

	for (unsigned i = 0; i < num_samplers; i++)
	{
		auto *sampler = llvm::cast<llvm::MDNode>(samplers->getOperand(i));
		unsigned index = get_constant_metadata(sampler, 0);
		auto name = get_string_metadata(sampler, 2);
		unsigned bind_space = get_constant_metadata(sampler, 3);
		unsigned bind_register = get_constant_metadata(sampler, 4);
		// range_size = 5

		spv::Id type_id = builder.makeSamplerType();
		spv::Id var_id =
		    builder.createVariable(spv::StorageClassUniformConstant, type_id, name.empty() ? nullptr : name.c_str());

		builder.addDecoration(var_id, spv::DecorationDescriptorSet, bind_space);
		builder.addDecoration(var_id, spv::DecorationBinding, bind_register);

		sampler_index_to_id.resize(std::max(sampler_index_to_id.size(), size_t(index + 1)));
		sampler_index_to_id[index] = var_id;
	}
}

void Converter::Impl::emit_resources()
{
	auto &module = bitcode_parser.get_module();
	auto *resource_meta = module.getNamedMetadata("dx.resources");
	if (!resource_meta)
		return;

	auto *metas = resource_meta->getOperand(0);

	if (metas->getOperand(0))
		emit_srvs(llvm::dyn_cast<llvm::MDNode>(metas->getOperand(0)));
	if (metas->getOperand(1))
		emit_uavs(llvm::dyn_cast<llvm::MDNode>(metas->getOperand(1)));
	if (metas->getOperand(2))
		emit_cbvs(llvm::dyn_cast<llvm::MDNode>(metas->getOperand(2)));
	if (metas->getOperand(3))
		emit_samplers(llvm::dyn_cast<llvm::MDNode>(metas->getOperand(3)));
}

spv::Id Converter::Impl::get_id_for_constant(const llvm::Constant &constant, unsigned forced_width)
{
	auto &builder = spirv_module.get_builder();

	switch (constant.getType()->getTypeID())
	{
	case llvm::Type::TypeID::FloatTyID:
	{
		auto &fp = llvm::cast<llvm::ConstantFP>(constant);
		return builder.makeFloatConstant(fp.getValueAPF().convertToFloat());
	}

	case llvm::Type::TypeID::DoubleTyID:
	{
		auto &fp = llvm::cast<llvm::ConstantFP>(constant);
		return builder.makeDoubleConstant(fp.getValueAPF().convertToDouble());
	}

	case llvm::Type::TypeID::IntegerTyID:
	{
		unsigned integer_width = forced_width ? forced_width : constant.getType()->getIntegerBitWidth();
		switch (integer_width)
		{
		case 32:
			return builder.makeUintConstant(constant.getUniqueInteger().getZExtValue());

		default:
			return 0;
		}
	}

	default:
		return 0;
	}
}

spv::Id Converter::Impl::get_id_for_undef(const llvm::UndefValue &undef)
{
	auto &builder = spirv_module.get_builder();
	return builder.createUndefined(get_type_id(*undef.getType()));
}

spv::Id Converter::Impl::get_id_for_value(const llvm::Value &value, unsigned forced_width)
{
	auto itr = value_map.find(&value);
	if (itr != value_map.end())
		return itr->second;

	spv::Id ret;
	if (auto *undef = llvm::dyn_cast<llvm::UndefValue>(&value))
		ret = get_id_for_undef(*undef);
	else if (auto *constant = llvm::dyn_cast<llvm::Constant>(&value))
		ret = get_id_for_constant(*constant, forced_width);
	else
		ret = spirv_module.allocate_id();

	value_map[&value] = ret;
	return ret;
}

static std::string get_entry_point_name(const llvm::Module &module)
{
	auto *ep_meta = module.getNamedMetadata("dx.entryPoints");
	auto *node = ep_meta->getOperand(0);
	return llvm::cast<llvm::MDString>(node->getOperand(1))->getString();
}

static spv::ExecutionModel get_execution_model(const llvm::Module &module)
{
	auto *shader_model = module.getNamedMetadata("dx.shaderModel");
	auto *shader_model_node = shader_model->getOperand(0);
	auto model = llvm::cast<llvm::MDString>(shader_model_node->getOperand(0))->getString();
	if (model == "vs")
		return spv::ExecutionModelVertex;
	else if (model == "ps")
		return spv::ExecutionModelFragment;
	else if (model == "hs")
		return spv::ExecutionModelTessellationControl;
	else if (model == "ds")
		return spv::ExecutionModelTessellationEvaluation;
	else if (model == "gs")
		return spv::ExecutionModelGeometry;
	else if (model == "cs")
		return spv::ExecutionModelGLCompute;
	else
		return spv::ExecutionModelMax;
}

spv::Id Converter::Impl::get_type_id(const llvm::Type &type)
{
	auto &builder = spirv_module.get_builder();
	switch (type.getTypeID())
	{
	case llvm::Type::TypeID::HalfTyID:
		return builder.makeFloatType(16);
	case llvm::Type::TypeID::FloatTyID:
		return builder.makeFloatType(32);
	case llvm::Type::TypeID::DoubleTyID:
		return builder.makeFloatType(64);

	case llvm::Type::TypeID::IntegerTyID:
		if (type.getIntegerBitWidth() == 1)
			return builder.makeBoolType();
		else
			return builder.makeIntegerType(type.getIntegerBitWidth(), false);

	case llvm::Type::TypeID::PointerTyID:
		return builder.makePointer(spv::StorageClassFunction, get_type_id(*type.getPointerElementType()));

	case llvm::Type::TypeID::ArrayTyID:
		return builder.makeArrayType(get_type_id(*type.getArrayElementType()),
		                             builder.makeUintConstant(type.getArrayNumElements(), false), 0);

	default:
		return 0;
	}
}

spv::Id Converter::Impl::get_type_id(unsigned element_type, unsigned rows, unsigned cols)
{
	auto &builder = spirv_module.get_builder();

	spv::Id component_type;
	switch (static_cast<DXIL::ComponentType>(element_type))
	{
	case DXIL::ComponentType::I1:
		component_type = builder.makeBoolType();
		break;

	case DXIL::ComponentType::I16:
		component_type = builder.makeIntegerType(16, true);
		break;

	case DXIL::ComponentType::U16:
		component_type = builder.makeIntegerType(16, false);
		break;

	case DXIL::ComponentType::I32:
		component_type = builder.makeIntegerType(32, true);
		break;

	case DXIL::ComponentType::U32:
		component_type = builder.makeIntegerType(32, false);
		break;

	case DXIL::ComponentType::I64:
		component_type = builder.makeIntegerType(64, true);
		break;

	case DXIL::ComponentType::U64:
		component_type = builder.makeIntegerType(64, false);
		break;

	case DXIL::ComponentType::F16:
		component_type = builder.makeFloatType(16);
		break;

	case DXIL::ComponentType::F32:
		component_type = builder.makeFloatType(32);
		break;

	case DXIL::ComponentType::F64:
		component_type = builder.makeFloatType(64);
		break;

	default:
		LOGE("Unknown component type.\n");
		return 0;
	}

	if (rows == 1 && cols == 1)
		return component_type;
	else if (rows == 1)
	{
		auto vector_type = builder.makeVectorType(component_type, cols);
		return vector_type;
	}
	else
	{
		auto matrix_type = builder.makeMatrixType(component_type, rows, cols);
		return matrix_type;
	}
}

spv::Id Converter::Impl::get_type_id(spv::Id id) const
{
	auto itr = id_to_type.find(id);
	if (itr == id_to_type.end())
		return 0;
	else
		return itr->second;
}

void Converter::Impl::emit_stage_output_variables()
{
	auto &module = bitcode_parser.get_module();

	auto *ep_meta = module.getNamedMetadata("dx.entryPoints");
	auto *node = ep_meta->getOperand(0);
	auto &signature = node->getOperand(2);
	auto *outputs_node = llvm::cast<llvm::MDNode>(llvm::cast<llvm::MDNode>(signature)->getOperand(1));

	auto &builder = spirv_module.get_builder();

	unsigned location = 0;

	for (unsigned i = 0; i < outputs_node->getNumOperands(); i++)
	{
		auto *output = llvm::cast<llvm::MDNode>(outputs_node->getOperand(i));
		auto element_id = get_constant_metadata(output, 0);
		auto semantic_name = get_string_metadata(output, 1);
		auto element_type = get_constant_metadata(output, 2);
		auto system_value = static_cast<DXIL::Semantic>(get_constant_metadata(output, 3));

		// Semantic index?
		//auto interpolation = get_constant_metadata(input, 5);
		auto rows = get_constant_metadata(output, 6);
		auto cols = get_constant_metadata(output, 7);

#if 0
		auto start_row = get_constant_metadata(input, 8);
		auto col = get_constant_metadata(input, 9);
#endif

#if 0
		LOGE("Semantic output %u: %s\n", element_id, semantic_name.c_str());
		LOGE("  Type: %u\n", element_type);
		LOGE("  System value: %u\n", system_value);
		LOGE("  Interpolation: %u\n", interpolation);
		LOGE("  Rows: %u\n", rows);
		LOGE("  Cols: %u\n", cols);
		LOGE("  Start row: %u\n", start_row);
		LOGE("  Col: %u\n", col);
#endif

		spv::Id type_id = get_type_id(element_type, rows, cols);
		spv::Id variable_id = builder.createVariable(spv::StorageClassOutput, type_id, semantic_name.c_str());
		output_elements_ids[element_id] = variable_id;

		if (system_value == DXIL::Semantic::Target)
		{
			auto semantic_index = get_constant_metadata(output, 8);
			builder.addDecoration(variable_id, spv::DecorationLocation, semantic_index);
		}
		else if (system_value != DXIL::Semantic::User)
		{
			emit_builtin_decoration(variable_id, system_value);
		}
		else
		{
			builder.addDecoration(variable_id, spv::DecorationLocation, location);
			location += rows;
		}

		spirv_module.get_entry_point()->addIdOperand(variable_id);
	}
}

void Converter::Impl::emit_builtin_decoration(spv::Id id, DXIL::Semantic semantic)
{
	auto &builder = spirv_module.get_builder();
	switch (semantic)
	{
	case DXIL::Semantic::Position:
		builder.addDecoration(id, spv::DecorationBuiltIn, spv::BuiltInPosition);
		break;

	default:
		break;
	}
}

void Converter::Impl::emit_stage_input_variables()
{
	auto &module = bitcode_parser.get_module();

	auto *ep_meta = module.getNamedMetadata("dx.entryPoints");
	auto *node = ep_meta->getOperand(0);
	auto &signature = node->getOperand(2);
	auto *inputs_node = llvm::cast<llvm::MDNode>(llvm::cast<llvm::MDNode>(signature)->getOperand(0));

	auto &builder = spirv_module.get_builder();

	unsigned location = 0;

	for (unsigned i = 0; i < inputs_node->getNumOperands(); i++)
	{
		auto *input = llvm::cast<llvm::MDNode>(inputs_node->getOperand(i));
		auto element_id = get_constant_metadata(input, 0);
		auto semantic_name = get_string_metadata(input, 1);
		auto element_type = get_constant_metadata(input, 2);
		auto system_value = static_cast<DXIL::Semantic>(get_constant_metadata(input, 3));

		// Semantic index?
		//auto interpolation = get_constant_metadata(input, 5);
		auto rows = get_constant_metadata(input, 6);
		auto cols = get_constant_metadata(input, 7);

#if 0
		auto start_row = get_constant_metadata(input, 8);
		auto col = get_constant_metadata(input, 9);
#endif

#if 0
		LOGE("Semantic output %u: %s\n", element_id, semantic_name.c_str());
		LOGE("  Type: %u\n", element_type);
		LOGE("  System value: %u\n", system_value);
		LOGE("  Interpolation: %u\n", interpolation);
		LOGE("  Rows: %u\n", rows);
		LOGE("  Cols: %u\n", cols);
		LOGE("  Start row: %u\n", start_row);
		LOGE("  Col: %u\n", col);
#endif

		spv::Id type_id = get_type_id(element_type, rows, cols);
		spv::Id variable_id = builder.createVariable(spv::StorageClassInput, type_id, semantic_name.c_str());
		input_elements_ids[element_id] = variable_id;

		if (system_value != DXIL::Semantic::User)
			emit_builtin_decoration(variable_id, system_value);
		else
		{
			builder.addDecoration(variable_id, spv::DecorationLocation, location);
			location += rows;
		}

		spirv_module.get_entry_point()->addIdOperand(variable_id);
	}
}

uint32_t Converter::Impl::get_constant_operand(const llvm::CallInst &value, unsigned index)
{
	auto *constant = llvm::cast<llvm::Constant>(value.getOperand(index));
	return uint32_t(constant->getUniqueInteger().getZExtValue());
}

void Converter::Impl::emit_load_input_instruction(CFGNode *block, const llvm::CallInst &instruction)
{
	auto &builder = spirv_module.get_builder();
	uint32_t var_id = input_elements_ids[get_constant_operand(instruction, 1)];
	uint32_t ptr_id;
	Operation op;

	uint32_t num_rows = builder.getNumTypeComponents(builder.getDerefTypeId(var_id));

	if (num_rows > 1)
	{
		ptr_id = spirv_module.allocate_id();

		op.op = spv::OpInBoundsAccessChain;
		op.id = ptr_id;
		op.type_id = get_type_id(*instruction.getType());
		op.type_id = builder.makePointer(spv::StorageClassInput, op.type_id);
		op.arguments = { var_id, get_id_for_value(*instruction.getOperand(3), 32) };
		assert(op.arguments[0]);
		assert(op.arguments[1]);

		block->ir.operations.push_back(std::move(op));
	}
	else
		ptr_id = var_id;

	op = {};
	op.op = spv::OpLoad;
	op.id = get_id_for_value(instruction);
	op.type_id = get_type_id(*instruction.getType());
	op.arguments = { ptr_id };
	assert(op.arguments[0]);

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_store_output_instruction(CFGNode *block, const llvm::CallInst &instruction)
{
	auto &builder = spirv_module.get_builder();
	uint32_t var_id = output_elements_ids[get_constant_operand(instruction, 1)];
	uint32_t ptr_id;
	Operation op;

	uint32_t num_rows = builder.getNumTypeComponents(builder.getDerefTypeId(var_id));

	if (num_rows > 1)
	{
		ptr_id = spirv_module.allocate_id();

		op.op = spv::OpInBoundsAccessChain;
		op.id = ptr_id;
		op.type_id = builder.getScalarTypeId(builder.getDerefTypeId(var_id));
		op.type_id = builder.makePointer(spv::StorageClassOutput, op.type_id);
		op.arguments = { var_id, get_id_for_value(*instruction.getOperand(3), 32) };
		assert(op.arguments[0]);
		assert(op.arguments[1]);

		block->ir.operations.push_back(std::move(op));
	}
	else
		ptr_id = var_id;

	op = {};
	op.op = spv::OpStore;
	op.arguments = { ptr_id, get_id_for_value(*instruction.getOperand(4)) };
	assert(op.arguments[0]);
	assert(op.arguments[1]);

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_create_handle_instruction(CFGNode *block, const llvm::CallInst &instruction)
{
	auto &builder = spirv_module.get_builder();
	auto resource_type = static_cast<DXIL::ResourceType>(get_constant_operand(instruction, 1));
	auto resource_range = get_constant_operand(instruction, 2);
	// 3 = index into range
	// 4 = non-uniform resource index
	switch (resource_type)
	{
	case DXIL::ResourceType::SRV:
	{
		spv::Id image_id = srv_index_to_id[resource_range];
		spv::Id type_id = builder.getDerefTypeId(image_id);
		Operation op;
		op.op = spv::OpLoad;
		op.id = spirv_module.allocate_id();
		op.type_id = type_id;
		op.arguments = { image_id };
		id_to_type[op.id] = type_id;
		handle_to_ptr_id[&instruction] = op.id;
		block->ir.operations.push_back(std::move(op));
		break;
	}

	case DXIL::ResourceType::UAV:
		handle_to_ptr_id[&instruction] = uav_index_to_id[resource_range];
		break;

	case DXIL::ResourceType::CBV:
		handle_to_ptr_id[&instruction] = cbv_index_to_id[resource_range];
		break;

	case DXIL::ResourceType::Sampler:
	{
		spv::Id sampler_id = sampler_index_to_id[resource_range];
		spv::Id type_id = builder.getDerefTypeId(sampler_id);
		Operation op;
		op.op = spv::OpLoad;
		op.id = spirv_module.allocate_id();
		op.type_id = type_id;
		op.arguments = { sampler_id };
		handle_to_ptr_id[&instruction] = op.id;
		id_to_type[op.id] = type_id;
		block->ir.operations.push_back(std::move(op));
		break;
	}

	default:
		break;
	}
}

void Converter::Impl::emit_cbuffer_load_legacy_instruction(CFGNode *block, const llvm::CallInst &instruction)
{
	auto &builder = spirv_module.get_builder();
	// This function returns a struct, but ignore that, and just return a vec4 for now.
	// extractvalue is used to pull out components and that works for vectors as well.
	spv::Id ptr_id = handle_to_ptr_id[instruction.getOperand(1)];
	assert(ptr_id);

	spv::Id vec4_index = get_id_for_value(*instruction.getOperand(2));
	spv::Id access_chain_id = spirv_module.allocate_id();

	Operation op;
	op.op = spv::OpInBoundsAccessChain;
	op.id = access_chain_id;
	op.type_id = builder.makeVectorType(builder.makeFloatType(32), 4);
	op.type_id = builder.makePointer(spv::StorageClassUniform, op.type_id);
	op.arguments = { ptr_id, builder.makeUintConstant(0), vec4_index };

	block->ir.operations.push_back(std::move(op));

	bool need_bitcast = false;
	auto *result_type = instruction.getType();
	assert(result_type->getTypeID() == llvm::Type::TypeID::StructTyID);
	assert(result_type->getStructNumElements() == 4);
	if (result_type->getStructElementType(0)->getTypeID() != llvm::Type::TypeID::FloatTyID)
		need_bitcast = true;

	spv::Id bitcast_input_id = 0;
	op = {};
	op.op = spv::OpLoad;
	op.id = need_bitcast ? spirv_module.allocate_id() : get_id_for_value(instruction);
	op.type_id = builder.makeVectorType(builder.makeFloatType(32), 4);
	op.arguments = { access_chain_id };

	bitcast_input_id = op.id;
	block->ir.operations.push_back(std::move(op));

	if (need_bitcast)
	{
		op = {};
		op.op = spv::OpBitcast;
		op.id = get_id_for_value(instruction);

		assert(result_type->getStructElementType(0)->getTypeID() == llvm::Type::TypeID::IntegerTyID);
		op.type_id = builder.makeVectorType(builder.makeUintType(32), 4);
		op.arguments = { bitcast_input_id };
		block->ir.operations.push_back(std::move(op));
	}
}

spv::Id Converter::Impl::build_sampled_image(CFGNode *block, spv::Id image_id, spv::Id sampler_id, bool comparison)
{
	auto &builder = spirv_module.get_builder();
	spv::Id image_type_id = get_type_id(image_id);
	spv::Dim dim = builder.getTypeDimensionality(image_type_id);
	bool arrayed = builder.isArrayedImageType(image_type_id);
	bool multisampled = builder.isMultisampledImageType(image_type_id);
	spv::Id sampled_format = builder.getImageComponentType(image_type_id);

	image_type_id =
	    builder.makeImageType(sampled_format, dim, comparison, arrayed, multisampled, 2, spv::ImageFormatUnknown);

	spv::Id id = spirv_module.allocate_id();
	Operation op;
	op.op = spv::OpSampledImage;
	op.id = id;
	op.type_id = builder.makeSampledImageType(image_type_id);
	op.arguments = { image_id, sampler_id };

	block->ir.operations.push_back(std::move(op));
	return id;
}

spv::Id Converter::Impl::build_vector(CFGNode *block, spv::Id element_type, spv::Id *elements, unsigned count)
{
	if (count == 1)
		return elements[0];

	uint32_t id = spirv_module.allocate_id();
	auto &builder = spirv_module.get_builder();

	Operation op;
	op.op = spv::OpCompositeConstruct;
	op.id = id;
	op.type_id = builder.makeVectorType(element_type, count);
	op.arguments.insert(op.arguments.end(), elements, elements + count);

	block->ir.operations.push_back(std::move(op));
	return id;
}

void Converter::Impl::emit_sample_instruction(DXIL::Op opcode, CFGNode *block, const llvm::CallInst &instruction)
{
	auto &builder = spirv_module.get_builder();
	bool comparison_sampling = opcode == DXIL::Op::SampleCmp || opcode == DXIL::Op::SampleCmpLevelZero;

	spv::Id image_id = handle_to_ptr_id[instruction.getOperand(1)];
	spv::Id sampler_id = handle_to_ptr_id[instruction.getOperand(2)];
	spv::Id combined_image_sampler_id = build_sampled_image(block, image_id, sampler_id, comparison_sampling);

	spv::Id image_type_id = get_type_id(image_id);

	spv::Dim dim = builder.getTypeDimensionality(image_type_id);
	bool arrayed = builder.isArrayedImageType(image_type_id);

	unsigned num_coords;
	switch (dim)
	{
	case spv::Dim1D:
	case spv::DimBuffer:
		num_coords = 1;
		break;

	case spv::Dim2D:
		num_coords = 2;
		break;

	case spv::Dim3D:
	case spv::DimCube:
		num_coords = 3;
		break;

	default:
		LOGE("Unexpected sample dimensionality.\n");
		return;
	}

	unsigned num_coords_full = arrayed ? num_coords + 1 : num_coords;

	spv::Id coord[4] = {};
	for (unsigned i = 0; i < num_coords_full; i++)
		coord[i] = get_id_for_value(*instruction.getOperand(i + 3));

	uint32_t image_ops = 0;

	if (opcode == DXIL::Op::SampleLevel || opcode == DXIL::Op::SampleCmpLevelZero)
		image_ops |= spv::ImageOperandsLodMask;
	else if (opcode == DXIL::Op::SampleBias)
		image_ops |= spv::ImageOperandsBiasMask;

	spv::Id offsets[3] = {};
	for (unsigned i = 0; i < num_coords; i++)
	{
		if (!llvm::isa<llvm::UndefValue>(instruction.getOperand(i + 7)))
		{
			assert(llvm::isa<llvm::ConstantInt>(instruction.getOperand(i + 7)));
			image_ops |= spv::ImageOperandsConstOffsetMask;
			offsets[i] = builder.makeIntConstant(
			    int(llvm::cast<llvm::ConstantInt>(instruction.getOperand(i + 7))->getUniqueInteger().getSExtValue()));
		}
		else
			offsets[i] = builder.makeIntConstant(0);
	}

	spv::Id dref_id = 0;

	if (opcode == DXIL::Op::SampleCmp)
		dref_id = get_id_for_value(*instruction.getOperand(10));

	spv::Id aux_argument = 0;
	unsigned aux_argument_index = opcode == DXIL::Op::SampleCmp ? 11 : 10;

	if (opcode == DXIL::Op::Sample || opcode == DXIL::Op::SampleCmp)
	{
		if (!llvm::isa<llvm::UndefValue>(instruction.getOperand(aux_argument_index)))
		{
			aux_argument = get_id_for_value(*instruction.getOperand(aux_argument_index));
			image_ops |= spv::ImageOperandsMinLodMask;
			builder.addCapability(spv::CapabilityMinLod);
		}
	}
	else if (opcode != DXIL::Op::SampleCmpLevelZero)
		aux_argument = get_id_for_value(*instruction.getOperand(aux_argument_index));
	else
		aux_argument = builder.makeFloatConstant(0.0f);

	Operation op;

	switch (opcode)
	{
	case DXIL::Op::SampleLevel:
		op.op = spv::OpImageSampleExplicitLod;
		break;

	case DXIL::Op::Sample:
	case DXIL::Op::SampleBias:
		op.op = spv::OpImageSampleImplicitLod;
		break;

	case DXIL::Op::SampleCmp:
		op.op = spv::OpImageSampleDrefImplicitLod;
		break;

	case DXIL::Op::SampleCmpLevelZero:
		op.op = spv::OpImageSampleDrefExplicitLod;
		break;

	default:
		break;
	}

	spv::Id sampled_value_id = 0;

	// Comparison sampling only returns a scalar, so we'll need to splat out result.
	if (comparison_sampling)
	{
		sampled_value_id = spirv_module.allocate_id();
		op.id = sampled_value_id;
	}
	else
		op.id = get_id_for_value(instruction);

	auto *result_type = instruction.getType();
	assert(result_type->getTypeID() == llvm::Type::TypeID::StructTyID);

	// For tiled resources, there is a status result in the 5th member, but as long as noone attempts to extract it,
	// we should be fine ...
	assert(result_type->getStructNumElements() == 5);

	op.type_id = get_type_id(*result_type->getStructElementType(0));
	if (!comparison_sampling)
		op.type_id = builder.makeVectorType(op.type_id, 4);

	op.arguments.push_back(combined_image_sampler_id);
	op.arguments.push_back(build_vector(block, builder.makeFloatType(32), coord, num_coords_full));

	if (dref_id)
		op.arguments.push_back(dref_id);

	op.arguments.push_back(image_ops);

	if (image_ops & (spv::ImageOperandsBiasMask | spv::ImageOperandsLodMask))
		op.arguments.push_back(aux_argument);

	if (image_ops & spv::ImageOperandsConstOffsetMask)
	{
		op.arguments.push_back(build_vector(block, builder.makeIntegerType(32, true), offsets, num_coords));
	}

	if (image_ops & spv::ImageOperandsMinLodMask)
		op.arguments.push_back(aux_argument);

	block->ir.operations.push_back(std::move(op));

	if (comparison_sampling)
	{
		op = {};
		op.op = spv::OpCompositeConstruct;
		op.id = get_id_for_value(instruction);
		op.type_id = builder.makeVectorType(builder.makeFloatType(32), 4);
		op.arguments = { sampled_value_id, sampled_value_id, sampled_value_id, sampled_value_id };
		block->ir.operations.push_back(std::move(op));
	}
}

void Converter::Impl::emit_builtin_instruction(CFGNode *block, const llvm::CallInst &instruction)
{
	auto &builder = spirv_module.get_builder();

	// The opcode is encoded as a constant integer.
	auto opcode = static_cast<DXIL::Op>(get_constant_operand(instruction, 0));

	switch (opcode)
	{
	case DXIL::Op::LoadInput:
		emit_load_input_instruction(block, instruction);
		break;

	case DXIL::Op::StoreOutput:
		emit_store_output_instruction(block, instruction);
		break;

	case DXIL::Op::CreateHandle:
		emit_create_handle_instruction(block, instruction);
		break;

	case DXIL::Op::CBufferLoadLegacy:
		emit_cbuffer_load_legacy_instruction(block, instruction);
		break;

	case DXIL::Op::Sample:
	case DXIL::Op::SampleBias:
	case DXIL::Op::SampleLevel:
	case DXIL::Op::SampleCmp:
	case DXIL::Op::SampleCmpLevelZero:
		emit_sample_instruction(opcode, block, instruction);
		break;

	default:
		break;
	}
}

void Converter::Impl::emit_phi_instruction(CFGNode *block, const llvm::PHINode &instruction)
{
	PHI phi;
	phi.id = get_id_for_value(instruction);
	phi.type_id = get_type_id(*instruction.getType());

	unsigned count = instruction.getNumIncomingValues();
	for (unsigned i = 0; i < count; i++)
	{
		IncomingValue incoming = {};
		incoming.block = bb_map[instruction.getIncomingBlock(i)]->node;
		auto *value = instruction.getIncomingValue(i);
		incoming.id = get_id_for_value(*value);
		phi.incoming.push_back(incoming);
	}

	block->ir.phi.push_back(std::move(phi));
}

void Converter::Impl::emit_compare_instruction(CFGNode *block, const llvm::CmpInst &instruction)
{
	auto &builder = spirv_module.get_builder();

	Operation op;
	op.id = get_id_for_value(instruction);
	op.type_id = get_type_id(*instruction.getType());

	uint32_t id0 = get_id_for_value(*instruction.getOperand(0));
	uint32_t id1 = get_id_for_value(*instruction.getOperand(1));
	op.arguments = { id0, id1 };

	switch (instruction.getPredicate())
	{
	case llvm::CmpInst::Predicate::FCMP_OEQ:
		op.op = spv::OpFOrdEqual;
		break;

	case llvm::CmpInst::Predicate::FCMP_UEQ:
		op.op = spv::OpFUnordEqual;
		break;

	case llvm::CmpInst::Predicate::FCMP_OGT:
		op.op = spv::OpFOrdGreaterThan;
		break;

	case llvm::CmpInst::Predicate::FCMP_UGT:
		op.op = spv::OpFUnordGreaterThan;
		break;

	case llvm::CmpInst::Predicate::FCMP_OGE:
		op.op = spv::OpFOrdGreaterThanEqual;
		break;

	case llvm::CmpInst::Predicate::FCMP_UGE:
		op.op = spv::OpFUnordGreaterThanEqual;
		break;

	case llvm::CmpInst::Predicate::FCMP_OLT:
		op.op = spv::OpFOrdLessThan;
		break;

	case llvm::CmpInst::Predicate::FCMP_ULT:
		op.op = spv::OpFUnordLessThan;
		break;

	case llvm::CmpInst::Predicate::FCMP_OLE:
		op.op = spv::OpFOrdLessThanEqual;
		break;

	case llvm::CmpInst::Predicate::FCMP_ULE:
		op.op = spv::OpFUnordLessThanEqual;
		break;

	case llvm::CmpInst::Predicate::FCMP_ONE:
		op.op = spv::OpFOrdNotEqual;
		break;

	case llvm::CmpInst::Predicate::FCMP_UNE:
		op.op = spv::OpFUnordNotEqual;
		break;

	case llvm::CmpInst::Predicate::FCMP_FALSE:
		// Why on earth is this a thing ...
		op.op = spv::OpCopyLogical;
		op.arguments = { builder.makeBoolConstant(false) };
		break;

	case llvm::CmpInst::Predicate::FCMP_TRUE:
		// Why on earth is this a thing ...
		op.op = spv::OpCopyLogical;
		op.arguments = { builder.makeBoolConstant(true) };
		break;

	case llvm::CmpInst::Predicate::ICMP_EQ:
		op.op = spv::OpIEqual;
		break;

	case llvm::CmpInst::Predicate::ICMP_NE:
		op.op = spv::OpINotEqual;
		break;

	case llvm::CmpInst::Predicate::ICMP_SLT:
		op.op = spv::OpSLessThan;
		break;

	case llvm::CmpInst::Predicate::ICMP_SLE:
		op.op = spv::OpSLessThanEqual;
		break;

	case llvm::CmpInst::Predicate::ICMP_SGT:
		op.op = spv::OpSGreaterThan;
		break;

	case llvm::CmpInst::Predicate::ICMP_SGE:
		op.op = spv::OpSGreaterThanEqual;
		break;

	case llvm::CmpInst::Predicate::ICMP_ULT:
		op.op = spv::OpULessThan;
		break;

	case llvm::CmpInst::Predicate::ICMP_ULE:
		op.op = spv::OpULessThanEqual;
		break;

	case llvm::CmpInst::Predicate::ICMP_UGT:
		op.op = spv::OpUGreaterThan;
		break;

	case llvm::CmpInst::Predicate::ICMP_UGE:
		op.op = spv::OpUGreaterThanEqual;
		break;

	default:
		LOGE("Unknown CmpInst predicate.\n");
		break;
	}

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_extract_value_instruction(CFGNode *block, const llvm::ExtractValueInst &instruction)
{
	Operation op;
	op.op = spv::OpCompositeExtract;
	op.id = get_id_for_value(instruction);
	op.type_id = get_type_id(*instruction.getType());

	op.arguments.push_back(get_id_for_value(*instruction.getAggregateOperand()));
	for (unsigned i = 0; i < instruction.getNumIndices(); i++)
	{
		op.arguments.push_back(instruction.getIndices()[i]);
	}

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_alloca_instruction(CFGNode *block, const llvm::AllocaInst &instruction)
{
	auto &builder = spirv_module.get_builder();
	spv::Id pointee_type_id = get_type_id(*instruction.getType()->getPointerElementType());

	// DXC seems to allocate arrays on stack as 1 element of array type rather than N elements of basic non-array type.
	// Should be possible to support both schemes if desirable, but this will do.
	assert(llvm::isa<llvm::ConstantInt>(instruction.getArraySize()));
	assert(llvm::cast<llvm::ConstantInt>(instruction.getArraySize())->getUniqueInteger().getZExtValue() == 1);

	spv::Id var_id = builder.createVariable(spv::StorageClassFunction, pointee_type_id, instruction.getName().data());
	value_map[&instruction] = var_id;
}

void Converter::Impl::emit_select_instruction(CFGNode *block, const llvm::SelectInst &instruction)
{
	Operation op;
	op.op = spv::OpSelect;
	op.id = get_id_for_value(instruction);
	op.type_id = get_type_id(*instruction.getType());

	op.arguments = {
		get_id_for_value(*instruction.getOperand(0)),
		get_id_for_value(*instruction.getOperand(1)),
		get_id_for_value(*instruction.getOperand(2)),
	};

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_load_instruction(CFGNode *block, const llvm::LoadInst &instruction)
{
	Operation op;
	op.op = spv::OpLoad;
	op.id = get_id_for_value(instruction);
	op.type_id = get_type_id(*instruction.getType());
	op.arguments = { get_id_for_value(*instruction.getPointerOperand()) };

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_store_instruction(CFGNode *block, const llvm::StoreInst &instruction)
{
	Operation op;
	op.op = spv::OpStore;
	op.arguments = { get_id_for_value(*instruction.getOperand(1)), get_id_for_value(*instruction.getOperand(0)) };

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_getelementptr_instruction(CFGNode *block, const llvm::GetElementPtrInst &instruction)
{
	// This is actually the same as PtrAccessChain, but we would need to use variable pointers to support that properly.
	// For now, just assert that the first index is constant 0, in which case PtrAccessChain == AccessChain.

	Operation op;
	op.op = instruction.isInBounds() ? spv::OpInBoundsAccessChain : spv::OpAccessChain;
	op.id = get_id_for_value(instruction);
	op.type_id = get_type_id(*instruction.getType());

	unsigned num_operands = instruction.getNumOperands();
	for (uint32_t i = 0; i < num_operands; i++)
	{
		auto *operand = instruction.getOperand(i);
		if (i == 1)
		{
			// This one must be constant 0, ignore it.
			assert(llvm::isa<llvm::ConstantInt>(operand));
			assert(llvm::cast<llvm::ConstantInt>(operand)->getUniqueInteger().getZExtValue() == 0);
		}
		else
			op.arguments.push_back(get_id_for_value(*operand));
	}

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_cast_instruction(CFGNode *block, const llvm::CastInst &instruction)
{
	Operation op;
	op.id = get_id_for_value(instruction);
	op.type_id = get_type_id(*instruction.getType());

	op.arguments = { get_id_for_value(*instruction.getOperand(0)) };

	switch (instruction.getOpcode())
	{
	case llvm::CastInst::CastOps::BitCast:
		op.op = spv::OpBitcast;
		break;

	case llvm::CastInst::CastOps::SExt:
		op.op = spv::OpSConvert;
		break;

	case llvm::CastInst::CastOps::Trunc:
	case llvm::CastInst::CastOps::ZExt:
		op.op = spv::OpUConvert;
		break;

	case llvm::CastInst::CastOps::FPTrunc:
	case llvm::CastInst::CastOps::FPExt:
		op.op = spv::OpFConvert;
		break;

	case llvm::CastInst::CastOps::FPToUI:
		op.op = spv::OpConvertFToU;
		break;

	case llvm::CastInst::CastOps::FPToSI:
		op.op = spv::OpConvertFToS;
		break;

	case llvm::CastInst::CastOps::SIToFP:
		op.op = spv::OpConvertSToF;
		break;

	case llvm::CastInst::CastOps::UIToFP:
		op.op = spv::OpConvertUToF;
		break;

	default:
		LOGE("Unknown cast operation.\n");
		return;
	}

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_unary_instruction(CFGNode *block, const llvm::UnaryOperator &instruction)
{
	Operation op;
	op.id = get_id_for_value(instruction);
	op.type_id = get_type_id(*instruction.getType());

	op.arguments = { get_id_for_value(*instruction.getOperand(0)) };

	switch (instruction.getOpcode())
	{
	case llvm::UnaryOperator::UnaryOps::FNeg:
		op.op = spv::OpFNegate;
		break;

	default:
		LOGE("Unknown unary operator.\n");
		return;
	}

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_binary_instruction(CFGNode *block, const llvm::BinaryOperator &instruction)
{
	Operation op;
	op.id = get_id_for_value(instruction);
	op.type_id = get_type_id(*instruction.getType());

	uint32_t id0 = get_id_for_value(*instruction.getOperand(0));
	uint32_t id1 = get_id_for_value(*instruction.getOperand(1));
	op.arguments = { id0, id1 };

	switch (instruction.getOpcode())
	{
	case llvm::BinaryOperator::BinaryOps::FAdd:
		op.op = spv::OpFAdd;
		break;

	case llvm::BinaryOperator::BinaryOps::FSub:
		op.op = spv::OpFSub;
		break;

	case llvm::BinaryOperator::BinaryOps::FMul:
		op.op = spv::OpFMul;
		break;

	case llvm::BinaryOperator::BinaryOps::FDiv:
		op.op = spv::OpFDiv;
		break;

	case llvm::BinaryOperator::BinaryOps::Add:
		op.op = spv::OpIAdd;
		break;

	case llvm::BinaryOperator::BinaryOps::Sub:
		op.op = spv::OpISub;
		break;

	case llvm::BinaryOperator::BinaryOps::Mul:
		op.op = spv::OpIMul;
		break;

	case llvm::BinaryOperator::BinaryOps::SDiv:
		op.op = spv::OpSDiv;
		break;

	case llvm::BinaryOperator::BinaryOps::UDiv:
		op.op = spv::OpUDiv;
		break;

	case llvm::BinaryOperator::BinaryOps::Shl:
		op.op = spv::OpShiftLeftLogical;
		break;

	case llvm::BinaryOperator::BinaryOps::LShr:
		op.op = spv::OpShiftRightLogical;
		break;

	case llvm::BinaryOperator::BinaryOps::AShr:
		op.op = spv::OpShiftRightArithmetic;
		break;

	case llvm::BinaryOperator::BinaryOps::SRem:
		op.op = spv::OpSRem;
		break;

	case llvm::BinaryOperator::BinaryOps::FRem:
		op.op = spv::OpFRem;
		break;

	case llvm::BinaryOperator::BinaryOps::URem:
		// Is this correct? There is no URem.
		op.op = spv::OpUMod;
		break;

	case llvm::BinaryOperator::BinaryOps::Xor:
		op.op = spv::OpBitwiseXor;
		break;

	case llvm::BinaryOperator::BinaryOps::And:
		op.op = spv::OpBitwiseAnd;
		break;

	case llvm::BinaryOperator::BinaryOps::Or:
		op.op = spv::OpBitwiseOr;
		break;

	default:
		LOGE("Unknown binary operator.\n");
		break;
	}

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_instruction(CFGNode *block, const llvm::Instruction &instruction)
{
	if (auto *call_inst = llvm::dyn_cast<llvm::CallInst>(&instruction))
	{
		auto *called_function = call_inst->getCalledFunction();
		if (strncmp(called_function->getName().data(), "dx.op", 5) == 0)
		{
			emit_builtin_instruction(block, *call_inst);
		}
		else
		{
			LOGE("Normal function call ...\n");
		}
	}
	else if (auto *binary_inst = llvm::dyn_cast<llvm::BinaryOperator>(&instruction))
	{
		emit_binary_instruction(block, *binary_inst);
	}
	else if (auto *unary_inst = llvm::dyn_cast<llvm::UnaryOperator>(&instruction))
	{
		emit_unary_instruction(block, *unary_inst);
	}
	else if (auto *cast_inst = llvm::dyn_cast<llvm::CastInst>(&instruction))
	{
		emit_cast_instruction(block, *cast_inst);
	}
	else if (auto *getelementptr_inst = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction))
	{
		emit_getelementptr_instruction(block, *getelementptr_inst);
	}
	else if (auto *load_inst = llvm::dyn_cast<llvm::LoadInst>(&instruction))
	{
		emit_load_instruction(block, *load_inst);
	}
	else if (auto *store_inst = llvm::dyn_cast<llvm::StoreInst>(&instruction))
	{
		emit_store_instruction(block, *store_inst);
	}
	else if (auto *compare_inst = llvm::dyn_cast<llvm::CmpInst>(&instruction))
	{
		emit_compare_instruction(block, *compare_inst);
	}
	else if (auto *extract_inst = llvm::dyn_cast<llvm::ExtractValueInst>(&instruction))
	{
		emit_extract_value_instruction(block, *extract_inst);
	}
	else if (auto *alloca_inst = llvm::dyn_cast<llvm::AllocaInst>(&instruction))
	{
		emit_alloca_instruction(block, *alloca_inst);
	}
	else if (auto *select_inst = llvm::dyn_cast<llvm::SelectInst>(&instruction))
	{
		emit_select_instruction(block, *select_inst);
	}
	else if (auto *phi_inst = llvm::dyn_cast<llvm::PHINode>(&instruction))
	{
		emit_phi_instruction(block, *phi_inst);
	}
}

ConvertedFunction Converter::Impl::convert_entry_point()
{
	ConvertedFunction result;
	result.node_pool = std::make_unique<CFGNodePool>();
	auto &pool = *result.node_pool;

	auto *module = &bitcode_parser.get_module();
	spirv_module.emit_entry_point(get_execution_model(*module), "main");

	emit_resources();
	emit_stage_input_variables();
	emit_stage_output_variables();

	llvm::Function *func = module->getFunction(get_entry_point_name(*module));
	assert(func);

	auto *entry = &func->getEntryBlock();
	auto entry_meta = std::make_unique<BlockMeta>(entry);
	bb_map[entry] = entry_meta.get();
	result.entry = pool.create_node();
	bb_map[entry]->node = result.entry;
	result.entry->name = entry->getName().data();
	result.entry->name += ".entry";
	metas.push_back(std::move(entry_meta));

	std::vector<llvm::BasicBlock *> to_process;
	std::vector<llvm::BasicBlock *> processing;
	to_process.push_back(entry);
	std::vector<llvm::BasicBlock *> visit_order;

	// Traverse the CFG and register all blocks in the pool.
	while (!to_process.empty())
	{
		std::swap(to_process, processing);
		for (auto *block : processing)
		{
			visit_order.push_back(block);
			for (auto itr = llvm::succ_begin(block); itr != llvm::succ_end(block); ++itr)
			{
				auto *succ = *itr;
				if (!bb_map.count(succ))
				{
					to_process.push_back(succ);
					auto succ_meta = std::make_unique<BlockMeta>(succ);
					bb_map[succ] = succ_meta.get();
					auto *succ_node = pool.create_node();
					bb_map[succ]->node = succ_node;
					succ_node->name = succ->getName().data();
					metas.push_back(std::move(succ_meta));
				}

				bb_map[block]->node->add_branch(bb_map[succ]->node);
			}
		}
		processing.clear();
	}

	for (auto *bb : visit_order)
	{
		CFGNode *node = bb_map[bb]->node;

		// Scan opcodes.
		for (auto &instruction : *bb)
			emit_instruction(node, instruction);

		auto *instruction = bb->getTerminator();
		if (auto *inst = llvm::dyn_cast<llvm::BranchInst>(instruction))
		{
			if (inst->isConditional())
			{
				node->ir.terminator.type = Terminator::Type::Condition;
				node->ir.terminator.conditional_id = get_id_for_value(*inst->getCondition());
				assert(inst->getNumSuccessors() == 2);
				node->ir.terminator.true_block = bb_map[inst->getSuccessor(0)]->node;
				node->ir.terminator.false_block = bb_map[inst->getSuccessor(1)]->node;
			}
			else
			{
				node->ir.terminator.type = Terminator::Type::Branch;
				assert(inst->getNumSuccessors() == 1);
				node->ir.terminator.direct_block = bb_map[inst->getSuccessor(0)]->node;
			}
		}
		else if (auto *inst = llvm::dyn_cast<llvm::SwitchInst>(instruction))
		{
			node->ir.terminator.type = Terminator::Type::Switch;
			node->ir.terminator.default_node = bb_map[inst->getDefaultDest()]->node;
			node->ir.terminator.conditional_id = get_id_for_value(*inst->getCondition());
			for (auto itr = inst->case_begin(); itr != inst->case_end(); ++itr)
			{
				Terminator::Case switch_case = {};
				switch_case.node = bb_map[itr->getCaseSuccessor()]->node;
				switch_case.value = uint32_t(itr->getCaseValue()->getUniqueInteger().getZExtValue());
				node->ir.terminator.cases.push_back(switch_case);
			}
		}
		else if (auto *inst = llvm::dyn_cast<llvm::ReturnInst>(instruction))
		{
			node->ir.terminator.type = Terminator::Type::Return;
			if (inst->getReturnValue())
				node->ir.terminator.return_value = get_id_for_value(*inst->getReturnValue());
		}
		else if (llvm::isa<llvm::UnreachableInst>(instruction))
		{
			node->ir.terminator.type = Terminator::Type::Unreachable;
		}
		else
			LOGE("Unsupported terminator ...\n");
	}

	return result;
}
} // namespace DXIL2SPIRV