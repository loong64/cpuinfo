#include <stdint.h>
#include <inttypes.h>

#include <loongarch/api.h>
#include <loongarch/cpucfg.h>
#include <cpuinfo/log.h>


void cpuinfo_loongarch_decode_vendor_uarch(
	uint32_t cpucfg,
	enum cpuinfo_vendor vendor[restrict static 1],
	enum cpuinfo_uarch uarch[restrict static 1])
{
	*vendor = cpuinfo_vendor_unknown;
	*uarch = cpuinfo_uarch_unknown;

	switch (cpucfg_get_companyID(cpucfg)) {
		case 0x14:
			*vendor = cpuinfo_vendor_loongson;
			switch (cpucfg_get_series(cpucfg)) {
				case 0xc:
					*uarch = cpuinfo_uarch_LA464;
					break;
				case 0xd:
					*uarch = cpuinfo_uarch_LA664;
					break;
			}
			if (*uarch == cpuinfo_uarch_unknown) {
				cpuinfo_log_warning("unknown Loongson series ID: 0x%" PRIx32,
					cpucfg_get_series(cpucfg));
			}
			break;
		default:
			break;
	}
}
