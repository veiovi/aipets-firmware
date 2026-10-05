/* Minimal include adapter for the pinned, hardened tinfl source. */
#pragma once
#include "miniz_tinfl.h"
#define memcpy fp_miniz_copy
#define memset fp_miniz_set
