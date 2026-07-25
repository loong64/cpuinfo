#include <stdint.h>

#include <cpuinfo.h>
#include <cpuinfo/common.h>
#include <loongarch/api.h>
#include <loongarch/linux/api.h>


static inline bool bitmask_all(uint32_t bitfield, uint32_t mask) {
	return (bitfield & mask) == mask;
}

void cpuinfo_loongarch_linux_decode_processor_uarchs(
	uint32_t max_processors_count,
	struct cpuinfo_loongarch_linux_processor processors[restrict static max_processors_count])
{
	for (uint32_t i = 0; i < max_processors_count; i++) {
		if (!bitmask_all(processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			continue;
		}

		cpuinfo_loongarch_decode_vendor_uarch(
			processors[i].cpucfg_id, &processors[i].vendor, &processors[i].uarch);
	}
}
