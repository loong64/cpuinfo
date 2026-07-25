#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cpuinfo.h>
#include <loongarch/linux/api.h>
#include <loongarch/api.h>
#include <loongarch/cpucfg.h>
#include <linux/api.h>
#include <cpuinfo/internal-api.h>
#include <cpuinfo/log.h>


struct cpuinfo_loongarch_isa cpuinfo_isa = { 0 };

#define CPUINFO_LOONGARCH_CACHE_MAX_LEAVES 64

static inline bool bitmask_all(uint32_t bitfield, uint32_t mask) {
	return (bitfield & mask) == mask;
}

static inline uint32_t min(uint32_t a, uint32_t b) {
	return a < b ? a : b;
}

static inline int cmp(uint32_t a, uint32_t b) {
	return (a > b) - (a < b);
}

static bool sysfs_uint32_parser(
	const char* filename, const char* text_start, const char* text_end, void* context)
{
	(void) filename;
	if (text_start == text_end) {
		return false;
	}

	uint32_t value = 0;
	const char* p = text_start;
	while (p != text_end && *p >= '0' && *p <= '9') {
		value = value * 10 + (uint32_t) (*p - '0');
		p++;
	}
	if (p == text_start) {
		return false;
	}
	for (; p != text_end; p++) {
		if (*p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') {
			return false;
		}
	}
	*(uint32_t*) context = value;
	return true;
}

static bool sysfs_cache_size_parser(
	const char* filename, const char* text_start, const char* text_end, void* context)
{
	(void) filename;
	if (text_start == text_end) {
		return false;
	}

	uint64_t value = 0;
	const char* p = text_start;
	while (p != text_end && *p >= '0' && *p <= '9') {
		const uint64_t digit = (uint64_t) (*p - '0');
		if (value > (UINT64_MAX - digit) / UINT64_C(10)) {
			return false;
		}
		value = value * UINT64_C(10) + digit;
		p++;
	}
	if (p == text_start) {
		return false;
	}
	if (p != text_end && (*p == 'K' || *p == 'k')) {
		if (value > UINT64_MAX / UINT64_C(1024)) {
			return false;
		}
		value *= UINT64_C(1024);
		p++;
	} else if (p != text_end && (*p == 'M' || *p == 'm')) {
		if (value > UINT64_MAX / (UINT64_C(1024) * UINT64_C(1024))) {
			return false;
		}
		value *= UINT64_C(1024) * UINT64_C(1024);
		p++;
	}
	for (; p != text_end; p++) {
		if (*p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') {
			return false;
		}
	}
	if (value == 0 || value > UINT32_MAX) {
		return false;
	}
	*(uint32_t*) context = (uint32_t) value;
	return true;
}

static bool sysfs_cache_type_parser(
	const char* filename, const char* text_start, const char* text_end, void* context)
{
	(void) filename;
	while (text_end != text_start &&
		(text_end[-1] == '\n' || text_end[-1] == '\r' ||
			text_end[-1] == ' ' || text_end[-1] == '\t')) {
		text_end--;
	}
	const size_t length = (size_t) (text_end - text_start);
	const bool valid = (length == 7 && memcmp(text_start, "Unified", 7) == 0) ||
		(length == 4 && memcmp(text_start, "Data", 4) == 0);
	*(bool*) context = valid;
	return valid;
}

static bool core_siblings_parser(
	uint32_t processor,
	uint32_t siblings_start,
	uint32_t siblings_end,
	struct cpuinfo_loongarch_linux_processor* processors)
{
	const uint32_t previous_core_leader_id = processors[processor].core_leader_id;
	uint32_t core_leader_id = previous_core_leader_id;
	for (uint32_t sibling = siblings_start; sibling < siblings_end; sibling++) {
		if (bitmask_all(processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID) && sibling < core_leader_id) {
			core_leader_id = sibling;
		}
	}
	processors[processor].core_leader_id = core_leader_id;
	if (core_leader_id != previous_core_leader_id) {
		/* cpulist callbacks split comma-separated entries; update earlier entries too. */
		for (uint32_t sibling = 0; sibling < siblings_end; sibling++) {
			if (bitmask_all(processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID) &&
				processors[sibling].core_leader_id == previous_core_leader_id) {
				processors[sibling].core_leader_id = core_leader_id;
			}
		}
	}

	for (uint32_t sibling = siblings_start; sibling < siblings_end; sibling++) {
		if (!bitmask_all(processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID)) {
			continue;
		}
		processors[sibling].core_leader_id = core_leader_id;
		processors[sibling].core_topology_valid = true;
		if (!processors[sibling].core_id_valid) {
			processors[sibling].core_id = core_leader_id;
			processors[sibling].core_id_valid = true;
		}
	}
	return true;
}

static bool cluster_cpus_parser(
	uint32_t processor,
	uint32_t siblings_start,
	uint32_t siblings_end,
	struct cpuinfo_loongarch_linux_processor* processors)
{
	const uint32_t previous_cluster_leader_id = processors[processor].cluster_leader_id;
	uint32_t cluster_leader_id = previous_cluster_leader_id;
	for (uint32_t sibling = siblings_start; sibling < siblings_end; sibling++) {
		if (bitmask_all(processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID) &&
			sibling < cluster_leader_id) {
			cluster_leader_id = sibling;
		}
	}
	processors[processor].cluster_leader_id = cluster_leader_id;
	if (cluster_leader_id != previous_cluster_leader_id) {
		for (uint32_t sibling = 0; sibling < siblings_end; sibling++) {
			if (bitmask_all(processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID) &&
				processors[sibling].cluster_leader_id == previous_cluster_leader_id) {
				processors[sibling].cluster_leader_id = cluster_leader_id;
			}
		}
	}

	for (uint32_t sibling = siblings_start; sibling < siblings_end; sibling++) {
		if (bitmask_all(processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID)) {
			processors[sibling].cluster_leader_id = cluster_leader_id;
			processors[sibling].cluster_topology_valid = true;
		}
	}
	return true;
}

static void set_l3_geometry(
	struct cpuinfo_loongarch_linux_processor* processor,
	uint32_t size,
	uint32_t associativity,
	uint32_t sets,
	uint32_t line_size,
	uint32_t partitions)
{
	processor->l3_size = size;
	processor->l3_associativity = associativity;
	processor->l3_sets = sets;
	processor->l3_line_size = line_size;
	processor->l3_partitions = partitions;
	processor->l3_geometry_valid = true;
}

static void apply_l3_geometry(
	const struct cpuinfo_loongarch_linux_processor* processor,
	struct cpuinfo_cache* cache)
{
	if (processor->l3_geometry_valid) {
		*cache = (struct cpuinfo_cache) {
			.size = processor->l3_size,
			.associativity = processor->l3_associativity,
			.sets = processor->l3_sets,
			.partitions = processor->l3_partitions,
			.line_size = processor->l3_line_size,
		};
	}
}

struct l3_siblings_parser_state {
	uint32_t processor;
	uint32_t max_processors;
	struct cpuinfo_loongarch_linux_processor* processors;
};

static bool l3_siblings_parser(uint32_t siblings_start, uint32_t siblings_end, void* context) {
	struct l3_siblings_parser_state* state = (struct l3_siblings_parser_state*) context;
	if (siblings_start >= state->max_processors) {
		return true;
	}
	if (siblings_end > state->max_processors) {
		siblings_end = state->max_processors;
	}

	uint32_t l3_leader_id = state->processors[state->processor].l3_leader_id;
	for (uint32_t sibling = siblings_start; sibling < siblings_end; sibling++) {
		if (!bitmask_all(state->processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID)) {
			continue;
		}
		state->processors[sibling].l3_id_valid = true;
		if (state->processors[sibling].l3_leader_id < l3_leader_id) {
			l3_leader_id = state->processors[sibling].l3_leader_id;
		}
	}

	state->processors[state->processor].l3_id_valid = true;
	state->processors[state->processor].l3_leader_id = l3_leader_id;
	for (uint32_t sibling = siblings_start; sibling < siblings_end; sibling++) {
		if (bitmask_all(state->processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID)) {
			state->processors[sibling].l3_leader_id = l3_leader_id;
		}
	}
	return true;
}

static bool detect_l3_siblings(
	uint32_t processor,
	uint32_t max_processors,
	struct cpuinfo_loongarch_linux_processor processors[restrict static max_processors])
{
	char filename[128];
	uint32_t cache_level;
	uint32_t cache_index = 0;
	bool found_l3 = false;
	for (; cache_index < CPUINFO_LOONGARCH_CACHE_MAX_LEAVES; cache_index++) {
		snprintf(filename, sizeof(filename),
			"/sys/devices/system/cpu/cpu%" PRIu32 "/cache/index%" PRIu32 "/level",
			processor, cache_index);
		if (!cpuinfo_linux_parse_small_file(filename, 16, sysfs_uint32_parser, &cache_level)) {
			break;
		}
		if (cache_level == 3) {
			bool valid_type = false;
			snprintf(filename, sizeof(filename),
				"/sys/devices/system/cpu/cpu%" PRIu32 "/cache/index%" PRIu32 "/type",
				processor, cache_index);
			if (cpuinfo_linux_parse_small_file(
				filename, 16, sysfs_cache_type_parser, &valid_type) && valid_type) {
				found_l3 = true;
				break;
			}
		}
	}
	if (!found_l3) {
		return false;
	}

	snprintf(filename, sizeof(filename),
		"/sys/devices/system/cpu/cpu%" PRIu32 "/cache/index%" PRIu32 "/shared_cpu_list",
		processor, cache_index);
	struct l3_siblings_parser_state state = {
		.processor = processor,
		.max_processors = max_processors,
		.processors = processors,
	};
	if (!cpuinfo_linux_parse_cpulist(filename, l3_siblings_parser, &state) ||
		!processors[processor].l3_id_valid) {
		return false;
	}

	uint32_t size = 0;
	uint32_t associativity = 0;
	uint32_t sets = 0;
	uint32_t line_size = 0;
	uint32_t partitions = 0;
	bool geometry_valid = true;
	snprintf(filename, sizeof(filename),
		"/sys/devices/system/cpu/cpu%" PRIu32 "/cache/index%" PRIu32 "/size",
		processor, cache_index);
	geometry_valid &= cpuinfo_linux_parse_small_file(
		filename, 32, sysfs_cache_size_parser, &size);
	snprintf(filename, sizeof(filename),
		"/sys/devices/system/cpu/cpu%" PRIu32 "/cache/index%" PRIu32 "/ways_of_associativity",
		processor, cache_index);
	geometry_valid &= cpuinfo_linux_parse_small_file(
		filename, 16, sysfs_uint32_parser, &associativity);
	snprintf(filename, sizeof(filename),
		"/sys/devices/system/cpu/cpu%" PRIu32 "/cache/index%" PRIu32 "/number_of_sets",
		processor, cache_index);
	geometry_valid &= cpuinfo_linux_parse_small_file(
		filename, 16, sysfs_uint32_parser, &sets);
	snprintf(filename, sizeof(filename),
		"/sys/devices/system/cpu/cpu%" PRIu32 "/cache/index%" PRIu32 "/coherency_line_size",
		processor, cache_index);
	geometry_valid &= cpuinfo_linux_parse_small_file(
		filename, 16, sysfs_uint32_parser, &line_size);
	snprintf(filename, sizeof(filename),
		"/sys/devices/system/cpu/cpu%" PRIu32 "/cache/index%" PRIu32 "/physical_line_partition",
		processor, cache_index);
	if (!cpuinfo_linux_parse_small_file(filename, 16, sysfs_uint32_parser, &partitions)) {
		/* Older kernels do not expose this optional field. */
		partitions = 1;
	}
	geometry_valid = geometry_valid && size != 0 && associativity != 0 &&
		sets != 0 && line_size != 0 && partitions != 0;
	if (geometry_valid) {
		const uint32_t l3_leader_id = processors[processor].l3_leader_id;
		for (uint32_t sibling = 0; sibling < max_processors; sibling++) {
			if (bitmask_all(processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID) &&
				processors[sibling].l3_leader_id == l3_leader_id) {
				set_l3_geometry(
					&processors[sibling], size, associativity, sets, line_size, partitions);
			}
		}
	}
	return true;
}

static bool cluster_siblings_parser(
	uint32_t processor, uint32_t siblings_start, uint32_t siblings_end,
	struct cpuinfo_loongarch_linux_processor* processors)
{
	processors[processor].flags |= CPUINFO_LINUX_FLAG_PACKAGE_CLUSTER;
	uint32_t package_leader_id = processors[processor].package_leader_id;

	for (uint32_t sibling = siblings_start; sibling < siblings_end; sibling++) {
		if (!bitmask_all(processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID)) {
			cpuinfo_log_info("invalid processor %"PRIu32" reported as a sibling for processor %"PRIu32,
				sibling, processor);
			continue;
		}

		const uint32_t sibling_package_leader_id = processors[sibling].package_leader_id;
		if (sibling_package_leader_id < package_leader_id) {
			package_leader_id = sibling_package_leader_id;
		}

		processors[sibling].package_leader_id = package_leader_id;
		processors[sibling].flags |= CPUINFO_LINUX_FLAG_PACKAGE_CLUSTER;
	}

	processors[processor].package_leader_id = package_leader_id;

	return true;
}

static int cmp_loongarch_linux_processor(const void* ptr_a, const void* ptr_b) {
	const struct cpuinfo_loongarch_linux_processor* processor_a = (const struct cpuinfo_loongarch_linux_processor*) ptr_a;
	const struct cpuinfo_loongarch_linux_processor* processor_b = (const struct cpuinfo_loongarch_linux_processor*) ptr_b;

	/* Move usable processors towards the start of the array */
	const bool usable_a = bitmask_all(processor_a->flags, CPUINFO_LINUX_FLAG_VALID);
	const bool usable_b = bitmask_all(processor_b->flags, CPUINFO_LINUX_FLAG_VALID);
	if (usable_a != usable_b) {
		return (int) usable_b - (int) usable_a;
	}

	/* Keep package, cluster, L3 domain, core, and SMT siblings contiguous. */
	int result = cmp(processor_a->package_leader_id, processor_b->package_leader_id);
	if (result != 0) {
		return result;
	}
	result = cmp(processor_a->cluster_leader_id, processor_b->cluster_leader_id);
	if (result != 0) {
		return result;
	}
	result = cmp(processor_a->l3_leader_id, processor_b->l3_leader_id);
	if (result != 0) {
		return result;
	}
	result = cmp(processor_a->core_leader_id, processor_b->core_leader_id);
	if (result != 0) {
		return result;
	}
	return cmp(processor_a->system_processor_id, processor_b->system_processor_id);
}

void cpuinfo_loongarch_linux_init(void) {

	struct cpuinfo_loongarch_linux_processor* loongarch_linux_processors = NULL;
	struct cpuinfo_processor* processors = NULL;
	struct cpuinfo_core* cores = NULL;
	struct cpuinfo_cluster* clusters = NULL;
	struct cpuinfo_package* packages = NULL;
	struct cpuinfo_uarch_info* uarchs = NULL;
	const struct cpuinfo_processor** linux_cpu_to_processor_map = NULL;
	const struct cpuinfo_core** linux_cpu_to_core_map = NULL;
	struct cpuinfo_cache* l1i = NULL;
	struct cpuinfo_cache* l1d = NULL;
	struct cpuinfo_cache* l2 = NULL;
	struct cpuinfo_cache* l3 = NULL;
	uint32_t* linux_cpu_to_uarch_index_map = NULL;

	const uint32_t max_processors_count = cpuinfo_linux_get_max_processors_count();
	cpuinfo_log_debug("system maximum processors count: %"PRIu32, max_processors_count);

	const uint32_t max_possible_processors_count = 1 +
		cpuinfo_linux_get_max_possible_processor(max_processors_count);
	cpuinfo_log_debug("maximum possible processors count: %"PRIu32, max_possible_processors_count);
	const uint32_t max_present_processors_count = 1 +
		cpuinfo_linux_get_max_present_processor(max_processors_count);
	cpuinfo_log_debug("maximum present processors count: %"PRIu32, max_present_processors_count);

	uint32_t valid_processor_mask = 0;
	uint32_t loongarch_linux_processors_count = max_processors_count;
	if (max_present_processors_count != 0) {
		loongarch_linux_processors_count = min(loongarch_linux_processors_count, max_present_processors_count);
		valid_processor_mask = CPUINFO_LINUX_FLAG_PRESENT;
	}
	if (max_possible_processors_count != 0) {
		loongarch_linux_processors_count = min(loongarch_linux_processors_count, max_possible_processors_count);
		valid_processor_mask |= CPUINFO_LINUX_FLAG_POSSIBLE;
	}
	if ((max_present_processors_count | max_possible_processors_count) == 0) {
		cpuinfo_log_error("failed to parse both lists of possible and present processors");
		return;
	}

	loongarch_linux_processors = calloc(loongarch_linux_processors_count, sizeof(struct cpuinfo_loongarch_linux_processor));
	if (loongarch_linux_processors == NULL) {
		cpuinfo_log_error(
			"failed to allocate %zu bytes for descriptions of %"PRIu32" Loongarch logical processors",
			loongarch_linux_processors_count * sizeof(struct cpuinfo_loongarch_linux_processor),
			loongarch_linux_processors_count);
		return;
	}

	if (max_possible_processors_count) {
		cpuinfo_linux_detect_possible_processors(
			loongarch_linux_processors_count, &loongarch_linux_processors->flags,
			sizeof(struct cpuinfo_loongarch_linux_processor),
			CPUINFO_LINUX_FLAG_POSSIBLE);
	}

	if (max_present_processors_count) {
		cpuinfo_linux_detect_present_processors(
			loongarch_linux_processors_count, &loongarch_linux_processors->flags,
			sizeof(struct cpuinfo_loongarch_linux_processor),
			CPUINFO_LINUX_FLAG_PRESENT);
	}

	char proc_cpuinfo_hardware[CPUINFO_HARDWARE_VALUE_MAX] = { 0 };

	if (!cpuinfo_loongarch_linux_parse_proc_cpuinfo(
			proc_cpuinfo_hardware,
			loongarch_linux_processors_count,
			loongarch_linux_processors)) {
		cpuinfo_log_error("failed to parse processor information from /proc/cpuinfo");
		goto cleanup;
	}

	uint32_t valid_processors = 0;

	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		loongarch_linux_processors[i].system_processor_id = i;
		const bool present_and_possible = bitmask_all(
			loongarch_linux_processors[i].flags, valid_processor_mask);
		const bool reported_in_cpuinfo =
			(loongarch_linux_processors[i].flags & CPUINFO_LOONGARCH_LINUX_VALID_PROCESSOR) != 0;
		if (present_and_possible && reported_in_cpuinfo) {
			loongarch_linux_processors[i].flags |= CPUINFO_LINUX_FLAG_VALID;
			valid_processors += 1;
		} else if (present_and_possible) {
			/* /proc/cpuinfo lists online CPUs only; don't expose an offline CPU. */
			cpuinfo_log_info("processor %"PRIu32" is not listed in /proc/cpuinfo", i);
		} else if (reported_in_cpuinfo) {
			/* Processor reported in /proc/cpuinfo, but not in possible and/or present lists. */
			cpuinfo_log_warning("invalid processor %"PRIu32" reported in /proc/cpuinfo", i);
		}
	}
	if (valid_processors == 0) {
		cpuinfo_log_error("failed to discover any online processors");
		goto cleanup;
	}

	#if CPUINFO_ARCH_LOONGARCH64
		uint32_t isa_features = 0;
		cpuinfo_loongarch_linux_hwcap_from_getauxval(&isa_features);
		cpuinfo_loongarch64_linux_decode_isa_from_proc_cpuinfo(
			isa_features, &cpuinfo_isa);
	#endif

	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			if (cpuinfo_linux_get_processor_package_id(i, &loongarch_linux_processors[i].package_id)) {
				loongarch_linux_processors[i].package_id_valid = true;
				loongarch_linux_processors[i].flags |= CPUINFO_LINUX_FLAG_PACKAGE_ID;
			}
		}
	}

	/* Initialize topology group IDs */
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		loongarch_linux_processors[i].package_leader_id = i;
		loongarch_linux_processors[i].cluster_leader_id = i;
		loongarch_linux_processors[i].core_leader_id = i;
		loongarch_linux_processors[i].l3_leader_id = i;
	}

	/* Propagate topology group IDs among siblings */
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (!bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			continue;
		}

		if (loongarch_linux_processors[i].flags & CPUINFO_LINUX_FLAG_PACKAGE_ID) {
			cpuinfo_linux_detect_core_siblings(
				loongarch_linux_processors_count, i,
				(cpuinfo_siblings_callback) cluster_siblings_parser,
				loongarch_linux_processors);
		}
	}

	/* Rebuild package groups from physical_package_id when core_siblings_list is unavailable. */
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (!bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID) ||
			!loongarch_linux_processors[i].package_id_valid) {
			continue;
		}
		uint32_t package_leader_id = i;
		for (uint32_t sibling = 0; sibling < loongarch_linux_processors_count; sibling++) {
			if (bitmask_all(loongarch_linux_processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID) &&
				loongarch_linux_processors[sibling].package_id_valid &&
				loongarch_linux_processors[sibling].package_id == loongarch_linux_processors[i].package_id &&
				sibling < package_leader_id) {
				package_leader_id = sibling;
			}
		}
		for (uint32_t sibling = 0; sibling < loongarch_linux_processors_count; sibling++) {
			if (bitmask_all(loongarch_linux_processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID) &&
				loongarch_linux_processors[sibling].package_id_valid &&
				loongarch_linux_processors[sibling].package_id == loongarch_linux_processors[i].package_id) {
				loongarch_linux_processors[sibling].package_leader_id = package_leader_id;
				loongarch_linux_processors[sibling].flags |= CPUINFO_LINUX_FLAG_PACKAGE_CLUSTER;
			}
		}
	}

	/* Prefer the generic Linux cluster topology when the kernel exposes it. */
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			cpuinfo_linux_detect_cluster_cpus(
				loongarch_linux_processors_count, i,
				(cpuinfo_siblings_callback) cluster_cpus_parser,
				loongarch_linux_processors);
		}
	}
	/* Kernels without cluster_cpus_list retain the existing package-level fallback. */
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID) &&
			!loongarch_linux_processors[i].cluster_topology_valid) {
			loongarch_linux_processors[i].cluster_leader_id =
				loongarch_linux_processors[i].package_leader_id;
		}
	}

	/* Propagate all cluster IDs */
	uint32_t clustered_processors = 0;
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID | CPUINFO_LINUX_FLAG_PACKAGE_CLUSTER)) {
			clustered_processors += 1;

			const uint32_t package_leader_id = loongarch_linux_processors[i].package_leader_id;
			if (package_leader_id < i) {
				loongarch_linux_processors[i].package_leader_id = loongarch_linux_processors[package_leader_id].package_leader_id;
			}

			cpuinfo_log_debug("processor %"PRIu32" clustered with processor %"PRIu32" as inferred from system siblings lists",
				i, loongarch_linux_processors[i].package_leader_id);
		}
	}

	/* Prefer topology information from sysfs, which also describes offline CPUs. */
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (!bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			continue;
		}
		uint32_t core_id;
		if (cpuinfo_linux_get_processor_core_id(i, &core_id)) {
			loongarch_linux_processors[i].core_id = core_id;
			loongarch_linux_processors[i].core_id_valid = true;
		}
	}
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (!bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			continue;
		}
		if (!cpuinfo_linux_detect_thread_siblings(
			loongarch_linux_processors_count, i,
			(cpuinfo_siblings_callback) core_siblings_parser,
			loongarch_linux_processors)) {
			/* Some kernels expose this information only as core_cpus_list. */
			cpuinfo_linux_detect_core_cpus(
				loongarch_linux_processors_count, i,
				(cpuinfo_siblings_callback) core_siblings_parser,
				loongarch_linux_processors);
		}
	}

	/* Fall back to the core IDs from /proc/cpuinfo when sysfs topology is unavailable. */
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (!bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			continue;
		}
		for (uint32_t sibling = 0; sibling < i; sibling++) {
			if (bitmask_all(loongarch_linux_processors[sibling].flags, CPUINFO_LINUX_FLAG_VALID) &&
				(!loongarch_linux_processors[sibling].core_topology_valid ||
					!loongarch_linux_processors[i].core_topology_valid) &&
				loongarch_linux_processors[sibling].core_id_valid &&
				loongarch_linux_processors[i].core_id_valid &&
				loongarch_linux_processors[sibling].package_id_valid &&
				loongarch_linux_processors[i].package_id_valid &&
				loongarch_linux_processors[sibling].package_id == loongarch_linux_processors[i].package_id &&
				loongarch_linux_processors[sibling].core_id == loongarch_linux_processors[i].core_id) {
				loongarch_linux_processors[i].core_leader_id =
					loongarch_linux_processors[sibling].core_leader_id;
				break;
			}
		}
	}

	/* Count logical processors per core and physical cores per package and cluster. */
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			const uint32_t core_leader_id = loongarch_linux_processors[i].core_leader_id;
			const uint32_t cluster_leader_id = loongarch_linux_processors[i].cluster_leader_id;
			loongarch_linux_processors[core_leader_id].core_processor_count += 1;
			loongarch_linux_processors[cluster_leader_id].cluster_processor_count += 1;
			if (core_leader_id == i) {
				loongarch_linux_processors[loongarch_linux_processors[i].package_leader_id].package_core_count += 1;
				loongarch_linux_processors[cluster_leader_id].cluster_core_count += 1;
			}
		}
	}
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			const uint32_t core_leader_id = loongarch_linux_processors[i].core_leader_id;
			const uint32_t cluster_leader_id = loongarch_linux_processors[i].cluster_leader_id;
			const uint32_t package_leader_id = loongarch_linux_processors[i].package_leader_id;
			loongarch_linux_processors[i].core_processor_count =
				loongarch_linux_processors[core_leader_id].core_processor_count;
			loongarch_linux_processors[i].cluster_processor_count =
				loongarch_linux_processors[cluster_leader_id].cluster_processor_count;
			loongarch_linux_processors[i].cluster_core_count =
				loongarch_linux_processors[cluster_leader_id].cluster_core_count;
			loongarch_linux_processors[i].package_core_count =
				loongarch_linux_processors[package_leader_id].package_core_count;
		}
	}

	/* Discover last-level-cache domains from Linux cacheinfo when available. */
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (!bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			continue;
		}
		detect_l3_siblings(i, loongarch_linux_processors_count, loongarch_linux_processors);
	}
	/*
	 * Keep package siblings together when no cacheinfo can be recovered. This is
	 * only a topology ordering fallback: l3_id_valid deliberately remains false,
	 * so the hard-coded cache geometry is not exposed as a detected package-wide
	 * L3 cache below.
	 */
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID) &&
			!loongarch_linux_processors[i].l3_id_valid) {
			loongarch_linux_processors[i].l3_leader_id =
				loongarch_linux_processors[i].package_leader_id;
		}
	}

	cpuinfo_loongarch_linux_count_package_processors(loongarch_linux_processors_count, loongarch_linux_processors);

	/* Count logical processors and physical cores in each L3 domain. */
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (!bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			continue;
		}
		const uint32_t l3_leader_id = loongarch_linux_processors[i].l3_leader_id;
		loongarch_linux_processors[l3_leader_id].l3_processor_count += 1;
		if (loongarch_linux_processors[i].core_leader_id == i) {
			loongarch_linux_processors[l3_leader_id].l3_core_count += 1;
		}
	}
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			const uint32_t l3_leader_id = loongarch_linux_processors[i].l3_leader_id;
			loongarch_linux_processors[i].l3_processor_count =
				loongarch_linux_processors[l3_leader_id].l3_processor_count;
			loongarch_linux_processors[i].l3_core_count =
				loongarch_linux_processors[l3_leader_id].l3_core_count;
		}
	}

	/* Decode each logical processor only from its own parsed CPUCFG value. */
	cpuinfo_loongarch_linux_decode_processor_uarchs(
		loongarch_linux_processors_count, loongarch_linux_processors);


	qsort(loongarch_linux_processors, loongarch_linux_processors_count,
		sizeof(struct cpuinfo_loongarch_linux_processor), cmp_loongarch_linux_processor);


	uint32_t uarchs_count = 0;
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			bool found_uarch = false;
			for (uint32_t previous = 0; previous < i; previous++) {
				if (bitmask_all(loongarch_linux_processors[previous].flags, CPUINFO_LINUX_FLAG_VALID) &&
					loongarch_linux_processors[previous].uarch == loongarch_linux_processors[i].uarch) {
					loongarch_linux_processors[i].uarch_index =
						loongarch_linux_processors[previous].uarch_index;
					found_uarch = true;
					break;
				}
			}
			if (!found_uarch) {
				loongarch_linux_processors[i].uarch_index = uarchs_count;
				uarchs_count += 1;
			}
		}
	}

	uint32_t package_count = 0;
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (!bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			continue;
		}
		uint32_t package_index = package_count;
		for (uint32_t previous = 0; previous < i; previous++) {
			if (bitmask_all(loongarch_linux_processors[previous].flags, CPUINFO_LINUX_FLAG_VALID) &&
				loongarch_linux_processors[previous].package_leader_id ==
					loongarch_linux_processors[i].package_leader_id) {
				package_index = loongarch_linux_processors[previous].package_index;
				break;
			}
		}
		if (package_index == package_count) {
			package_count += 1;
		}
		loongarch_linux_processors[i].package_index = package_index;
	}

	uint32_t cluster_count = 0;
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (!bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			continue;
		}
		uint32_t cluster_index = cluster_count;
		for (uint32_t previous = 0; previous < i; previous++) {
			if (bitmask_all(loongarch_linux_processors[previous].flags, CPUINFO_LINUX_FLAG_VALID) &&
				loongarch_linux_processors[previous].cluster_leader_id ==
					loongarch_linux_processors[i].cluster_leader_id) {
				cluster_index = loongarch_linux_processors[previous].cluster_index;
				break;
			}
		}
		if (cluster_index == cluster_count) {
			cluster_count += 1;
		}
		loongarch_linux_processors[i].cluster_index = cluster_index;
	}

	/*
	 * Cache topology assumptions:
	 * - Level 1 instruction and data caches are private to each physical core.
	 * - Level 2 is private to each physical core on supported LoongArch uarchs.
	 * - Level 3 sharing follows Linux cacheinfo shared_cpu_list; package is only
	 *   used as a fallback when cacheinfo topology is unavailable.
	 */
	char package_name[CPUINFO_PACKAGE_NAME_MAX] = { 0 };
	if (proc_cpuinfo_hardware[0] != '\0') {
		strncpy(package_name, proc_cpuinfo_hardware, CPUINFO_PACKAGE_NAME_MAX - 1);
	} else {
		strncpy(package_name, "Unknown", CPUINFO_PACKAGE_NAME_MAX - 1);
	}

	uint32_t valid_cores = 0;
	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID) &&
			loongarch_linux_processors[i].core_leader_id ==
				loongarch_linux_processors[i].system_processor_id) {
			valid_cores += 1;
		}
	}
	packages = calloc(package_count, sizeof(struct cpuinfo_package));
	if (packages == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" packages",
			package_count * sizeof(struct cpuinfo_package), package_count);
		goto cleanup;
	}
	for (uint32_t i = 0; i < package_count; i++) {
		strncpy(packages[i].name, package_name, CPUINFO_PACKAGE_NAME_MAX - 1);
	}

	processors = calloc(valid_processors, sizeof(struct cpuinfo_processor));
	if (processors == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" logical processors",
			valid_processors * sizeof(struct cpuinfo_processor), valid_processors);
		goto cleanup;
	}

	cores = calloc(valid_cores, sizeof(struct cpuinfo_core));
	if (cores == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" cores",
			valid_cores * sizeof(struct cpuinfo_core), valid_cores);
		goto cleanup;
	}

	clusters = calloc(cluster_count, sizeof(struct cpuinfo_cluster));
	if (clusters == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" core clusters",
			cluster_count * sizeof(struct cpuinfo_cluster), cluster_count);
		goto cleanup;
	}

	uarchs = calloc(uarchs_count, sizeof(struct cpuinfo_uarch_info));
	if (uarchs == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" microarchitectures",
			uarchs_count * sizeof(struct cpuinfo_uarch_info), uarchs_count);
		goto cleanup;
	}

	linux_cpu_to_processor_map = calloc(loongarch_linux_processors_count, sizeof(struct cpuinfo_processor*));
	if (linux_cpu_to_processor_map == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for %"PRIu32" logical processor mapping entries",
			loongarch_linux_processors_count * sizeof(struct cpuinfo_processor*), loongarch_linux_processors_count);
		goto cleanup;
	}

	linux_cpu_to_core_map = calloc(loongarch_linux_processors_count, sizeof(struct cpuinfo_core*));
	if (linux_cpu_to_core_map == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for %"PRIu32" core mapping entries",
			loongarch_linux_processors_count * sizeof(struct cpuinfo_core*), loongarch_linux_processors_count);
		goto cleanup;
	}

	if (uarchs_count > 1) {
		linux_cpu_to_uarch_index_map = calloc(loongarch_linux_processors_count, sizeof(uint32_t));
		if (linux_cpu_to_uarch_index_map == NULL) {
			cpuinfo_log_error("failed to allocate %zu bytes for %"PRIu32" uarch index mapping entries",
				loongarch_linux_processors_count * sizeof(uint32_t), loongarch_linux_processors_count);
			goto cleanup;
		}
	}

	l1i = calloc(valid_cores, sizeof(struct cpuinfo_cache));
	if (l1i == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" L1I caches",
			valid_cores * sizeof(struct cpuinfo_cache), valid_cores);
		goto cleanup;
	}

	l1d = calloc(valid_cores, sizeof(struct cpuinfo_cache));
	if (l1d == NULL) {
		cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" L1D caches",
			valid_cores * sizeof(struct cpuinfo_cache), valid_cores);
		goto cleanup;
	}

	for (uint32_t i = 0; i < loongarch_linux_processors_count; i++) {
		if (bitmask_all(loongarch_linux_processors[i].flags, CPUINFO_LINUX_FLAG_VALID)) {
			const uint32_t uarch_index = loongarch_linux_processors[i].uarch_index;
			if (uarchs[uarch_index].processor_count == 0) {
				uarchs[uarch_index] = (struct cpuinfo_uarch_info) {
					.uarch = loongarch_linux_processors[i].uarch,
					.cpucfg = loongarch_linux_processors[i].cpucfg_id,
				};
			}
			uarchs[uarch_index].processor_count += 1;
			if (loongarch_linux_processors[i].core_leader_id ==
				loongarch_linux_processors[i].system_processor_id) {
				uarchs[uarch_index].core_count += 1;
			}
		}
	}


	uint32_t l2_count = 0, l3_count = 0;
	uint32_t core_index = 0;
	/* Populate cache information structures in l1i, l1d */
	for (uint32_t i = 0; i < valid_processors; i++) {
		struct cpuinfo_cache decoded_l1i, decoded_l1d, temp_l2, temp_l3;
		cpuinfo_loongarch_decode_cache(
			loongarch_linux_processors[i].uarch,
			loongarch_linux_processors[i].l3_core_count,
			&decoded_l1i, &decoded_l1d, &temp_l2, &temp_l3);
		apply_l3_geometry(&loongarch_linux_processors[i], &temp_l3);
		if (!loongarch_linux_processors[i].l3_id_valid) {
			temp_l3 = (struct cpuinfo_cache) { 0 };
		}
		const uint32_t package_index = loongarch_linux_processors[i].package_index;
		const uint32_t cluster_index = loongarch_linux_processors[i].cluster_index;
		if (loongarch_linux_processors[i].cluster_leader_id == loongarch_linux_processors[i].system_processor_id) {
			enum cpuinfo_vendor cluster_vendor = loongarch_linux_processors[i].vendor;
			enum cpuinfo_uarch cluster_uarch = loongarch_linux_processors[i].uarch;
			uint32_t cluster_cpucfg = loongarch_linux_processors[i].cpucfg_id;
			const uint32_t cluster_end = i + loongarch_linux_processors[i].cluster_processor_count;
			for (uint32_t sibling = i + 1; sibling < cluster_end; sibling++) {
				if (loongarch_linux_processors[sibling].core_leader_id !=
						loongarch_linux_processors[sibling].system_processor_id) {
					continue;
				}
				if (loongarch_linux_processors[sibling].vendor != cluster_vendor ||
					loongarch_linux_processors[sibling].uarch != cluster_uarch ||
					loongarch_linux_processors[sibling].cpucfg_id != cluster_cpucfg) {
					cluster_vendor = cpuinfo_vendor_unknown;
					cluster_uarch = cpuinfo_uarch_unknown;
					cluster_cpucfg = 0;
					break;
				}
			}
			uint32_t cluster_id = 0;
			for (uint32_t previous = 0; previous < i; previous++) {
				if (loongarch_linux_processors[previous].package_index == package_index &&
					loongarch_linux_processors[previous].cluster_leader_id ==
						loongarch_linux_processors[previous].system_processor_id) {
					cluster_id += 1;
				}
			}
			clusters[cluster_index] = (struct cpuinfo_cluster) {
				.processor_start = i,
				.processor_count = loongarch_linux_processors[i].cluster_processor_count,
				.core_start = core_index,
				.core_count = loongarch_linux_processors[i].cluster_core_count,
				.cluster_id = cluster_id,
				.package = &packages[package_index],
				.vendor = cluster_vendor,
				.uarch = cluster_uarch,
				.cpucfg = cluster_cpucfg,
			};
		}

		uint32_t smt_id = 0;
		for (uint32_t previous = 0; previous < i; previous++) {
			if (loongarch_linux_processors[previous].core_leader_id ==
				loongarch_linux_processors[i].core_leader_id &&
				bitmask_all(loongarch_linux_processors[previous].flags, CPUINFO_LINUX_FLAG_VALID)) {
				smt_id += 1;
			}
		}
		processors[i].smt_id = smt_id;
		processors[i].cpucfg_id = loongarch_linux_processors[i].cpucfg_id;
		processors[i].cluster = clusters + cluster_index;
		processors[i].package = &packages[package_index];
		processors[i].linux_id = (int) loongarch_linux_processors[i].system_processor_id;
		linux_cpu_to_processor_map[loongarch_linux_processors[i].system_processor_id] = &processors[i];

		if (loongarch_linux_processors[i].core_leader_id == loongarch_linux_processors[i].system_processor_id) {
			uint32_t package_core_id = 0;
			for (uint32_t previous = 0; previous < i; previous++) {
				if (bitmask_all(loongarch_linux_processors[previous].flags, CPUINFO_LINUX_FLAG_VALID) &&
					loongarch_linux_processors[previous].package_index == package_index &&
					loongarch_linux_processors[previous].core_leader_id ==
						loongarch_linux_processors[previous].system_processor_id) {
					package_core_id += 1;
				}
			}
			processors[i].core = cores + core_index;
			cores[core_index].processor_start = i;
			cores[core_index].processor_count = loongarch_linux_processors[i].core_processor_count;
			cores[core_index].core_id = package_core_id;
			cores[core_index].cluster = clusters + cluster_index;
			cores[core_index].package = &packages[package_index];
			cores[core_index].vendor = loongarch_linux_processors[i].vendor;
			cores[core_index].uarch = loongarch_linux_processors[i].uarch;
			cores[core_index].cpucfg = loongarch_linux_processors[i].cpucfg_id;
			l1i[core_index] = decoded_l1i;
			l1d[core_index] = decoded_l1d;
			l1i[core_index].processor_start = l1d[core_index].processor_start = i;
			l1i[core_index].processor_count = l1d[core_index].processor_count =
				loongarch_linux_processors[i].core_processor_count;
			core_index += 1;
		} else {
			processors[i].core = linux_cpu_to_core_map[
				loongarch_linux_processors[i].core_leader_id];
		}
		const uint32_t processor_core_index = (uint32_t)(processors[i].core - cores);
		processors[i].cache.l1i = l1i + processor_core_index;
		processors[i].cache.l1d = l1d + processor_core_index;
		linux_cpu_to_core_map[loongarch_linux_processors[i].system_processor_id] =
			processors[i].core;

		if (linux_cpu_to_uarch_index_map != NULL) {
			linux_cpu_to_uarch_index_map[loongarch_linux_processors[i].system_processor_id] =
				loongarch_linux_processors[i].uarch_index;
		}

		if (loongarch_linux_processors[i].core_leader_id == loongarch_linux_processors[i].system_processor_id &&
			temp_l2.size != 0) {
			l2_count += 1;
		}
		if (loongarch_linux_processors[i].l3_leader_id == loongarch_linux_processors[i].system_processor_id &&
			temp_l3.size != 0) {
			l3_count += 1;
		}
	}

	if (l2_count != 0) {
		l2 = calloc(l2_count, sizeof(struct cpuinfo_cache));
		if (l2 == NULL) {
			cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" L2 caches",
				l2_count * sizeof(struct cpuinfo_cache), l2_count);
			goto cleanup;
		}

	}
	if (l3_count != 0) {
		l3 = calloc(l3_count, sizeof(struct cpuinfo_cache));
		if (l3 == NULL) {
			cpuinfo_log_error("failed to allocate %zu bytes for descriptions of %"PRIu32" L3 caches",
				l3_count * sizeof(struct cpuinfo_cache), l3_count);
			goto cleanup;
		}
	}

	uint32_t l2_index = UINT32_MAX, l3_index = UINT32_MAX;
	for (uint32_t i = 0; i < valid_processors; i++) {
		struct cpuinfo_cache dummy_l1i, dummy_l1d, temp_l2 = { 0 }, temp_l3 = { 0 };
		cpuinfo_loongarch_decode_cache(
			loongarch_linux_processors[i].uarch,
			loongarch_linux_processors[i].l3_core_count,
			&dummy_l1i, &dummy_l1d, &temp_l2, &temp_l3);
		apply_l3_geometry(&loongarch_linux_processors[i], &temp_l3);
		if (!loongarch_linux_processors[i].l3_id_valid) {
			temp_l3 = (struct cpuinfo_cache) { 0 };
		}
		if (loongarch_linux_processors[i].core_leader_id == loongarch_linux_processors[i].system_processor_id) {
			if (temp_l2.size != 0) {
				l2_index += 1;
				l2[l2_index] = (struct cpuinfo_cache) {
					.size = temp_l2.size,
					.associativity = temp_l2.associativity,
					.sets = temp_l2.sets,
					.partitions = 1,
					.line_size = temp_l2.line_size,
					.processor_start = i,
					.processor_count = loongarch_linux_processors[i].core_processor_count,
			};
				processors[i].cache.l2 = l2 + l2_index;
			}
		}
		if (loongarch_linux_processors[i].l3_leader_id == loongarch_linux_processors[i].system_processor_id &&
			temp_l3.size != 0 && l3 != NULL) {
			l3_index += 1;
			l3[l3_index] = (struct cpuinfo_cache) {
				.size = temp_l3.size,
				.associativity = temp_l3.associativity,
				.sets = temp_l3.sets,
				.partitions = temp_l3.partitions,
				.line_size = temp_l3.line_size,
				.processor_start = i,
				.processor_count = loongarch_linux_processors[i].l3_processor_count,
			};
			processors[i].cache.l3 = l3 + l3_index;
		}
		if (loongarch_linux_processors[i].core_leader_id != loongarch_linux_processors[i].system_processor_id) {
			processors[i].cache.l2 = linux_cpu_to_processor_map[
				loongarch_linux_processors[i].core_leader_id]->cache.l2;
		}
		if (loongarch_linux_processors[i].l3_leader_id != loongarch_linux_processors[i].system_processor_id) {
			processors[i].cache.l3 = linux_cpu_to_processor_map[
				loongarch_linux_processors[i].l3_leader_id]->cache.l3;
		}
	}

	for (uint32_t i = 0; i < valid_processors; i++) {
		const uint32_t package_index = loongarch_linux_processors[i].package_index;
		struct cpuinfo_package* package = &packages[package_index];
		if (package->processor_count == 0) {
			package->processor_start = i;
		}
		package->processor_count += 1;
		if (loongarch_linux_processors[i].core_leader_id == loongarch_linux_processors[i].system_processor_id) {
			if (package->core_count == 0) {
				package->core_start = (uint32_t)(processors[i].core - cores);
			}
			package->core_count += 1;
		}
		if (loongarch_linux_processors[i].cluster_leader_id == loongarch_linux_processors[i].system_processor_id) {
			if (package->cluster_count == 0) {
				package->cluster_start = (uint32_t)(processors[i].cluster - clusters);
			}
			package->cluster_count += 1;
		}
	}

	/* Commit */
	cpuinfo_processors = processors;
	cpuinfo_cores = cores;
	cpuinfo_clusters = clusters;
	cpuinfo_packages = packages;
	cpuinfo_uarchs = uarchs;
	cpuinfo_cache[cpuinfo_cache_level_1i] = l1i;
	cpuinfo_cache[cpuinfo_cache_level_1d] = l1d;
	cpuinfo_cache[cpuinfo_cache_level_2]  = l2;
	cpuinfo_cache[cpuinfo_cache_level_3]  = l3;

	cpuinfo_processors_count = valid_processors;
	cpuinfo_cores_count = valid_cores;
	cpuinfo_clusters_count = cluster_count;
	cpuinfo_packages_count = package_count;
	cpuinfo_uarchs_count = uarchs_count;
	cpuinfo_cache_count[cpuinfo_cache_level_1i] = valid_cores;
	cpuinfo_cache_count[cpuinfo_cache_level_1d] = valid_cores;
	cpuinfo_cache_count[cpuinfo_cache_level_2]  = l2_count;
	cpuinfo_cache_count[cpuinfo_cache_level_3]  = l3_count;
	cpuinfo_max_cache_size = 0;
	for (uint32_t i = 0; i < cpuinfo_processors_count; i++) {
		const struct cpuinfo_cache* cache = processors[i].cache.l3;
		if (cache == NULL) {
			cache = processors[i].cache.l2;
		}
		if (cache != NULL && cache->size > cpuinfo_max_cache_size) {
			cpuinfo_max_cache_size = cache->size;
		}
	}
	if (cpuinfo_max_cache_size == 0) {
		cpuinfo_max_cache_size = cpuinfo_loongarch_compute_max_cache_size(&processors[0]);
	}

	cpuinfo_linux_cpu_max = loongarch_linux_processors_count;
	cpuinfo_linux_cpu_to_processor_map = linux_cpu_to_processor_map;
	cpuinfo_linux_cpu_to_core_map = linux_cpu_to_core_map;
	cpuinfo_linux_cpu_to_uarch_index_map = linux_cpu_to_uarch_index_map;

	__sync_synchronize();
	cpuinfo_is_initialized = true;

	processors = NULL;
	cores = NULL;
	clusters = NULL;
	packages = NULL;
	uarchs = NULL;
	l1i = l1d = l2 = l3 = NULL;
	linux_cpu_to_processor_map = NULL;
	linux_cpu_to_core_map = NULL;
	linux_cpu_to_uarch_index_map = NULL;

cleanup:
	free(loongarch_linux_processors);
	free(processors);
	free(cores);
	free(clusters);
	free(packages);
	free(uarchs);
	free(l1i);
	free(l1d);
	free(l2);
	free(l3);
	free(linux_cpu_to_processor_map);
	free(linux_cpu_to_core_map);
	free(linux_cpu_to_uarch_index_map);
}

void cpuinfo_loongarch_linux_deinit(void) {
	free(cpuinfo_processors);
	cpuinfo_processors = NULL;
	cpuinfo_processors_count = 0;

	free(cpuinfo_cores);
	cpuinfo_cores = NULL;
	cpuinfo_cores_count = 0;

	free(cpuinfo_clusters);
	cpuinfo_clusters = NULL;
	cpuinfo_clusters_count = 0;

	free(cpuinfo_packages);
	cpuinfo_packages = NULL;
	cpuinfo_packages_count = 0;

	free(cpuinfo_uarchs);
	cpuinfo_uarchs = NULL;
	cpuinfo_uarchs_count = 0;

	for (int level = 0; level < cpuinfo_cache_level_max; level++) {
		free(cpuinfo_cache[level]);
		cpuinfo_cache[level] = NULL;
		cpuinfo_cache_count[level] = 0;
	}
	cpuinfo_max_cache_size = 0;

	free(cpuinfo_linux_cpu_to_processor_map);
	cpuinfo_linux_cpu_to_processor_map = NULL;

	free(cpuinfo_linux_cpu_to_core_map);
	cpuinfo_linux_cpu_to_core_map = NULL;

	free((void*) cpuinfo_linux_cpu_to_uarch_index_map);
	cpuinfo_linux_cpu_to_uarch_index_map = NULL;

	cpuinfo_linux_cpu_max = 0;
}
