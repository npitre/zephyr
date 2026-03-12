/*
 * Copyright (c) 2026 BayLibre SAS
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ExecuTorch inference runner for Corstone-1000-A320 with Ethos-U85 NPU.
 *
 * Based on executorch/examples/arm/zephyr/src/arm_executor_runner.cpp,
 * adapted for Cortex-A targets where the model resides in DDR (directly
 * NPU-accessible via address remapping) rather than flash.
 */

#include <stdio.h>
#include <math.h>
#include <cstring>

#include <zephyr/kernel.h>

#include <executorch/examples/arm/executor_runner/arm_memory_allocator.h>
#include <executorch/extension/data_loader/buffer_data_loader.h>
#include <executorch/extension/runner_util/inputs.h>
#include <executorch/runtime/core/memory_allocator.h>
#include <executorch/runtime/executor/program.h>
#include <executorch/runtime/platform/log.h>
#include <executorch/runtime/platform/platform.h>
#include <executorch/runtime/platform/runtime.h>

/* Generated at build time from the .pte model binary. */
#include "model_pte.h"

using executorch::aten::ScalarType;
using executorch::aten::Tensor;
using executorch::aten::TensorImpl;
using executorch::extension::BufferCleanup;
using executorch::extension::BufferDataLoader;
using executorch::runtime::Error;
using executorch::runtime::EValue;
using executorch::runtime::HierarchicalAllocator;
using executorch::runtime::MemoryAllocator;
using executorch::runtime::MemoryManager;
using executorch::runtime::Method;
using executorch::runtime::MethodMeta;
using executorch::runtime::Program;
using executorch::runtime::Result;
using executorch::runtime::Span;
using executorch::runtime::Tag;
using executorch::runtime::TensorInfo;

#if defined(CONFIG_ETHOS_U)
extern "C" executorch::runtime::Error
executorch_delegate_EthosUBackend_registered(void);
#endif

/*
 * Memory pools.
 *
 * On Cortex-A targets with DDR, we can be generous with allocator sizes.
 * The method allocator holds the loaded method, planned buffers, and input
 * tensors.  The temp allocator is used for scratch during kernel/delegate
 * execution and is reset after each call.
 */
#if !defined(ET_ARM_METHOD_ALLOCATOR_POOL_SIZE)
#define ET_ARM_METHOD_ALLOCATOR_POOL_SIZE (2 * 1024 * 1024)
#endif

static const size_t method_allocation_pool_size =
    ET_ARM_METHOD_ALLOCATOR_POOL_SIZE;
static unsigned char __attribute__((aligned(16)))
    method_allocation_pool[ET_ARM_METHOD_ALLOCATOR_POOL_SIZE];

#if !defined(ET_ARM_BAREMETAL_SCRATCH_TEMP_ALLOCATOR_POOL_SIZE)
#define ET_ARM_BAREMETAL_SCRATCH_TEMP_ALLOCATOR_POOL_SIZE (64 * 1024)
#endif

static const size_t temp_allocation_pool_size =
    ET_ARM_BAREMETAL_SCRATCH_TEMP_ALLOCATOR_POOL_SIZE;
static unsigned char __attribute__((aligned(16)))
    temp_allocation_pool[ET_ARM_BAREMETAL_SCRATCH_TEMP_ALLOCATOR_POOL_SIZE];

/*
 * Prepare input tensors: allocate buffers and fill with ones (dummy input).
 */
static Result<BufferCleanup> prepare_input_tensors(
    Method &method,
    MemoryAllocator &allocator)
{
	MethodMeta method_meta = method.method_meta();
	size_t num_inputs = method_meta.num_inputs();
	size_t num_allocated = 0;

	void **inputs = static_cast<void **>(
	    allocator.allocate(num_inputs * sizeof(void *)));
	ET_CHECK_OR_RETURN_ERROR(
	    inputs != nullptr,
	    MemoryAllocationFailed,
	    "Could not allocate memory for input buffer pointers.");

	for (size_t i = 0; i < num_inputs; i++) {
		auto tag = method_meta.input_tag(i);
		ET_CHECK_OK_OR_RETURN_ERROR(tag.error());

		if (tag.get() != Tag::Tensor) {
			ET_LOG(Debug, "Skipping non-tensor input %zu", i);
			continue;
		}

		Result<TensorInfo> tensor_meta =
		    method_meta.input_tensor_meta(i);
		ET_CHECK_OK_OR_RETURN_ERROR(tensor_meta.error());

		void *data_ptr = allocator.allocate(tensor_meta->nbytes());
		ET_CHECK_OR_RETURN_ERROR(
		    data_ptr != nullptr,
		    MemoryAllocationFailed,
		    "Could not allocate memory for input buffer.");
		inputs[num_allocated++] = data_ptr;

		TensorImpl impl = TensorImpl(
		    tensor_meta.get().scalar_type(),
		    tensor_meta.get().sizes().size(),
		    const_cast<TensorImpl::SizesType *>(
			tensor_meta.get().sizes().data()),
		    data_ptr,
		    const_cast<TensorImpl::DimOrderType *>(
			tensor_meta.get().dim_order().data()));
		Tensor t(&impl);

		/* Fill with ones as dummy input. */
		for (size_t j = 0; j < static_cast<size_t>(t.numel()); j++) {
			switch (t.scalar_type()) {
			case ScalarType::Int:
				t.mutable_data_ptr<int>()[j] = 1;
				break;
			case ScalarType::Float:
				t.mutable_data_ptr<float>()[j] = 1.0f;
				break;
			case ScalarType::Char:
				t.mutable_data_ptr<int8_t>()[j] = 1;
				break;
			default:
				break;
			}
		}

		Error err = method.set_input(t, i);
		if (err != Error::Ok) {
			ET_LOG(Error,
			       "Failed to set input %zu: 0x%" PRIx32,
			       i, (uint32_t)err);
			BufferCleanup cleanup({inputs, num_allocated});
			return err;
		}
	}

	return BufferCleanup({inputs, num_allocated});
}

int main(void)
{
	ET_LOG(Info, "ExecuTorch + Ethos-U85 on Corstone-1000-A320");

#if defined(CONFIG_ETHOS_U)
	if (executorch_delegate_EthosUBackend_registered() != Error::Ok) {
		ET_LOG(Error, "Ethos-U backend registration failed");
		return 1;
	}
	ET_LOG(Info, "Ethos-U backend registered");
#endif

	executorch::runtime::runtime_init();

	/*
	 * On Cortex-A with DDR, the model data in the ELF's .rodata is
	 * directly accessible by the NPU (via address remapping in the
	 * Ethos-U driver's ethosu_address_remap()).  No flash-to-SRAM
	 * copy is needed unlike Cortex-M targets.
	 */
	const void *program_data = model_pte;
	size_t program_data_len = sizeof(model_pte);

	ET_LOG(Info, "Model at %p, size: %zu bytes",
	       program_data, program_data_len);

	auto loader = BufferDataLoader(program_data, program_data_len);

	Result<Program> program = Program::load(&loader);
	if (!program.ok()) {
		ET_LOG(Error, "Program load failed: 0x%" PRIx32,
		       static_cast<uint32_t>(program.error()));
		return 1;
	}

	ET_LOG(Info, "Model loaded, %zu methods", program->num_methods());

	const char *method_name = nullptr;
	{
		const auto method_name_result = program->get_method_name(0);
		ET_CHECK_MSG(method_name_result.ok(),
			     "Program has no methods");
		method_name = *method_name_result;
	}
	ET_LOG(Info, "Running method: %s", method_name);

	Result<MethodMeta> method_meta = program->method_meta(method_name);
	if (!method_meta.ok()) {
		ET_LOG(Error, "Failed to get method_meta: 0x%x",
		       (unsigned int)method_meta.error());
		return 1;
	}

	/* Set up allocators. */
	ArmMemoryAllocator method_allocator(
	    method_allocation_pool_size, method_allocation_pool);

	/* Allocate memory-planned buffers. */
	size_t num_planned = method_meta->num_memory_planned_buffers();
	/* Use VLAs would be simpler but let's use the allocator pattern. */
	uint8_t *planned_buf_ptrs[16];
	Span<uint8_t> planned_spans[16];

	ET_CHECK_MSG(num_planned <= 16, "Too many planned buffers");

	for (size_t id = 0; id < num_planned; ++id) {
		size_t buf_size = static_cast<size_t>(
		    method_meta->memory_planned_buffer_size(id).get());
		ET_LOG(Info, "Planned buffer %zu: %zu bytes", id, buf_size);

		uint8_t *buf = reinterpret_cast<uint8_t *>(
		    method_allocator.allocate(buf_size));
		ET_CHECK_MSG(buf != nullptr,
			     "Failed to allocate planned buffer %zu", id);
		planned_buf_ptrs[id] = buf;
		planned_spans[id] = {buf, buf_size};
	}

	HierarchicalAllocator planned_memory(
	    {planned_spans, num_planned});

	ArmMemoryAllocator temp_allocator(
	    temp_allocation_pool_size,
	    const_cast<unsigned char *>(temp_allocation_pool));

	MemoryManager memory_manager(
	    &method_allocator, &planned_memory, &temp_allocator);

	/* Load the method. */
	ET_LOG(Info, "Loading method...");
	Result<Method> method = program->load_method(
	    method_name, &memory_manager, nullptr);

	if (!method.ok()) {
		ET_LOG(Error, "Method load failed: 0x%" PRIx32,
		       static_cast<uint32_t>(method.error()));
		return 1;
	}
	ET_LOG(Info, "Method loaded. Allocator used: %zu / %zu bytes",
	       method_allocator.used_size(), method_allocator.size());

	/* Prepare inputs (fill with ones). */
	ET_LOG(Info, "Preparing inputs...");
	{
		static auto prepared =
		    prepare_input_tensors(*method, method_allocator);
		if (!prepared.ok()) {
			ET_LOG(Error, "Input preparation failed: 0x%" PRIx32,
			       static_cast<uint32_t>(prepared.error()));
			return 1;
		}
	}

	/* Execute. */
	ET_LOG(Info, "Executing model...");
	Error status = method->execute();

	if (status != Error::Ok) {
		ET_LOG(Error, "Execution failed: 0x%" PRIx32,
		       static_cast<uint32_t>(status));
		return 1;
	}
	ET_LOG(Info, "Model executed successfully.");

	/* Print outputs. */
	size_t num_outputs = method->outputs_size();
	EValue outputs[8];
	ET_CHECK_MSG(num_outputs <= 8, "Too many outputs");
	status = method->get_outputs(outputs, num_outputs);
	ET_CHECK(status == Error::Ok);

	ET_LOG(Info, "Outputs (%zu):", num_outputs);
	for (size_t i = 0; i < num_outputs; ++i) {
		if (!outputs[i].isTensor()) {
			ET_LOG(Info, "  [%zu]: non-tensor", i);
			continue;
		}
		Tensor tensor = outputs[i].toTensor();
		ET_LOG(Info, "  [%zu]: %s, numel=%zd",
		       i,
		       executorch::runtime::toString(tensor.scalar_type()),
		       tensor.numel());

		for (ssize_t j = 0; j < tensor.numel(); ++j) {
			switch (tensor.scalar_type()) {
			case ScalarType::Int:
				ET_LOG(Info, "    [%zd] = %d",
				       j, tensor.const_data_ptr<int>()[j]);
				break;
			case ScalarType::Float:
				ET_LOG(Info, "    [%zd] = %f",
				       j, static_cast<double>(
					      tensor.const_data_ptr<float>()[j]));
				break;
			case ScalarType::Char:
				ET_LOG(Info, "    [%zd] = %d",
				       j, tensor.const_data_ptr<int8_t>()[j]);
				break;
			default:
				ET_LOG(Info, "    (dump skipped for %s)",
				       executorch::runtime::toString(
					   tensor.scalar_type()));
				break;
			}
		}
	}

	ET_LOG(Info, "SUCCESS: program complete.");
	return 0;
}
