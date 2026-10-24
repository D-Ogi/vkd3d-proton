#ifndef CS_WORKGRAPH_MULTI_GPU_INPUT_MATH_H_
#define CS_WORKGRAPH_MULTI_GPU_INPUT_MATH_H_

// amdgpu-wddm fork: the count arithmetic of the work graph GPU input passes (cs_workgraph_setup_gpu_input.comp and
// cs_workgraph_multi_gpu_input_*.comp). It has no memory access and no atomics, so that the C test
// test_workgraph_multi_gpu_input_count_math can check it for counts near 2^32 without a dispatch.
// The file is GLSL and C. C defines WG_MATH_FN, WG_MATH_U32 and WG_MATH_U64 before it includes the file.
// The GLSL users need GL_EXT_shader_explicit_arithmetic_types_int64.
#ifndef WG_MATH_FN
#define WG_MATH_FN
#define WG_MATH_U32 uint
#define WG_MATH_U64 uint64_t
#endif

// The number of workgroups for count records at divider records per workgroup.
// count + divider - 1 would wrap for counts near 2^32.
WG_MATH_FN WG_MATH_U32 wg_math_div_round_up(WG_MATH_U32 count, WG_MATH_U32 divider)
{
	return count / divider + (count % divider != 0u ? 1u : 0u);
}

// The carry into the high word when value is added to old in a 32-bit word.
WG_MATH_FN WG_MATH_U32 wg_math_add_carry(WG_MATH_U32 old, WG_MATH_U32 value)
{
	WG_MATH_U32 sum = old + value;
	return sum < old ? 1u : 0u;
}

// An input is large if its records would take more than large_size bytes in the staging range. An empty record
// counts as 4 bytes here, so that very large counts of empty records take a direct slot.
WG_MATH_FN bool wg_math_is_large(WG_MATH_U32 num_records, WG_MATH_U32 record_stride, WG_MATH_U32 large_size)
{
	WG_MATH_U64 records = num_records;
	WG_MATH_U64 stride = record_stride > 4u ? record_stride : 4u;
	WG_MATH_U64 limit = large_size;
	return records * stride > limit;
}

// The number of the merged records of an entry point that its merge slot takes: at most 2^32 - 1, because
// NumRecords is 32 bits, and at most the records that fit in room bytes. A stride of 0 needs no room.
// Records are lost if the result is less than merged.
WG_MATH_FN WG_MATH_U64 wg_math_merge_count(WG_MATH_U64 merged, WG_MATH_U32 stride, WG_MATH_U64 room)
{
	WG_MATH_U64 count = merged;
	WG_MATH_U64 max_count = 0xffffffffu;
	if (count > max_count)
		count = max_count;
	if (stride != 0u)
	{
		WG_MATH_U64 wide_stride = stride;
		WG_MATH_U64 fit = room / wide_stride;
		if (count > fit)
			count = fit;
	}
	return count;
}

// The room left in a staging range of capacity bytes after offset bytes.
WG_MATH_FN WG_MATH_U64 wg_math_room(WG_MATH_U64 capacity, WG_MATH_U64 offset)
{
	WG_MATH_U64 zero = 0u;
	return capacity > offset ? capacity - offset : zero;
}

// The offset of the next merge slot: after count records of stride bytes, aligned to 16 bytes.
WG_MATH_FN WG_MATH_U64 wg_math_next_offset(WG_MATH_U64 offset, WG_MATH_U64 count, WG_MATH_U32 stride)
{
	WG_MATH_U64 wide_stride = stride;
	WG_MATH_U64 mask = 15u;
	return (offset + count * wide_stride + mask) & ~mask;
}

// The number of records that a reservation of num records gets when old of fit records are taken.
// The cursor then moves to old plus the result, which is at most fit, so it cannot wrap.
WG_MATH_FN WG_MATH_U32 wg_math_reserve_count(WG_MATH_U32 old, WG_MATH_U32 fit, WG_MATH_U32 num)
{
	if (old >= fit)
		return 0u;
	return num < fit - old ? num : fit - old;
}

#endif
