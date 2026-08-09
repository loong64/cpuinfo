#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <cpuinfo.h>
#include <cpuinfo/common.h>

#ifndef __cplusplus
	CPUINFO_INTERNAL void cpuinfo_loongarch_decode_vendor_uarch(
		uint32_t cpucfg,
		enum cpuinfo_vendor vendor[restrict static 1],
		enum cpuinfo_uarch uarch[restrict static 1]);

	CPUINFO_INTERNAL void cpuinfo_loongarch_decode_cache(
		enum cpuinfo_uarch uarch,
		uint32_t cluster_cores,
		struct cpuinfo_cache l1i[restrict static 1],
		struct cpuinfo_cache l1d[restrict static 1],
		struct cpuinfo_cache l2[restrict static 1],
		struct cpuinfo_cache l3[restrict static 1]);

	CPUINFO_INTERNAL uint32_t cpuinfo_loongarch_compute_max_cache_size(
		const struct cpuinfo_processor processor[restrict static 1]);
#else /* defined(__cplusplus) */
	CPUINFO_INTERNAL void cpuinfo_loongarch_decode_cache(
		enum cpuinfo_uarch uarch,
		uint32_t cluster_cores,
		struct cpuinfo_cache l1i[1],
		struct cpuinfo_cache l1d[1],
		struct cpuinfo_cache l2[1],
		struct cpuinfo_cache l3[1]);
#endif
