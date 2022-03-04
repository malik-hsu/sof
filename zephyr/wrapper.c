// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright(c) 2020 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#include <sof/init.h>
#include <sof/lib/alloc.h>
#include <sof/drivers/idc.h>
#include <sof/drivers/interrupt.h>
#include <sof/drivers/interrupt-map.h>
#include <sof/lib/dma.h>
#include <sof/schedule/schedule.h>
#include <platform/drivers/interrupt.h>
#include <platform/lib/memory.h>
#include <sof/platform.h>
#include <sof/lib/notifier.h>
#include <sof/lib/pm_runtime.h>
#include <sof/audio/pipeline.h>
#include <sof/audio/component_ext.h>
#include <sof/trace/trace.h>

/* Zephyr includes */
#include <device.h>
#include <soc.h>
#include <kernel.h>
#include <version.h>

#if !CONFIG_KERNEL_COHERENCE
#include <arch/xtensa/cache.h>
#endif

extern K_KERNEL_STACK_ARRAY_DEFINE(z_interrupt_stacks, CONFIG_MP_NUM_CPUS,
				   CONFIG_ISR_STACK_SIZE);

/* 300aaad4-45d2-8313-25d0-5e1d6086cdd1 */
DECLARE_SOF_RT_UUID("zephyr", zephyr_uuid, 0x300aaad4, 0x45d2, 0x8313,
		 0x25, 0xd0, 0x5e, 0x1d, 0x60, 0x86, 0xcd, 0xd1);

DECLARE_TR_CTX(zephyr_tr, SOF_UUID(zephyr_uuid), LOG_LEVEL_INFO);

/*
 * Memory - Create Zephyr HEAP for SOF.
 *
 * Currently functional but some items still WIP.
 */

#ifndef HEAP_RUNTIME_SIZE
#define HEAP_RUNTIME_SIZE	0
#endif

/* system size not declared on some platforms */
#ifndef HEAP_SYSTEM_SIZE
#define HEAP_SYSTEM_SIZE	0
#endif

/* The Zephyr heap */

#ifdef CONFIG_IMX
#define HEAPMEM_SIZE		(HEAP_SYSTEM_SIZE + HEAP_RUNTIME_SIZE + HEAP_BUFFER_SIZE)

/*
 * Include heapmem variable in .heap_mem section, otherwise the HEAPMEM_SIZE is
 * duplicated in two sections and the sdram0 region overflows.
 */
__section(".heap_mem") static uint8_t __aligned(64) heapmem[HEAPMEM_SIZE];

#else

extern uint8_t _end, _heap_sentry;
#define heapmem ((uint8_t *)ALIGN_UP((uintptr_t)&_end, PLATFORM_DCACHE_ALIGN))
#define HEAPMEM_SIZE (&_heap_sentry - heapmem)

#endif

static struct k_heap sof_heap;

static int statics_init(const struct device *unused)
{
	ARG_UNUSED(unused);

	sys_heap_init(&sof_heap.heap, heapmem, HEAPMEM_SIZE);

	return 0;
}

SYS_INIT(statics_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_OBJECTS);

static void *heap_alloc_aligned(struct k_heap *h, size_t min_align, size_t bytes)
{
	k_spinlock_key_t key;
	void *ret;

	key = k_spin_lock(&h->lock);
	ret = sys_heap_aligned_alloc(&h->heap, min_align, bytes);
	k_spin_unlock(&h->lock, key);

	return ret;
}

static void *heap_alloc_aligned_cached(struct k_heap *h, size_t min_align, size_t bytes)
{
	void *ptr;

	/*
	 * Zephyr sys_heap stores metadata at start of each
	 * heap allocation. To ensure no allocated cached buffer
	 * overlaps the same cacheline with the metadata chunk,
	 * align both allocation start and size of allocation
	 * to cacheline. As cached and non-cached allocations are
	 * mixed, same rules need to be followed for both type of
	 * allocations.
	 */
#ifdef CONFIG_SOF_ZEPHYR_HEAP_CACHED
	min_align = MAX(PLATFORM_DCACHE_ALIGN, min_align);
	bytes = ALIGN_UP(bytes, min_align);
#endif

	ptr = heap_alloc_aligned(h, min_align, bytes);

#ifdef CONFIG_SOF_ZEPHYR_HEAP_CACHED
	if (ptr)
		ptr = z_soc_cached_ptr(ptr);
#endif

	return ptr;
}

static void heap_free(struct k_heap *h, void *mem)
{
	k_spinlock_key_t key = k_spin_lock(&h->lock);
#ifdef CONFIG_SOF_ZEPHYR_HEAP_CACHED
	void *mem_uncached;

	if (is_cached(mem)) {
		mem_uncached = z_soc_uncached_ptr(mem);
		z_xtensa_cache_flush_inv(mem, sys_heap_usable_size(&h->heap, mem_uncached));

		mem = mem_uncached;
	}
#endif

	sys_heap_free(&h->heap, mem);

	k_spin_unlock(&h->lock, key);
}

static inline bool zone_is_cached(enum mem_zone zone)
{
#ifdef CONFIG_SOF_ZEPHYR_HEAP_CACHED
	switch (zone) {
	case SOF_MEM_ZONE_SYS:
	case SOF_MEM_ZONE_SYS_RUNTIME:
	case SOF_MEM_ZONE_RUNTIME:
	case SOF_MEM_ZONE_BUFFER:
		return true;
	default:
		break;
	}
#endif

	return false;
}

void *rmalloc(enum mem_zone zone, uint32_t flags, uint32_t caps, size_t bytes)
{
	void *ptr;

	if (zone_is_cached(zone)) {
		ptr = heap_alloc_aligned_cached(&sof_heap, 0, bytes);
	} else {
		/*
		 * XTOS alloc implementation has used dcache alignment,
		 * so SOF application code is expecting this behaviour.
		 */
		ptr = heap_alloc_aligned(&sof_heap, PLATFORM_DCACHE_ALIGN, bytes);
	}

	if (!ptr && zone == SOF_MEM_ZONE_SYS)
		k_panic();

	return ptr;
}

/* Use SOF_MEM_ZONE_BUFFER at the moment */
void *rbrealloc_align(void *ptr, uint32_t flags, uint32_t caps, size_t bytes,
		      size_t old_bytes, uint32_t alignment)
{
	void *new_ptr;

	if (!ptr) {
		/* TODO: Use correct zone */
		return rballoc_align(flags, caps, bytes, alignment);
	}

	/* Original version returns NULL without freeing this memory */
	if (!bytes) {
		/* TODO: Should we call rfree(ptr); */
		tr_err(&zephyr_tr, "realloc failed for 0 bytes");
		return NULL;
	}

	new_ptr = rballoc_align(flags, caps, bytes, alignment);
	if (!new_ptr) {
		return NULL;
	}

	if (!(flags & SOF_MEM_FLAG_NO_COPY)) {
		memcpy(new_ptr, ptr, MIN(bytes, old_bytes));
	}

	rfree(ptr);

	tr_info(&zephyr_tr, "rbealloc: new ptr %p", new_ptr);

	return new_ptr;
}

/**
 * Similar to rmalloc(), guarantees that returned block is zeroed.
 *
 * @note Do not use  for buffers (SOF_MEM_ZONE_BUFFER zone).
 *       rballoc(), rballoc_align() to allocate memory for buffers.
 */
void *rzalloc(enum mem_zone zone, uint32_t flags, uint32_t caps, size_t bytes)
{
	void *ptr = rmalloc(zone, flags, caps, bytes);

	memset(ptr, 0, bytes);

	return ptr;
}

/**
 * Allocates memory block from SOF_MEM_ZONE_BUFFER.
 * @param flags Flags, see SOF_MEM_FLAG_...
 * @param caps Capabilities, see SOF_MEM_CAPS_...
 * @param bytes Size in bytes.
 * @param alignment Alignment in bytes.
 * @return Pointer to the allocated memory or NULL if failed.
 */
void *rballoc_align(uint32_t flags, uint32_t caps, size_t bytes,
		    uint32_t alignment)
{
	return heap_alloc_aligned_cached(&sof_heap, alignment, bytes);
}

/*
 * Free's memory allocated by above alloc calls.
 */
void rfree(void *ptr)
{
	if (!ptr)
		return;

	heap_free(&sof_heap, ptr);
}

/* debug only - only needed for linking */
void heap_trace_all(int force)
{
}

/*
 * Interrupts.
 *
 * Mostly mapped. Still needs some linkage symbols that can be removed later.
 */

/* needed for linkage only */
const char irq_name_level2[] = "level2";
const char irq_name_level5[] = "level5";

int interrupt_register(uint32_t irq, void(*handler)(void *arg), void *arg)
{
#ifdef CONFIG_DYNAMIC_INTERRUPTS
	return arch_irq_connect_dynamic(irq, 0, (void (*)(const void *))handler,
					arg, 0);
#else
	tr_err(&zephyr_tr, "Cannot register handler for IRQ %u: dynamic IRQs are disabled",
		irq);
	return -EOPNOTSUPP;
#endif
}

#if !CONFIG_LIBRARY
/* unregister an IRQ handler - matches on IRQ number and data ptr */
void interrupt_unregister(uint32_t irq, const void *arg)
{
	/*
	 * There is no "unregister" (or "disconnect") for
	 * interrupts in Zephyr.
	 */
	z_soc_irq_disable(irq);
}

/* enable an interrupt source - IRQ needs mapped to Zephyr,
 * arg is used to match.
 */
uint32_t interrupt_enable(uint32_t irq, void *arg)
{
	z_soc_irq_enable(irq);

	return 0;
}

/* disable interrupt */
uint32_t interrupt_disable(uint32_t irq, void *arg)
{
	z_soc_irq_disable(irq);

	return 0;
}
#endif

/*
 * i.MX uses the IRQ_STEER
 */
#if !CONFIG_IMX
/*
 * CAVS IRQs are multilevel whereas BYT and BDW are DSP level only.
 */
int interrupt_get_irq(unsigned int irq, const char *cascade)
{
#if CONFIG_SOC_SERIES_INTEL_ADSP_BAYTRAIL ||\
	CONFIG_SOC_SERIES_INTEL_ADSP_BROADWELL || \
	CONFIG_LIBRARY
	return irq;
#else
	if (cascade == irq_name_level2)
		return SOC_AGGREGATE_IRQ(irq, IRQ_NUM_EXT_LEVEL2);
	if (cascade == irq_name_level5)
		return SOC_AGGREGATE_IRQ(irq, IRQ_NUM_EXT_LEVEL5);

	return SOC_AGGREGATE_IRQ(0, irq);
#endif
}

void interrupt_mask(uint32_t irq, unsigned int cpu)
{
	/* TODO: how do we mask on other cores with Zephyr APIs */
}

void interrupt_unmask(uint32_t irq, unsigned int cpu)
{
	/* TODO: how do we unmask on other cores with Zephyr APIs */
}

void platform_interrupt_init(void)
{
	/* handled by zephyr - needed for linkage */
}

void platform_interrupt_set(uint32_t irq)
{
	/* handled by zephyr - needed for linkage */
}

void platform_interrupt_clear(uint32_t irq, uint32_t mask)
{
	/* handled by zephyr - needed for linkage */
}
#endif

/*
 * Timers.
 *
 * Mostly mapped. TODO: align with 64bit Zephyr timers when they are upstream.
 */

#if !CONFIG_LIBRARY
uint64_t arch_timer_get_system(struct timer *timer)
{
	return platform_timer_get(timer);
}
#endif

uint64_t platform_timer_get(struct timer *timer)
{
#if CONFIG_SOC_SERIES_INTEL_ADSP_BAYTRAIL
	uint32_t low;
	uint32_t high;
	uint64_t time;

	do {
		/* TODO: check and see whether 32bit IRQ is pending for timer */
		high = timer->hitime;
		/* read low 32 bits */
		low = shim_read(SHIM_EXT_TIMER_STAT);
	} while (high != timer->hitime);

	time = ((uint64_t)high << 32) | low;

	return time;
#elif CONFIG_SOC_SERIES_INTEL_ADSP_BROADWELL || CONFIG_LIBRARY
	// FIXME!
	return 0;
#elif CONFIG_IMX
	/* For i.MX use Xtensa timer, as we do now with SOF */
	uint64_t time = 0;
	uint32_t low;
	uint32_t high;
	uint32_t ccompare;

	if (!timer || timer->id >= ARCH_TIMER_COUNT)
		goto out;

	ccompare = xthal_get_ccompare(timer->id);

	/* read low 32 bits */
	low = xthal_get_ccount();

	/* check and see whether 32bit IRQ is pending for timer */
	if (arch_interrupt_get_status() & (1 << timer->irq) && ccompare == 1) {
		/* yes, overflow has occurred but handler has not run */
		high = timer->hitime + 1;
	} else {
		/* no overflow */
		high = timer->hitime;
	}

	time = ((uint64_t)high << 32) | low;

out:

	return time;
#elif CONFIG_SOF_ZEPHYR
	return k_cycle_get_64();
#else
	/* CAVS versions */
	return shim_read64(SHIM_DSPWC);
#endif
}

void platform_timer_stop(struct timer *timer)
{
}

uint64_t platform_timer_get_atomic(struct timer *timer)
{
	uint32_t flags;
	uint64_t ticks_now;

	irq_local_disable(flags);
	ticks_now = platform_timer_get(timer);
	irq_local_enable(flags);

	return ticks_now;
}

/*
 * Notifier.
 *
 * Use SOF inter component messaging today. Zephyr has similar APIs that will
 * need some minor feature updates prior to merge. i.e. FW to host messages.
 * TODO: align with Zephyr API when ready.
 */

static struct notify *host_notify[CONFIG_CORE_COUNT];

struct notify **arch_notify_get(void)
{
	return host_notify + cpu_get_id();
}

/*
 * Debug
 */
void arch_dump_regs_a(void *dump_buf)
{
	/* needed for linkage only */
}

/*
 * Xtensa. TODO: this needs removed and fixed in SOF.
 */
unsigned int _xtos_ints_off(unsigned int mask)
{
	/* turn all local IRQs OFF */
	irq_lock();
	return 0;
}

void ipc_send_queued_msg(void);

static void ipc_send_queued_callback(void *private_data, enum notify_id event_type,
				     void *caller_data)
{
	if (!ipc_get()->pm_prepare_D3)
		ipc_send_queued_msg();
}

/*
 * Audio components.
 *
 * Integrated except for linkage so symbols are "used" here until linker
 * support is ready in Zephyr. TODO: fix component linkage in Zephyr.
 */

/* TODO: this is not yet working with Zephyr - section has been created but
 *  no symbols are being loaded into ELF file.
 */
extern intptr_t _module_init_start;
extern intptr_t _module_init_end;

static void sys_module_init(void)
{
#if !CONFIG_LIBRARY
	intptr_t *module_init = (intptr_t *)(&_module_init_start);

	for (; module_init < (intptr_t *)&_module_init_end; ++module_init)
		((void(*)(void))(*module_init))();
#endif
}

/*
 * TODO: all the audio processing components/modules constructor should be
 * linked to the module_init section, but this is not happening. Just call
 * constructors directly atm.
 */

void sys_comp_volume_init(void);
void sys_comp_host_init(void);
void sys_comp_mixer_init(void);
void sys_comp_dai_init(void);
void sys_comp_src_init(void);
void sys_comp_mux_init(void);
void sys_comp_selector_init(void);
void sys_comp_switch_init(void);
void sys_comp_tone_init(void);
void sys_comp_eq_fir_init(void);
void sys_comp_keyword_init(void);
void sys_comp_asrc_init(void);
void sys_comp_dcblock_init(void);
void sys_comp_eq_iir_init(void);
void sys_comp_kpb_init(void);
void sys_comp_smart_amp_init(void);
void sys_comp_basefw_init(void);
void sys_comp_copier_init(void);
void sys_comp_codec_cadence_interface_init(void);
void sys_comp_codec_passthrough_interface_init(void);
void sys_comp_gain_init(void);
void sys_comp_mixin_init(void);

/* Zephyr redefines log_message() and mtrace_printf() which leaves
 * totally empty the .static_log_entries ELF sections for the
 * sof-logger. This makes smex fail. Define at least one such section to
 * fix the build when sof-logger is not used.
 */
static inline const void *smex_placeholder_f(void)
{
	_DECLARE_LOG_ENTRY(LOG_LEVEL_DEBUG,
			   "placeholder so .static_log.X are not all empty",
			   _TRACE_INV_CLASS, 0);

	return &log_entry;
}

/* Need to actually use the function and export something otherwise the
 * compiler optimizes everything away.
 */
const void *_smex_placeholder;

int task_main_start(struct sof *sof)
{
	_smex_placeholder = smex_placeholder_f();

	int ret;

	/* init default audio components */
	sys_comp_init(sof);

	/* init self-registered modules */
	sys_module_init();

	/* host is mandatory */
	sys_comp_host_init();

	if (IS_ENABLED(CONFIG_COMP_VOLUME)) {
		sys_comp_volume_init();

		if (IS_ENABLED(CONFIG_IPC_MAJOR_4))
			sys_comp_gain_init();
	}

	if (IS_ENABLED(CONFIG_COMP_MIXER)) {
		sys_comp_mixer_init();

		if (IS_ENABLED(CONFIG_IPC_MAJOR_4))
			sys_comp_mixin_init();
	}

	if (IS_ENABLED(CONFIG_COMP_DAI)) {
		sys_comp_dai_init();
	}

	if (IS_ENABLED(CONFIG_COMP_SRC)) {
		sys_comp_src_init();
	}

	if (IS_ENABLED(CONFIG_COMP_SEL)) {
		sys_comp_selector_init();
	}

	if (IS_ENABLED(CONFIG_COMP_SWITCH)) {
		sys_comp_switch_init();
	}

	if (IS_ENABLED(CONFIG_COMP_TONE)) {
		sys_comp_tone_init();
	}

	if (IS_ENABLED(CONFIG_COMP_FIR)) {
		sys_comp_eq_fir_init();
	}

	if (IS_ENABLED(CONFIG_COMP_IIR)) {
		sys_comp_eq_iir_init();
	}

	if (IS_ENABLED(CONFIG_SAMPLE_KEYPHRASE)) {
		sys_comp_keyword_init();
	}

	if (IS_ENABLED(CONFIG_COMP_KPB)) {
		sys_comp_kpb_init();
	}

	if (IS_ENABLED(CONFIG_SAMPLE_SMART_AMP)) {
		sys_comp_smart_amp_init();
	}

	if (IS_ENABLED(CONFIG_COMP_ASRC)) {
		sys_comp_asrc_init();
	}

	if (IS_ENABLED(CONFIG_COMP_DCBLOCK)) {
		sys_comp_dcblock_init();
	}

	if (IS_ENABLED(CONFIG_COMP_MUX)) {
		sys_comp_mux_init();
	}

	if (IS_ENABLED(CONFIG_COMP_BASEFW_IPC4)) {
		sys_comp_basefw_init();
	}

	if (IS_ENABLED(CONFIG_COMP_COPIER)) {
		sys_comp_copier_init();
	}

	if (IS_ENABLED(CONFIG_CADENCE_CODEC)) {
		sys_comp_codec_cadence_interface_init();
	}

	if (IS_ENABLED(CONFIG_PASSTHROUGH_CODEC)) {
		sys_comp_codec_passthrough_interface_init();
	}

	/* init pipeline position offsets */
	pipeline_posn_init(sof);

#if defined(CONFIG_IMX)
#define SOF_IPC_QUEUED_DOMAIN SOF_SCHEDULE_LL_DMA
#else
#define SOF_IPC_QUEUED_DOMAIN SOF_SCHEDULE_LL_TIMER
#endif

	/* Temporary fix for issue #4356 */
	(void)notifier_register(NULL, scheduler_get_data(SOF_IPC_QUEUED_DOMAIN),
				NOTIFIER_ID_LL_POST_RUN,
				ipc_send_queued_callback, 0);

	/* let host know DSP boot is complete */
	ret = platform_boot_complete(0);

	return ret;
}

/*
 * Timestamps.
 *
 * TODO: move to generic code in SOF, currently platform code.
 */

/* get timestamp for host stream DMA position */
void platform_host_timestamp(struct comp_dev *host,
			     struct sof_ipc_stream_posn *posn)
{
	int err;

	/* get host position */
	err = comp_position(host, posn);
	if (err == 0)
		posn->flags |= SOF_TIME_HOST_VALID;
}

/* get timestamp for DAI stream DMA position */
void platform_dai_timestamp(struct comp_dev *dai,
			    struct sof_ipc_stream_posn *posn)
{
	int err;

	/* get DAI position */
	err = comp_position(dai, posn);
	if (err == 0)
		posn->flags |= SOF_TIME_DAI_VALID;

	/* get SSP wallclock - DAI sets this to stream start value */
	posn->wallclock = platform_timer_get(NULL) - posn->wallclock;
	posn->wallclock_hz = clock_get_freq(PLATFORM_DEFAULT_CLOCK);
	posn->flags |= SOF_TIME_WALL_VALID;
}

/* get current wallclock for componnent */
void platform_dai_wallclock(struct comp_dev *dai, uint64_t *wallclock)
{
	*wallclock = platform_timer_get(NULL);
}

/*
 * Multicore
 *
 * Mostly empty today waiting pending Zephyr CAVS SMP integration.
 */
#if CONFIG_MULTICORE && CONFIG_SMP
static atomic_t start_flag;
static atomic_t ready_flag;

/* Zephyr kernel_internal.h interface */
void smp_timer_init(void);

static FUNC_NORETURN void secondary_init(void *arg)
{
	struct k_thread dummy_thread;

	/*
	 * This is an open-coded version of zephyr/kernel/smp.c
	 * smp_init_top(). We do this so that we can call SOF
	 * secondary_core_init() for each core.
	 */

	atomic_set(&ready_flag, 1);
	z_smp_thread_init(arg, &dummy_thread);
	smp_timer_init();

	secondary_core_init(sof_get());

#ifdef CONFIG_THREAD_STACK_INFO
	dummy_thread.stack_info.start = (uintptr_t)z_interrupt_stacks +
		arch_curr_cpu()->id * Z_KERNEL_STACK_LEN(CONFIG_ISR_STACK_SIZE);
	dummy_thread.stack_info.size = Z_KERNEL_STACK_LEN(CONFIG_ISR_STACK_SIZE);
#endif

	z_smp_thread_swap();

	CODE_UNREACHABLE; /* LCOV_EXCL_LINE */
}

int arch_cpu_enable_core(int id)
{
	pm_runtime_get(PM_RUNTIME_DSP, PWRD_BY_TPLG | id);

	return 0;
}

int z_wrapper_cpu_enable_secondary_core(int id)
{
	/*
	 * This is an open-coded version of zephyr/kernel/smp.c
	 * z_smp_start_cpu(). We do this, so we can use a customized
	 * secondary_init() for SOF.
	 */

	if (arch_cpu_active(id))
		return 0;

#if ZEPHYR_VERSION(3, 0, 99) <= ZEPHYR_VERSION_CODE
	z_init_cpu(id);
#endif

	atomic_clear(&start_flag);
	atomic_clear(&ready_flag);

	arch_start_cpu(id, z_interrupt_stacks[id], CONFIG_ISR_STACK_SIZE,
		       secondary_init, &start_flag);

	while (!atomic_get(&ready_flag))
		k_busy_wait(100);

	atomic_set(&start_flag, 1);

	return 0;
}

int arch_cpu_restore_secondary_cores(void)
{
	/* TODO: use Zephyr version */
	return 0;
}

int arch_cpu_secondary_cores_prepare_d0ix(void)
{
	/* TODO: use Zephyr version */
	return 0;
}

void arch_cpu_disable_core(int id)
{
	/* TODO: call Zephyr API */
}

int arch_cpu_is_core_enabled(int id)
{
	return arch_cpu_active(id);
}

void cpu_power_down_core(uint32_t flags)
{
	/* TODO: use Zephyr version */
}

int arch_cpu_enabled_cores(void)
{
	unsigned int i;
	int mask = 0;

	for (i = 0; i < CONFIG_MP_NUM_CPUS; i++)
		if (arch_cpu_active(i))
			mask |= BIT(i);

	return mask;
}

static struct idc idc[CONFIG_MP_NUM_CPUS];
static struct idc *p_idc[CONFIG_MP_NUM_CPUS];

struct idc **idc_get(void)
{
	int cpu = cpu_get_id();

	p_idc[cpu] = idc + cpu;

	return p_idc + cpu;
}
#endif

