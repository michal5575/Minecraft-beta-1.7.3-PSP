#pragma once

#ifndef MC_WORLD_CHUNKS
#define MC_WORLD_CHUNKS 8
#endif

// The PSP FPU is single precision only; doubles are emulated in software and
// would make world generation many times slower. Build with -DMC_GEN_DOUBLE to
// use doubles everywhere (bit-closer to Beta, but slow on hardware).
#ifdef MC_GEN_DOUBLE
typedef double real;
#else
typedef float real;
#endif
