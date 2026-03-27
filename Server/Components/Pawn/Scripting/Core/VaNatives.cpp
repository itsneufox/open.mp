/*
 *  This Source Code Form is subject to the terms of the Mozilla Public License,
 *  v. 2.0. If a copy of the MPL was not distributed with this file, You can
 *  obtain one at http://mozilla.org/MPL/2.0/.
 *
 *  The original code is copyright (c) 2022, open.mp team and contributors.
 */

// Variadic argument list natives: va_init, va_get, va_count, va_format,
// va_call_remote, va_call_local.
// These work with the compiler's ... spread operator (opt-in via -V / #pragma variadics).
// A va_list is a 2-cell array: { base_addr, count_bytes } where base_addr is an AMX data
// offset to the first forwarded vararg and count_bytes is the total byte count of those args.
//
// Pawn's vararg ABI passes all arguments by reference (as AMX data offsets), so the cells
// stored in a va_list are exactly the AMX addresses that CallRemoteFunction expects in its
// params array.  va_call_remote / va_call_local exploit this to forward a va_list directly
// into the existing pawn_Script_CallAll / pawn_Script_Call implementations.

#include "sdk.hpp"
#include "../../format.hpp"
#include "../../utils.hpp"
#include <amx/amx.h>
#include <vector>

// Initialise a va_list from the calling function's vararg frame.
// skip: number of fixed (non-variadic) parameters in the calling function to skip over.
// The resulting va[] will point at the first argument after those skipped cells.
// Named va_init (not va_start) to avoid colliding with the C standard library macro.
SCRIPT_API(va_init, bool(cell* va, int skip))
{
	AMX* amx = GetAMX();
	AMX_HEADER* hdr = (AMX_HEADER*)amx->base;
	unsigned char* data = (unsigned char*)amx->base + hdr->dat;

	// amx->frm is the Pawn frame pointer of the function that called this native.
	// Frame layout (each slot is one cell = sizeof(cell) bytes):
	//   [frm+0]  previous frame pointer
	//   [frm+4]  return address
	//   [frm+8]  total argument byte count (all args including fixed ones)
	//   [frm+12] first argument (arg 0)
	cell frm = amx->frm;
	cell arg_bytes = *(cell*)(data + frm + 2 * sizeof(cell));
	cell skip_bytes = (cell)(skip * (int)sizeof(cell));

	if (skip_bytes > arg_bytes)
		skip_bytes = arg_bytes;

	// va[0] = AMX data offset of the first vararg after skipped fixed params
	// va[1] = number of bytes remaining (i.e. number_of_varargs * sizeof(cell))
	va[0] = frm + 3 * (cell)sizeof(cell) + skip_bytes;
	va[1] = arg_bytes - skip_bytes;
	return true;
}

// Read one cell from a va_list at the given zero-based index.
// Returns 0 if index is out of range.
// Named va_get (not va_arg) to avoid colliding with the C standard library macro.
SCRIPT_API(va_get, cell(cell const* va, int index))
{
	AMX* amx = GetAMX();
	AMX_HEADER* hdr = (AMX_HEADER*)amx->base;
	unsigned char* data = (unsigned char*)amx->base + hdr->dat;

	cell base_addr = va[0];
	cell count_bytes = va[1];
	cell offset = (cell)(index * (int)sizeof(cell));

	if (offset >= count_bytes)
		return 0;

	return *(cell*)(data + base_addr + offset);
}

// Return the number of cells in a va_list.
SCRIPT_API(va_count, int(cell const* va))
{
	return (int)(va[1] / sizeof(cell));
}

// Format a string using a va_list as the argument source.
// Equivalent to format(dest, size, fmt, va_arg(va,0), va_arg(va,1), ...) but done natively.
// String arguments in the va_list are AMX data addresses, exactly as pushed by the ... spread.
SCRIPT_API(va_format, bool(cell* dest, int size, cell const* fmt, cell const* va))
{
	if (size <= 0)
		return false;

	AMX* amx = GetAMX();
	AMX_HEADER* hdr = (AMX_HEADER*)amx->base;
	unsigned char* data = (unsigned char*)amx->base + hdr->dat;

	cell base_addr = va[0];
	cell count_bytes = va[1];
	int count = (int)(count_bytes / sizeof(cell));

	// Build a synthetic params array with the standard AMX layout:
	//   synthetic[0] = total byte count
	//   synthetic[1..N] = the vararg cells (ints, floats, or AMX string addresses)
	std::vector<cell> synthetic(count + 1);
	synthetic[0] = count_bytes;
	for (int i = 0; i < count; i++)
		synthetic[i + 1] = *(cell*)(data + base_addr + i * sizeof(cell));

	// AmxStringFormatter with paramOffset=0 reads all cells from synthetic[1..N]
	AmxStringFormatter result(fmt, amx, synthetic.data(), 0);
	StringView sv = result;

	// Write the result back into the Pawn dest[] array
	amx_SetString(dest, std::string(sv).c_str(), false, false, size);
	return true;
}

// Helper: build synthetic params for pawn_Script_Call / pawn_Script_CallAll from a va_list.
// The caller's orig_params[1] and orig_params[2] must be the raw AMX addresses of the
// function-name and format strings respectively.
static std::vector<cell> buildCallParams(AMX* amx, cell const* orig_params, cell const* va)
{
	AMX_HEADER* hdr = (AMX_HEADER*)amx->base;
	unsigned char* data_base = (unsigned char*)amx->base + hdr->dat;

	cell base_addr = va[0];
	cell count_bytes = va[1];
	int count = (int)(count_bytes / sizeof(cell));

	// Layout expected by pawn_Script_Call / pawn_Script_CallAll:
	//   [0]  total byte count = (2 + count) * sizeof(cell)
	//   [1]  AMX address of function-name string
	//   [2]  AMX address of format string
	//   [3+] va_list cells (AMX addresses of each argument, same ABI as CallRemoteFunction)
	std::vector<cell> synthetic(3 + count);
	synthetic[0] = (cell)((2 + count) * sizeof(cell));
	synthetic[1] = orig_params[1];
	synthetic[2] = orig_params[2];
	for (int i = 0; i < count; i++)
		synthetic[3 + i] = *(cell*)(data_base + base_addr + i * sizeof(cell));
	return synthetic;
}

// Equivalent to CallRemoteFunction but takes a va_list instead of raw varargs.
// The format string specifiers ('d','i','f','s','a','v') work identically.
SCRIPT_API(va_call_remote, cell(cell const* funcname, cell const* fmt_str, cell const* va))
{
	AMX* amx = GetAMX();
	auto synthetic = buildCallParams(amx, GetParams(), va);
	return utils::pawn_Script_CallAll(amx, synthetic.data());
}

// Equivalent to CallLocalFunction but takes a va_list instead of raw varargs.
SCRIPT_API(va_call_local, cell(cell const* funcname, cell const* fmt_str, cell const* va))
{
	AMX* amx = GetAMX();
	auto synthetic = buildCallParams(amx, GetParams(), va);
	return utils::pawn_Script_Call(amx, synthetic.data());
}
