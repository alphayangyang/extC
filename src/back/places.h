/**
 * @file places.h
 * @brief Emission entry points for the place registry and the dyn object table (see places.c).
 */
#ifndef EXTC_BACK_PLACES_H
#define EXTC_BACK_PLACES_H

#include "base.h"

/** @brief Append the place registry (slot table, chunk allocator) to the generated file. */
void placesEmitRuntime(Arena *a, Buf *out);

/** @brief Append the dyn object table; call after placesEmitRuntime. */
void placesEmitDynRuntime(Arena *a, Buf *out);

#endif
