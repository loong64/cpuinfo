#define _GNU_SOURCE

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <loongarch/api.h>
#include <loongarch/linux/api.h>

static bool parse_fixture_with_hardware(
	const char* fixture,
	char hardware[restrict static CPUINFO_HARDWARE_VALUE_MAX],
	struct cpuinfo_loongarch_linux_processor processor[restrict static 1])
{
	char filename[] = "/tmp/cpuinfo-loongarch-XXXXXX";
	const int file_descriptor = mkstemp(filename);
	if (file_descriptor < 0) {
		return false;
	}

	FILE* file = fdopen(file_descriptor, "w");
	if (file == NULL) {
		close(file_descriptor);
		unlink(filename);
		return false;
	}
	const bool written = fputs(fixture, file) >= 0;
	const bool closed = fclose(file) == 0;
	if (!written || !closed) {
		unlink(filename);
		return false;
	}

	const bool parsed = cpuinfo_loongarch_linux_parse_cpuinfo(
		filename, hardware, 1, processor);
	unlink(filename);
	return parsed;
}

static bool parse_fixture(
	const char* fixture,
	struct cpuinfo_loongarch_linux_processor processor[restrict static 1])
{
	char hardware[CPUINFO_HARDWARE_VALUE_MAX] = { 0 };
	return parse_fixture_with_hardware(fixture, hardware, processor);
}

static bool test_prid_priority(void) {
	struct cpuinfo_loongarch_linux_processor processor = { 0 };
	if (!parse_fixture(
		"processor : 0\n"
		"Model Name : Loongson-3A5000\n"
		"PRID : LA664 (0014d000)\n"
		"CPU Revision : 0x11\n",
		&processor)) {
		return false;
	}

	if (processor.cpucfg_id != UINT32_C(0x0014d000) ||
		!processor.prid_valid ||
		(processor.flags & CPUINFO_LOONGARCH_LINUX_VALID_CPUCFG) == 0) {
		return false;
	}

	enum cpuinfo_vendor vendor;
	enum cpuinfo_uarch uarch;
	cpuinfo_loongarch_decode_vendor_uarch(processor.cpucfg_id, &vendor, &uarch);
	return vendor == cpuinfo_vendor_loongson && uarch == cpuinfo_uarch_LA664;
}

static bool test_prid_series_with_product_id(void) {
	struct cpuinfo_loongarch_linux_processor processor = { 0 };
	if (!parse_fixture(
		"processor : 0\n"
		"Model Name : Loongson-3A5000\n"
		"PRID : LA664 (0014d100)\n"
		"CPU Revision : 0x11\n",
		&processor)) {
		return false;
	}

	enum cpuinfo_vendor vendor;
	enum cpuinfo_uarch uarch;
	cpuinfo_loongarch_decode_vendor_uarch(processor.cpucfg_id, &vendor, &uarch);
	return processor.cpucfg_id == UINT32_C(0x0014d100) &&
		processor.prid_valid &&
		(processor.flags & CPUINFO_LOONGARCH_LINUX_VALID_CPUCFG) != 0 &&
		vendor == cpuinfo_vendor_loongson && uarch == cpuinfo_uarch_LA664;
}

static bool test_per_processor_cpucfg(void) {
	struct cpuinfo_loongarch_linux_processor processors[3] = {
		{
			.cpucfg_id = UINT32_C(0x0014c000),
			.package_leader_id = 0,
			.flags = CPUINFO_LINUX_FLAG_VALID | CPUINFO_LOONGARCH_LINUX_VALID_CPUCFG,
		},
		{
			.cpucfg_id = UINT32_C(0x0014d100),
			.package_leader_id = 0,
			.flags = CPUINFO_LINUX_FLAG_VALID | CPUINFO_LOONGARCH_LINUX_VALID_CPUCFG,
		},
		{
			.package_leader_id = 0,
			.flags = CPUINFO_LINUX_FLAG_VALID,
		},
	};

	cpuinfo_loongarch_linux_decode_processor_uarchs(3, processors);
	return processors[0].vendor == cpuinfo_vendor_loongson &&
		processors[0].uarch == cpuinfo_uarch_LA464 &&
		processors[1].cpucfg_id == UINT32_C(0x0014d100) &&
		processors[1].vendor == cpuinfo_vendor_loongson &&
		processors[1].uarch == cpuinfo_uarch_LA664 &&
		processors[2].cpucfg_id == 0 &&
		processors[2].uarch == cpuinfo_uarch_unknown &&
		processors[2].vendor == cpuinfo_vendor_unknown &&
		(processors[2].flags & CPUINFO_LOONGARCH_LINUX_VALID_CPUCFG) == 0;
}

static bool test_max_cache_size_fallback(void) {
	struct cpuinfo_core core = {
		.uarch = cpuinfo_uarch_LA664,
	};
	struct cpuinfo_processor processor = {
		.core = &core,
	};
	if (cpuinfo_loongarch_compute_max_cache_size(&processor) != 32 * 1024 * 1024) {
		return false;
	}

	struct cpuinfo_cache l3 = {
		.size = 48 * 1024 * 1024,
	};
	processor.cache.l3 = &l3;
	return cpuinfo_loongarch_compute_max_cache_size(&processor) == 48 * 1024 * 1024;
}

static bool test_unknown_cache_is_unavailable(void) {
	struct cpuinfo_cache l1i, l1d, l2, l3;
	cpuinfo_loongarch_decode_cache(
		cpuinfo_uarch_unknown, 1, &l1i, &l1d, &l2, &l3);
	return l1i.size == 0 && l1d.size == 0 && l2.size == 0 && l3.size == 0;
}

static bool test_model_revision_fallback(void) {
	struct cpuinfo_loongarch_linux_processor processor = { 0 };
	if (!parse_fixture(
		"processor : 0\n"
		"Model Name : Loongson-3A6000\n"
		"CPU Revision : 0x11\n",
		&processor)) {
		return false;
	}

	return processor.cpucfg_id == UINT32_C(0x0014d011) &&
		!processor.prid_valid &&
		(processor.flags & CPUINFO_LOONGARCH_LINUX_VALID_CPUCFG) != 0;
}

static bool test_model_name_is_preserved(void) {
	struct cpuinfo_loongarch_linux_processor processor = { 0 };
	char hardware[CPUINFO_HARDWARE_VALUE_MAX] = { 0 };
	return parse_fixture_with_hardware(
		"processor : 0\n"
		"Model Name : Loongson-3A6000-HV\n"
		"CPU Revision : 0x11\n",
		hardware, &processor) &&
		strcmp(hardware, "3A6000-HV") == 0;
}

static bool test_malformed_prid(void) {
	struct cpuinfo_loongarch_linux_processor processor = { 0 };
	if (!parse_fixture(
		"processor : 0\n"
		"Model Name : Loongson-3A6000\n"
		"PRID : LA664 (0014zzzz)\n"
		"CPU Revision : 0x11\n",
		&processor)) {
		return false;
	}

	return processor.cpucfg_id == UINT32_C(0x0014d011) &&
		!processor.prid_valid &&
		(processor.flags & CPUINFO_LOONGARCH_LINUX_VALID_CPUCFG) != 0;
}

int main(void) {
	return test_prid_priority() &&
		test_prid_series_with_product_id() &&
		test_per_processor_cpucfg() &&
		test_max_cache_size_fallback() &&
		test_unknown_cache_is_unavailable() &&
		test_model_revision_fallback() &&
		test_model_name_is_preserved() &&
		test_malformed_prid() ? 0 : 1;
}
