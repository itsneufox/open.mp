/*
 *  This Source Code Form is subject to the terms of the Mozilla Public License,
 *  v. 2.0. If a copy of the MPL was not distributed with this file, You can
 *  obtain one at http://mozilla.org/MPL/2.0/.
 *
 *  The original code is copyright (c) 2024, open.mp team and contributors.
 */

#include "../Types.hpp"
#include "../../format.hpp"
#include "../../utils.hpp"
#include <memory>
#include <vector>

// ---------------------------------------------------------------------------
// Helper: build synthetic params[] for pawn_Script_Call / pawn_Script_CallAll.
// orig_params[1] and [2] must be raw AMX addresses (from GetParams()).
//   synthetic[0] = (2 + count) * sizeof(cell)
//   synthetic[1] = funcname AMX addr
//   synthetic[2] = fmt AMX addr
//   synthetic[3+i] = AMX addr of the i-th forwarded argument
// ---------------------------------------------------------------------------
static std::vector<cell> va_build_call_params(AMX* amx, cell const* orig_params, cell const* va)
{
	AMX_HEADER* hdr = (AMX_HEADER*)amx->base;
	unsigned char* data = (unsigned char*)amx->base + hdr->dat;
	cell base_addr = va[0];
	cell count_bytes = va[1];
	int count = (int)(count_bytes / sizeof(cell));
	std::vector<cell> synthetic(3 + count);
	synthetic[0] = (cell)((2 + count) * (int)sizeof(cell));
	synthetic[1] = orig_params[1];
	synthetic[2] = orig_params[2];
	for (int i = 0; i < count; i++)
		synthetic[3 + i] = *(cell*)(data + base_addr + i * sizeof(cell));
	return synthetic;
}

// ---------------------------------------------------------------------------
// va_init(va[2], skip = 0)
// Capture the calling Pawn function's vararg frame into a va_list.
// ---------------------------------------------------------------------------
SCRIPT_API(va_init, bool(cell* vaOut, int skip))
{
	AMX* amx = GetAMX();
	AMX_HEADER* hdr = (AMX_HEADER*)amx->base;
	unsigned char* data = (unsigned char*)amx->base + hdr->dat;

	if (skip < 0)
		skip = 0;

	// amx->frm is the frame of the Pawn function that called this native.
	// Layout: [frm+0]=prev_frm, [frm+4]=ret_addr, [frm+8]=arg_bytes, [frm+12]=first_arg
	cell frm = amx->frm;
	cell arg_bytes = *(cell*)(data + frm + 2 * sizeof(cell));
	cell skip_bytes = (cell)(skip * (int)sizeof(cell));
	if (skip_bytes > arg_bytes)
		skip_bytes = arg_bytes;

	vaOut[0] = frm + 3 * (cell)sizeof(cell) + skip_bytes; // base_addr
	vaOut[1] = arg_bytes - skip_bytes;                     // count_bytes
	return true;
}

// ---------------------------------------------------------------------------
// va_count(const va[2])
// Return the number of varargs captured in a va_list.
// ---------------------------------------------------------------------------
SCRIPT_API(va_count, int(cell const* va))
{
	return (int)(va[1] / sizeof(cell));
}

// ---------------------------------------------------------------------------
// va_get(const va[2], index)
// Read a single cell (int or float) from a va_list by zero-based index.
// ---------------------------------------------------------------------------
SCRIPT_API(va_get, int(cell const* va, int index))
{
	AMX* amx = GetAMX();
	AMX_HEADER* hdr = (AMX_HEADER*)amx->base;
	unsigned char* data = (unsigned char*)amx->base + hdr->dat;

	cell base_addr = va[0];
	cell count_bytes = va[1];
	cell offset = (cell)(index * (int)sizeof(cell));
	if (offset < 0 || offset >= count_bytes)
		return 0;

	cell ref = *(cell*)(data + base_addr + offset);
	return (int)*(cell*)(data + ref);
}

// ---------------------------------------------------------------------------
// va_get_str(const va[2], index, dest[], maxsize = sizeof(dest))
// Copy a string vararg from a va_list into dest by zero-based index.
// Returns number of characters copied (excluding NUL), or 0 on failure.
// ---------------------------------------------------------------------------
SCRIPT_API(va_get_str, int(cell const* va, int index, cell* dest, int maxsize))
{
	AMX* amx = GetAMX();
	AMX_HEADER* hdr = (AMX_HEADER*)amx->base;
	unsigned char* data = (unsigned char*)amx->base + hdr->dat;

	cell base_addr = va[0];
	cell count_bytes = va[1];
	cell offset = (cell)(index * (int)sizeof(cell));
	if (offset < 0 || offset >= count_bytes)
		return 0;
	if (maxsize <= 0)
		return 0;

	cell ref = *(cell*)(data + base_addr + offset);
	cell* src = (cell*)(data + ref);

	int i = 0;
	while (i < maxsize - 1 && src[i] != 0)
	{
		dest[i] = src[i];
		i++;
	}
	dest[i] = 0;
	return i;
}

// ---------------------------------------------------------------------------
// va_format(dest[], size = sizeof(dest), const fmt[], const va[2])
// Format a string using a va_list as the argument source.
// ---------------------------------------------------------------------------
SCRIPT_API(va_format, bool(cell* dest, int size, cell const* fmt, cell const* va))
{
	AMX* amx = GetAMX();
	AMX_HEADER* hdr = (AMX_HEADER*)amx->base;
	unsigned char* data = (unsigned char*)amx->base + hdr->dat;

	if (size <= 0)
		return false;

	cell base_addr = va[0];
	cell count_bytes = va[1];
	int count = (int)(count_bytes / sizeof(cell));

	// Build synthetic params matching what atcprintf expects.
	std::vector<cell> synthetic(count + 1);
	synthetic[0] = count_bytes;
	for (int i = 0; i < count; i++)
		synthetic[i + 1] = *(cell*)(data + base_addr + i * sizeof(cell));

	cell staticOutput[4096];
	cell* output = staticOutput;
	std::unique_ptr<cell[]> dynamicOutput;
	if (size > (int)(sizeof(staticOutput) / sizeof(cell)))
	{
		dynamicOutput = std::make_unique<cell[]>(size);
		output = dynamicOutput.get();
	}

	int param = 1;
	size_t len = atcprintf(output, size - 1, fmt, amx, synthetic.data(), &param);
	memcpy(dest, output, (len + 1) * sizeof(cell));
	return true;
}

// ---------------------------------------------------------------------------
// va_call_remote(const funcname[], const fmt[], const va[2])
// Equivalent to CallRemoteFunction but driven by a va_list.
// ---------------------------------------------------------------------------
SCRIPT_API(va_call_remote, int(cell const* funcname, cell const* fmt, cell const* va))
{
	AMX* amx = GetAMX();
	cell const* params = GetParams();
	auto synthetic = va_build_call_params(amx, params, va);
	return (int)utils::pawn_Script_CallAll(amx, synthetic.data());
}

// ---------------------------------------------------------------------------
// va_call_local(const funcname[], const fmt[], const va[2])
// Equivalent to CallLocalFunction but driven by a va_list.
// ---------------------------------------------------------------------------
SCRIPT_API(va_call_local, int(cell const* funcname, cell const* fmt, cell const* va))
{
	AMX* amx = GetAMX();
	cell const* params = GetParams();
	auto synthetic = va_build_call_params(amx, params, va);
	return (int)utils::pawn_Script_Call(amx, synthetic.data());
}
