/*
 * Copyright (c) 2007-2019 Apple Inc. All rights reserved.
 * Copyright (c) 2000-2006 Apple Computer, Inc. All rights reserved.
 */
#include <pexpert/pexpert.h>
#include <pexpert/boot.h>
#include <pexpert/protos.h>
#include <pexpert/device_tree.h>

#if defined(__arm__)
#include <pexpert/arm/board_config.h>
#elif defined(__arm64__)
#include <pexpert/arm64/board_config.h>
#endif

#include <kern/clock.h>
#include <machine/machine_routines.h>
#if DEVELOPMENT || DEBUG
#include <kern/simple_lock.h>
#include <kern/cpu_number.h>
#endif
/* Local declarations */
void            pe_identify_machine(boot_args * bootArgs);

/* External declarations */
extern void clean_mmu_dcache(void);

static char    *gPESoCDeviceType;
static char     gPESoCDeviceTypeBuffer[SOC_DEVICE_TYPE_BUFFER_SIZE];
static vm_offset_t gPESoCBasePhys;

static uint32_t gTCFG0Value;

static uint32_t pe_arm_init_timer(void *args);

#if DEVELOPMENT || DEBUG
decl_simple_lock_data(, panic_hook_lock);
#endif
/*
 * pe_identify_machine:
 *
 * Sets up platform parameters. Returns:    nothing
 */
void
pe_identify_machine(boot_args * bootArgs)
{
	OpaqueDTEntryIterator iter;
	DTEntry         cpus, cpu;
	uint32_t        mclk = 0, hclk = 0, pclk = 0, tclk = 0, use_dt = 0;
	unsigned long const *value;
	unsigned int    size;
	int             err;

	(void)bootArgs;

	if (pe_arm_get_soc_base_phys() == 0) {
		return;
	}

	/* Clear the gPEClockFrequencyInfo struct */
	bzero((void *)&gPEClockFrequencyInfo, sizeof(clock_frequency_info_t));

	if (!strcmp(gPESoCDeviceType, "s3c2410-io")) {
		mclk = 192 << 23;
		hclk = mclk / 2;
		pclk = hclk / 2;
		tclk = (1 << (23 + 2)) / 10;
		tclk = pclk / tclk;

		gTCFG0Value = tclk - 1;

		tclk = pclk / (4 * tclk);       /* Calculate the "actual"
		                                 * Timer0 frequency in fixed
		                                 * point. */

		mclk = (mclk >> 17) * (125 * 125);
		hclk = (hclk >> 17) * (125 * 125);
		pclk = (pclk >> 17) * (125 * 125);
		tclk = (((((tclk * 125) + 2) >> 2) * 125) + (1 << 14)) >> 15;
	} else if (!strcmp(gPESoCDeviceType, "integratorcp-io")) {
		mclk = 200000000;
		hclk = mclk / 2;
		pclk = hclk / 2;
		tclk = 100000;
	} else if (!strcmp(gPESoCDeviceType, "olocreek-io")) {
		mclk = 1000000000;
		hclk = mclk / 8;
		pclk = hclk / 2;
		tclk = pclk;
	} else if (!strcmp(gPESoCDeviceType, "omap3430sdp-io")) {
		mclk = 332000000;
		hclk =  19200000;
		pclk = hclk;
		tclk = pclk;
	} else if (!strcmp(gPESoCDeviceType, "s5i3000-io")) {
		mclk = 400000000;
		hclk = mclk / 4;
		pclk = hclk / 2;
		tclk = 100000;  /* timer is at 100khz */
	} else {
		use_dt = 1;
	}

	if (use_dt) {
		/* Start with default values. */
		gPEClockFrequencyInfo.timebase_frequency_hz = 24000000;
		gPEClockFrequencyInfo.bus_clock_rate_hz = 100000000;
		gPEClockFrequencyInfo.cpu_clock_rate_hz = 400000000;

		err = SecureDTLookupEntry(NULL, "/cpus", &cpus);
		assert(err == kSuccess);

		err = SecureDTInitEntryIterator(cpus, &iter);
		assert(err == kSuccess);

		while (kSuccess == SecureDTIterateEntries(&iter, &cpu)) {
			if ((kSuccess != SecureDTGetProperty(cpu, "state", (void const **)&value, &size)) ||
			    (strncmp((char const *)value, "running", size) != 0)) {
				continue;
			}

			/* Find the time base frequency first. */
			if (SecureDTGetProperty(cpu, "timebase-frequency", (void const **)&value, &size) == kSuccess) {
				/*
				 * timebase_frequency_hz is only 32 bits, and
				 * the device tree should never provide 64
				 * bits so this if should never be taken.
				 */
				if (size == 8) {
					gPEClockFrequencyInfo.timebase_frequency_hz = *(unsigned long long const *)value;
				} else {
					gPEClockFrequencyInfo.timebase_frequency_hz = *value;
				}
			}
			gPEClockFrequencyInfo.dec_clock_rate_hz = gPEClockFrequencyInfo.timebase_frequency_hz;

			/* Find the bus frequency next. */
			if (SecureDTGetProperty(cpu, "bus-frequency", (void const **)&value, &size) == kSuccess) {
				if (size == 8) {
					gPEClockFrequencyInfo.bus_frequency_hz = *(unsigned long long const *)value;
				} else {
					gPEClockFrequencyInfo.bus_frequency_hz = *value;
				}
			}
			gPEClockFrequencyInfo.bus_frequency_min_hz = gPEClockFrequencyInfo.bus_frequency_hz;
			gPEClockFrequencyInfo.bus_frequency_max_hz = gPEClockFrequencyInfo.bus_frequency_hz;

			if (gPEClockFrequencyInfo.bus_frequency_hz < 0x100000000ULL) {
				gPEClockFrequencyInfo.bus_clock_rate_hz = gPEClockFrequencyInfo.bus_frequency_hz;
			} else {
				gPEClockFrequencyInfo.bus_clock_rate_hz = 0xFFFFFFFF;
			}

			/* Find the memory frequency next. */
			if (SecureDTGetProperty(cpu, "memory-frequency", (void const **)&value, &size) == kSuccess) {
				if (size == 8) {
					gPEClockFrequencyInfo.mem_frequency_hz = *(unsigned long long const *)value;
				} else {
					gPEClockFrequencyInfo.mem_frequency_hz = *value;
				}
			}
			gPEClockFrequencyInfo.mem_frequency_min_hz = gPEClockFrequencyInfo.mem_frequency_hz;
			gPEClockFrequencyInfo.mem_frequency_max_hz = gPEClockFrequencyInfo.mem_frequency_hz;

			/* Find the peripheral frequency next. */
			if (SecureDTGetProperty(cpu, "peripheral-frequency", (void const **)&value, &size) == kSuccess) {
				if (size == 8) {
					gPEClockFrequencyInfo.prf_frequency_hz = *(unsigned long long const *)value;
				} else {
					gPEClockFrequencyInfo.prf_frequency_hz = *value;
				}
			}
			gPEClockFrequencyInfo.prf_frequency_min_hz = gPEClockFrequencyInfo.prf_frequency_hz;
			gPEClockFrequencyInfo.prf_frequency_max_hz = gPEClockFrequencyInfo.prf_frequency_hz;

			/* Find the fixed frequency next. */
			if (SecureDTGetProperty(cpu, "fixed-frequency", (void const **)&value, &size) == kSuccess) {
				if (size == 8) {
					gPEClockFrequencyInfo.fix_frequency_hz = *(unsigned long long const *)value;
				} else {
					gPEClockFrequencyInfo.fix_frequency_hz = *value;
				}
			}
			/* Find the cpu frequency last. */
			if (SecureDTGetProperty(cpu, "clock-frequency", (void const **)&value, &size) == kSuccess) {
				if (size == 8) {
					gPEClockFrequencyInfo.cpu_frequency_hz = *(unsigned long long const *)value;
				} else {
					gPEClockFrequencyInfo.cpu_frequency_hz = *value;
				}
			}
			gPEClockFrequencyInfo.cpu_frequency_min_hz = gPEClockFrequencyInfo.cpu_frequency_hz;
			gPEClockFrequencyInfo.cpu_frequency_max_hz = gPEClockFrequencyInfo.cpu_frequency_hz;

			if (gPEClockFrequencyInfo.cpu_frequency_hz < 0x100000000ULL) {
				gPEClockFrequencyInfo.cpu_clock_rate_hz = gPEClockFrequencyInfo.cpu_frequency_hz;
			} else {
				gPEClockFrequencyInfo.cpu_clock_rate_hz = 0xFFFFFFFF;
			}
		}
	} else {
		/* Use the canned values. */
		gPEClockFrequencyInfo.timebase_frequency_hz = tclk;
		gPEClockFrequencyInfo.fix_frequency_hz = tclk;
		gPEClockFrequencyInfo.bus_frequency_hz = hclk;
		gPEClockFrequencyInfo.cpu_frequency_hz = mclk;
		gPEClockFrequencyInfo.prf_frequency_hz = pclk;

		gPEClockFrequencyInfo.bus_frequency_min_hz = gPEClockFrequencyInfo.bus_frequency_hz;
		gPEClockFrequencyInfo.bus_frequency_max_hz = gPEClockFrequencyInfo.bus_frequency_hz;
		gPEClockFrequencyInfo.cpu_frequency_min_hz = gPEClockFrequencyInfo.cpu_frequency_hz;
		gPEClockFrequencyInfo.cpu_frequency_max_hz = gPEClockFrequencyInfo.cpu_frequency_hz;
		gPEClockFrequencyInfo.prf_frequency_min_hz = gPEClockFrequencyInfo.prf_frequency_hz;
		gPEClockFrequencyInfo.prf_frequency_max_hz = gPEClockFrequencyInfo.prf_frequency_hz;

		gPEClockFrequencyInfo.dec_clock_rate_hz = gPEClockFrequencyInfo.timebase_frequency_hz;
		gPEClockFrequencyInfo.bus_clock_rate_hz = gPEClockFrequencyInfo.bus_frequency_hz;
		gPEClockFrequencyInfo.cpu_clock_rate_hz = gPEClockFrequencyInfo.cpu_frequency_hz;
	}

	/* Set the num / den pairs form the hz values. */
	gPEClockFrequencyInfo.bus_clock_rate_num = gPEClockFrequencyInfo.bus_clock_rate_hz;
	gPEClockFrequencyInfo.bus_clock_rate_den = 1;

	gPEClockFrequencyInfo.bus_to_cpu_rate_num =
	    (2 * gPEClockFrequencyInfo.cpu_clock_rate_hz) / gPEClockFrequencyInfo.bus_clock_rate_hz;
	gPEClockFrequencyInfo.bus_to_cpu_rate_den = 2;

	gPEClockFrequencyInfo.bus_to_dec_rate_num = 1;
	gPEClockFrequencyInfo.bus_to_dec_rate_den =
	    gPEClockFrequencyInfo.bus_clock_rate_hz / gPEClockFrequencyInfo.dec_clock_rate_hz;
}

vm_offset_t
pe_arm_get_soc_base_phys(void)
{
	DTEntry         entryP;
	uintptr_t const *ranges_prop;
	uint32_t        prop_size;
	char const      *tmpStr;

	if (SecureDTFindEntry("name", "arm-io", &entryP) == kSuccess) {
		if (gPESoCDeviceType == 0) {
			SecureDTGetProperty(entryP, "device_type", (void const **)&tmpStr, &prop_size);
			strlcpy(gPESoCDeviceTypeBuffer, tmpStr, SOC_DEVICE_TYPE_BUFFER_SIZE);
			gPESoCDeviceType = gPESoCDeviceTypeBuffer;

			SecureDTGetProperty(entryP, "ranges", (void const **)&ranges_prop, &prop_size);
			gPESoCBasePhys = *(ranges_prop + 1);
		}
		return gPESoCBasePhys;
	}
	return 0;
}

extern void     fleh_fiq_generic(void);

#if defined(ARM_BOARD_CLASS_T8002)
extern void     fleh_fiq_t8002(void);
extern uint32_t t8002_get_decrementer(void);
extern void     t8002_set_decrementer(uint32_t);
static struct tbd_ops    t8002_funcs = {&fleh_fiq_t8002, &t8002_get_decrementer, &t8002_set_decrementer};
#endif /* defined(ARM_BOARD_CLASS_T8002) */

vm_offset_t     gPicBase;
vm_offset_t     gTimerBase;
vm_offset_t     gSocPhys;

#if DEVELOPMENT || DEBUG
// This block contains the panic trace implementation

// These variables are local to this file, and contain the panic trace configuration information
typedef enum{
	panic_trace_disabled = 0,
	panic_trace_unused,
	panic_trace_enabled,
	panic_trace_alt_enabled,
} panic_trace_t;
static panic_trace_t bootarg_panic_trace;

static int bootarg_stop_clocks;

// The command buffer contains the converted commands from the device tree for commanding cpu_halt, enable_trace, etc.
#define DEBUG_COMMAND_BUFFER_SIZE 256
typedef struct command_buffer_element {
	uintptr_t address;
	uintptr_t value;
	uint16_t destination_cpu_selector;
	uint16_t delay_us;
	bool is_32bit;
} command_buffer_element_t;
static command_buffer_element_t debug_command_buffer[DEBUG_COMMAND_BUFFER_SIZE];                // statically allocate to prevent needing alloc at runtime
static uint32_t  next_command_buffer_entry = 0;                                                                                // index of next unused slot in debug_command_buffer

#define CPU_SELECTOR_SHIFT              (16)
#define CPU_SELECTOR_MASK               (0xFFFF << CPU_SELECTOR_SHIFT)
#define REGISTER_OFFSET_MASK            ((1 << CPU_SELECTOR_SHIFT) - 1)
#define REGISTER_OFFSET(register_prop)  (register_prop & REGISTER_OFFSET_MASK)
#define CPU_SELECTOR(register_offset)   ((register_offset & CPU_SELECTOR_MASK) >> CPU_SELECTOR_SHIFT) // Upper 16bits holds the cpu selector
#define MAX_WINDOW_SIZE                 0xFFFF
#define PE_ISSPACE(c)                   (c == ' ' || c == '\t' || c == '\n' || c == '\12')
#define DELAY_SHIFT                     (32)
#define DELAY_MASK                      (0xFFFFULL << DELAY_SHIFT)
#define DELAY_US(register_offset)       ((register_offset & DELAY_MASK) >> DELAY_SHIFT)
#define REGISTER_32BIT_MASK             (1ULL << 63)
/*
 *  0x0000 - all cpus
 *  0x0001 - cpu 0
 *  0x0002 - cpu 1
 *  0x0004 - cpu 2
 *  0x0003 - cpu 0 and 1
 *  since it's 16bits, we can have up to 16 cpus
 */
#define ALL_CPUS 0x0000
#define IS_CPU_SELECTED(cpu_number, cpu_selector) (cpu_selector == ALL_CPUS ||  (cpu_selector & (1<<cpu_number) ) != 0 )

#define RESET_VIRTUAL_ADDRESS_WINDOW    0xFFFFFFFF

// Pointers into debug_command_buffer for each operation. Assumes runtime will init them to zero.
static command_buffer_element_t *cpu_halt;
static command_buffer_element_t *enable_trace;
static command_buffer_element_t *enable_alt_trace;
static command_buffer_element_t *trace_halt;
static command_buffer_element_t *enable_stop_clocks;
static command_buffer_element_t *stop_clocks;

// Record which CPU is currently running one of our debug commands, so we can trap panic reentrancy to PE_arm_debug_panic_hook.
static int running_debug_command_on_cpu_number = -1;

static void
pe_init_debug_command(DTEntry entryP, command_buffer_element_t **command_buffer, const char* entry_name)
{
	uintptr_t const *reg_prop;
	uint32_t        prop_size, reg_window_size = 0, command_starting_index;
	uintptr_t       debug_reg_window = 0;

	if (command_buffer == 0) {
		return;
	}

	if (SecureDTGetProperty(entryP, entry_name, (void const **)&reg_prop, &prop_size) != kSuccess) {
		panic("pe_init_debug_command: failed to read property %s\n", entry_name);
	}

	// make sure command will fit
	if (next_command_buffer_entry + prop_size / sizeof(uintptr_t) > DEBUG_COMMAND_BUFFER_SIZE - 1) {
		panic("pe_init_debug_command: property %s is %u bytes, command buffer only has %lu bytes remaining\n",
		    entry_name, prop_size, ((DEBUG_COMMAND_BUFFER_SIZE - 1) - next_command_buffer_entry) * sizeof(uintptr_t));
	}

	// Hold the pointer in a temp variable and later assign it to command buffer, in case we panic while half-initialized
	command_starting_index = next_command_buffer_entry;

	// convert to real virt addresses and stuff commands into debug_command_buffer
	for (; prop_size; reg_prop += 2, prop_size -= 2 * sizeof(uintptr_t)) {
		if (*reg_prop == RESET_VIRTUAL_ADDRESS_WINDOW) {
			debug_reg_window = 0; // Create a new window
		} else if (debug_reg_window == 0) {
			// create a window from virtual address to the specified physical address
			reg_window_size = ((uint32_t)*(reg_prop + 1));
			if (reg_window_size > MAX_WINDOW_SIZE) {
				panic("pe_init_debug_command: Command page size is %0x, exceeds the Maximum allowed page size 0f 0%x\n", reg_window_size, MAX_WINDOW_SIZE );
			}
			debug_reg_window =  ml_io_map(gSocPhys + *reg_prop, reg_window_size);
			// for debug -- kprintf("pe_init_debug_command: %s registers @ 0x%08lX for 0x%08lX\n", entry_name, debug_reg_window, *(reg_prop + 1) );
		} else {
			if ((REGISTER_OFFSET(*reg_prop) + sizeof(uintptr_t)) >= reg_window_size) {
				panic("pe_init_debug_command: Command Offset is %lx, exceeds allocated size of %x\n", REGISTER_OFFSET(*reg_prop), reg_window_size );
			}
			debug_command_buffer[next_command_buffer_entry].address = debug_reg_window + REGISTER_OFFSET(*reg_prop);
			debug_command_buffer[next_command_buffer_entry].destination_cpu_selector = (uint16_t)CPU_SELECTOR(*reg_prop);
#if defined(__arm64__)
			debug_command_buffer[next_command_buffer_entry].delay_us = DELAY_US(*reg_prop);
			debug_command_buffer[next_command_buffer_entry].is_32bit = ((*reg_prop & REGISTER_32BIT_MASK) != 0);
#else
			debug_command_buffer[next_command_buffer_entry].delay_us = 0;
			debug_command_buffer[next_command_buffer_entry].is_32bit = false;
#endif
			debug_command_buffer[next_command_buffer_entry++].value = *(reg_prop + 1);
		}
	}

	// null terminate the address field of the command to end it
	debug_command_buffer[next_command_buffer_entry++].address = 0;

	// save pointer into table for this command
	*command_buffer = &debug_command_buffer[command_starting_index];
}

static void
pe_run_debug_command(command_buffer_element_t *command_buffer)
{
	// When both the CPUs panic, one will get stuck on the lock and the other CPU will be halted when the first executes the debug command
	simple_lock(&panic_hook_lock, LCK_GRP_NULL);

	running_debug_command_on_cpu_number = cpu_number();

	while (command_buffer && command_buffer->address) {
		if (IS_CPU_SELECTED(running_debug_command_on_cpu_number, command_buffer->destination_cpu_selector)) {
			if (command_buffer->is_32bit) {
				*((volatile uint32_t*)(command_buffer->address)) = (uint32_t)(command_buffer->value);
			} else {
				*((volatile uintptr_t*)(command_buffer->address)) = command_buffer->value;      // register = value;
			}
			if (command_buffer->delay_us != 0) {
				uint64_t deadline;
				nanoseconds_to_absolutetime(command_buffer->delay_us * NSEC_PER_USEC, &deadline);
				deadline += ml_get_timebase();
				while (ml_get_timebase() < deadline) {
					os_compiler_barrier();
				}
			}
		}
		command_buffer++;
	}

	running_debug_command_on_cpu_number = -1;
	simple_unlock(&panic_hook_lock);
}


void
PE_arm_debug_enable_trace(void)
{
	switch (bootarg_panic_trace) {
	case panic_trace_enabled:
		pe_run_debug_command(enable_trace);
		break;

	case panic_trace_alt_enabled:
		pe_run_debug_command(enable_alt_trace);
		break;

	default:
		break;
	}
}

static void
PE_arm_panic_hook(const char *str __unused)
{
	(void)str; // not used
	if (bootarg_stop_clocks != 0) {
		pe_run_debug_command(stop_clocks);
	}
	// if panic trace is enabled
	if (bootarg_panic_trace != 0) {
		if (running_debug_command_on_cpu_number == cpu_number()) {
			// This is going to end badly if we don't trap, since we'd be panic-ing during our own code
			kprintf("## Panic Trace code caused the panic ##\n");
			return;  // allow the normal panic operation to occur.
		}

		// Stop tracing to freeze the buffer and return to normal panic processing.
		pe_run_debug_command(trace_halt);
	}
}

void (*PE_arm_debug_panic_hook)(const char *str) = PE_arm_panic_hook;

void
PE_init_cpu(void)
{
	if (bootarg_stop_clocks != 0) {
		pe_run_debug_command(enable_stop_clocks);
	}

	pe_init_fiq();
}

#else

void(*const PE_arm_debug_panic_hook)(const char *str) = NULL;

void
PE_init_cpu(void)
{
	pe_init_fiq();
}

#endif  // DEVELOPMENT || DEBUG

void
PE_panic_hook(const char *str __unused)
{
	if (PE_arm_debug_panic_hook != NULL) {
		PE_arm_debug_panic_hook(str);
	}
}

void
pe_arm_init_debug(void *args)
{
	DTEntry         entryP;
	uintptr_t const *reg_prop;
	uint32_t        prop_size;

	if (gSocPhys == 0) {
		kprintf("pe_arm_init_debug: failed to initialize gSocPhys == 0\n");
		return;
	}

	if (SecureDTFindEntry("device_type", "cpu-debug-interface", &entryP) == kSuccess) {
		if (args != NULL) {
			if (SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size) == kSuccess) {
				ml_init_arm_debug_interface(args, ml_io_map(gSocPhys + *reg_prop, *(reg_prop + 1)));
			}
#if DEVELOPMENT || DEBUG
			// When args != NULL, this means we're being called from arm_init on the boot CPU.
			// This controls one-time initialization of the Panic Trace infrastructure

			simple_lock_init(&panic_hook_lock, 0); //assuming single threaded mode

			// Panic_halt is deprecated. Please use panic_trace istead.
			unsigned int temp_bootarg_panic_trace;
			if (PE_parse_boot_argn("panic_trace", &temp_bootarg_panic_trace, sizeof(temp_bootarg_panic_trace)) ||
			    PE_parse_boot_argn("panic_halt", &temp_bootarg_panic_trace, sizeof(temp_bootarg_panic_trace))) {
				kprintf("pe_arm_init_debug: panic_trace=%d\n", temp_bootarg_panic_trace);

				// Prepare debug command buffers.
				pe_init_debug_command(entryP, &cpu_halt, "cpu_halt");
				pe_init_debug_command(entryP, &enable_trace, "enable_trace");
				pe_init_debug_command(entryP, &enable_alt_trace, "enable_alt_trace");
				pe_init_debug_command(entryP, &trace_halt, "trace_halt");

				// now that init's are done, enable the panic halt capture (allows pe_init_debug_command to panic normally if necessary)
				bootarg_panic_trace = temp_bootarg_panic_trace;

				// start tracing now if enabled
				PE_arm_debug_enable_trace();
			}
			unsigned int temp_bootarg_stop_clocks;
			if (PE_parse_boot_argn("stop_clocks", &temp_bootarg_stop_clocks, sizeof(temp_bootarg_stop_clocks))) {
				pe_init_debug_command(entryP, &enable_stop_clocks, "enable_stop_clocks");
				pe_init_debug_command(entryP, &stop_clocks, "stop_clocks");
				bootarg_stop_clocks = temp_bootarg_stop_clocks;
			}
#endif
		}
	} else {
		kprintf("pe_arm_init_debug: failed to find cpu-debug-interface\n");
	}
}

static uint32_t
pe_arm_map_interrupt_controller(void)
{
	DTEntry         entryP;
	uintptr_t const *reg_prop;
	uint32_t        prop_size;
	vm_offset_t     soc_phys = 0;

	gSocPhys = pe_arm_get_soc_base_phys();

	soc_phys = gSocPhys;
	kprintf("pe_arm_map_interrupt_controller: soc_phys:  0x%lx\n", (unsigned long)soc_phys);
	if (soc_phys == 0) {
		return 0;
	}

	if (SecureDTFindEntry("interrupt-controller", "master", &entryP) == kSuccess) {
		kprintf("pe_arm_map_interrupt_controller: found interrupt-controller\n");
		SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size);
		gPicBase = ml_io_map(soc_phys + *reg_prop, *(reg_prop + 1));
		kprintf("pe_arm_map_interrupt_controller: gPicBase: 0x%lx\n", (unsigned long)gPicBase);
	}
	if (gPicBase == 0) {
		kprintf("pe_arm_map_interrupt_controller: failed to find the interrupt-controller.\n");
		return 0;
	}

	if (SecureDTFindEntry("device_type", "timer", &entryP) == kSuccess) {
		kprintf("pe_arm_map_interrupt_controller: found timer\n");
		SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size);
		gTimerBase = ml_io_map(soc_phys + *reg_prop, *(reg_prop + 1));
		kprintf("pe_arm_map_interrupt_controller: gTimerBase: 0x%lx\n", (unsigned long)gTimerBase);
	}
	if (gTimerBase == 0) {
		kprintf("pe_arm_map_interrupt_controller: failed to find the timer.\n");
		return 0;
	}

	return 1;
}

uint32_t
pe_arm_init_interrupts(void *args)
{
	kprintf("pe_arm_init_interrupts: args: %p\n", args);

	/* Set up mappings for interrupt controller and possibly timers (if they haven't been set up already) */
	if (args != NULL) {
		if (!pe_arm_map_interrupt_controller()) {
			return 0;
		}
	}

	return pe_arm_init_timer(args);
}

static uint32_t
pe_arm_init_timer(void *args)
{
	vm_offset_t     pic_base = 0;
	vm_offset_t     timer_base = 0;
	vm_offset_t     soc_phys;
	vm_offset_t     eoi_addr = 0;
	uint32_t        eoi_value = 0;
	struct tbd_ops  generic_funcs = {&fleh_fiq_generic, NULL, NULL};
	struct tbd_ops  empty_funcs __unused = {NULL, NULL, NULL};
	tbd_ops_t       tbd_funcs = &generic_funcs;

	/* The SoC headers expect to use pic_base, timer_base, etc... */
	pic_base = gPicBase;
	timer_base = gTimerBase;
	soc_phys = gSocPhys;

#if defined(ARM_BOARD_CLASS_T8002)
	if (!strcmp(gPESoCDeviceType, "t8002-io") ||
	    !strcmp(gPESoCDeviceType, "t8004-io")) {
		/* Enable the Decrementer */
		aic_write32(kAICTmrCnt, 0x7FFFFFFF);
		aic_write32(kAICTmrCfg, kAICTmrCfgEn);
		aic_write32(kAICTmrIntStat, kAICTmrIntStatPct);
#ifdef ARM_BOARD_WFE_TIMEOUT_NS
		// Enable the WFE Timer
		rPMGR_EVENT_TMR_PERIOD = ((uint64_t)(ARM_BOARD_WFE_TIMEOUT_NS) *gPEClockFrequencyInfo.timebase_frequency_hz) / NSEC_PER_SEC;
		rPMGR_EVENT_TMR = rPMGR_EVENT_TMR_PERIOD;
		rPMGR_EVENT_TMR_CTL = PMGR_EVENT_TMR_CTL_EN;
#endif /* ARM_BOARD_WFE_TIMEOUT_NS */

		eoi_addr = pic_base;
		eoi_value = kAICTmrIntStatPct;
		tbd_funcs = &t8002_funcs;
	} else
#endif
#if defined(__arm64__)
	tbd_funcs = &empty_funcs;
#else
	return 0;
#endif

	if (args != NULL) {
		ml_init_timebase(args, tbd_funcs, eoi_addr, eoi_value);
	}

	return 1;
}
