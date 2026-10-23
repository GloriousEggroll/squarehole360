#pragma once
#include <rex/ppc/context.h>

// Exact pinned full GetData body, with only its register helpers locally inline.
// No query/report pointers or results are retained across calls.
REX_EXTERN(Gow3GetDataExactInlineLeaf);
bool Gow3GetDataExactInlineLeafAvailable() noexcept;

REX_EXTERN(Gow3GetDataPublishedQueryLeaf);
bool Gow3NativeQueryResultAvailable(uint32_t query,uint32_t device,uint32_t serial,uint8_t flags,uint8_t* base) noexcept;
