// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    adsp2100.cpp

    ADSP-21xx series emulator.

****************************************************************************

    For ADSP-2101, ADSP-2111
    ------------------------

        MMAP = 0                                        MMAP = 1

        Automatic boot loading                          No auto boot loading

        Program Space:                                  Program Space:
            0000-07ff = 2k Internal RAM (booted)            0000-37ff = 14k External access
            0800-3fff = 14k External access                 3800-3fff = 2k Internal RAM

        Data Space:                                     Data Space:
            0000-03ff = 1k External DWAIT0                  0000-03ff = 1k External DWAIT0
            0400-07ff = 1k External DWAIT1                  0400-07ff = 1k External DWAIT1
            0800-2fff = 10k External DWAIT2                 0800-2fff = 10k External DWAIT2
            3000-33ff = 1k External DWAIT3                  3000-33ff = 1k External DWAIT3
            3400-37ff = 1k External DWAIT4                  3400-37ff = 1k External DWAIT4
            3800-3bff = 1k Internal RAM                     3800-3bff = 1k Internal RAM
            3c00-3fff = 1k Internal Control regs            3c00-3fff = 1k Internal Control regs


    For ADSP-2105, ADSP-2115
    ------------------------

        MMAP = 0                                        MMAP = 1

        Automatic boot loading                          No auto boot loading

        Program Space:                                  Program Space:
            0000-03ff = 1k Internal RAM (booted)            0000-37ff = 14k External access
            0400-07ff = 1k Reserved                         3800-3bff = 1k Internal RAM
            0800-3fff = 14k External access                 3c00-3fff = 1k Reserved

        Data Space:                                     Data Space:
            0000-03ff = 1k External DWAIT0                  0000-03ff = 1k External DWAIT0
            0400-07ff = 1k External DWAIT1                  0400-07ff = 1k External DWAIT1
            0800-2fff = 10k External DWAIT2                 0800-2fff = 10k External DWAIT2
            3000-33ff = 1k External DWAIT3                  3000-33ff = 1k External DWAIT3
            3400-37ff = 1k External DWAIT4                  3400-37ff = 1k External DWAIT4
            3800-39ff = 512 Internal RAM                    3800-39ff = 512 Internal RAM
            3a00-3bff = 512 Reserved                        3a00-3bff = 512 Reserved
            3c00-3fff = 1k Internal Control regs            3c00-3fff = 1k Internal Control regs


    For ADSP-2104
    -------------

        MMAP = 0                                        MMAP = 1

        Automatic boot loading                          No auto boot loading

        Program Space:                                  Program Space:
            0000-01ff = 512 Internal RAM (booted)           0000-37ff = 14k External access
            0200-07ff = 1.5k Reserved                       3800-39ff = 512 Internal RAM
            0800-3fff = 14k External access                 3a00-3fff = 1.5k Reserved

        Data Space:                                     Data Space:
            0000-03ff = 1k External DWAIT0                  0000-03ff = 1k External DWAIT0
            0400-07ff = 1k External DWAIT1                  0400-07ff = 1k External DWAIT1
            0800-2fff = 10k External DWAIT2                 0800-2fff = 10k External DWAIT2
            3000-33ff = 1k External DWAIT3                  3000-33ff = 1k External DWAIT3
            3400-37ff = 1k External DWAIT4                  3400-37ff = 1k External DWAIT4
            3800-38ff = 256 Internal RAM                    3800-38ff = 256 Internal RAM
            3900-3bff = 768 Reserved                        3900-3bff = 768 Reserved
            3c00-3fff = 1k Internal Control regs            3c00-3fff = 1k Internal Control regs


    For ADSP-2181
    -------------

        MMAP = 0                                        MMAP = 1

        Auto boot loading via BDMA or IDMA              No auto boot loading

        Program Space:                                  Program Space:
            0000-1fff = 8k Internal RAM                     0000-1fff = 8k External access
            2000-3fff = 8k Internal RAM (PMOVLAY = 0)       2000-3fff = 8k Internal (PMOVLAY = 0)
            2000-3fff = 8k External (PMOVLAY = 1,2)

        Data Space:                                     Data Space:
            0000-1fff = 8k Internal RAM (DMOVLAY = 0)       0000-1fff = 8k Internal RAM (DMOVLAY = 0)
            0000-1fff = 8k External (DMOVLAY = 1,2)         0000-1fff = 8k External (DMOVLAY = 1,2)
            2000-3fdf = 8k-32 Internal RAM                  2000-3fdf = 8k-32 Internal RAM
            3fe0-3fff = 32 Internal Control regs            3fe0-3fff = 32 Internal Control regs

        I/O Space:                                      I/O Space:
            0000-01ff = 512 External IOWAIT0                0000-01ff = 512 External IOWAIT0
            0200-03ff = 512 External IOWAIT1                0200-03ff = 512 External IOWAIT1
            0400-05ff = 512 External IOWAIT2                0400-05ff = 512 External IOWAIT2
            0600-07ff = 512 External IOWAIT3                0600-07ff = 512 External IOWAIT3

    TODO:
    - Move internal stuffs into CPU core file (on-chip RAM, control registers, etc)
    - Support variable internal memory mappings

***************************************************************************/

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include "adsp2100.h"


/*###################################################################################################
**	CONSTANTS
**#################################################################################################*/

/* chip types */
#define CHIP_TYPE_ADSP2100	0
#define CHIP_TYPE_ADSP2101	1
#define CHIP_TYPE_ADSP2105	2
#define CHIP_TYPE_ADSP2115	3



/*###################################################################################################
**	ACCESS TO EMULATED PROCE$SSOR MEMORY SPACE
**#################################################################################################*/

/****************************************************************************/
/* Write a 24-bit value to program memory                                   */
/****************************************************************************/
#define ADSP2100_WRPGM(A,V)	(*(UINT32 *)(A) = (V) & 0xffffff)



/*###################################################################################################
**	PUBLIC GLOBAL VARIABLES
**#################################################################################################*/

int	adsp2100_icount = 50000;


/*###################################################################################################
**	PRIVATE STATIC VARIABLES
**#################################################################################################*/

static adsp2100_Regs adsp2100;

// public access to register file
adsp2100_Regs& adsp2100_get_regs() { return adsp2100; }


static int chip_type = CHIP_TYPE_ADSP2100;
static int mstat_mask;
static int imask_mask;

static UINT16 *reverse_table = 0;
static UINT16 *mask_table = 0;
static UINT8 *condition_table = 0;

static RX_CALLBACK sport_rx_callback = 0;
static TX_CALLBACK sport_tx_callback = 0;


/*###################################################################################################
**	PRIVATE FUNCTION PROTOTYPES
**#################################################################################################*/

static int create_tables(void);
static void check_irqs(void);


/*###################################################################################################
**	MEMORY ACCESSORS
**#################################################################################################*/


INLINE UINT32 RWORD_DATA(UINT32 addr)
{
	return adsp2100_host_read_dm(addr);
}

INLINE void WWORD_DATA(UINT32 addr, UINT32 data)
{
	adsp2100_host_write_dm(addr, data);
}

INLINE UINT32 RWORD_PGM(UINT32 addr)
{
	// special case for original (pre DCS-95) boards - PM($3000)
	// is the data port
	if (addr == 0x3000) 
		return adsp2100_host_read_pm(addr) << 8;

	return adsp2100_op_rom[addr];
}

INLINE void WWORD_PGM(UINT32 addr, UINT32 data)
{
	// special case hack for pre-WPC95 DCS - program memory 0x3000 
	// is the sound board data port
	if (addr == 0x3000)
		adsp2100_host_write_pm(addr, (data >> 8));

	adsp2100_op_rom[addr] = data;
}

#define ROPCODE() RWORD_PGM(adsp2100.pc)


/*###################################################################################################
**	OTHER INLINES
**#################################################################################################*/

INLINE void set_core_2100(void)
{
	chip_type = CHIP_TYPE_ADSP2100;
	mstat_mask = 0x0f;
	imask_mask = 0x0f;
}

#if (HAS_ADSP2101)
INLINE void set_core_2101(void)
{
	chip_type = CHIP_TYPE_ADSP2101;
	mstat_mask = 0x7f;
	imask_mask = 0x3f;
}
#endif

#if (HAS_ADSP2105)
INLINE void set_core_2105(void)
{
	chip_type = CHIP_TYPE_ADSP2105;
	mstat_mask = 0x7f;
	imask_mask = 0x3f;
}
#endif

#if (HAS_ADSP2115)
INLINE void set_core_2115(void)
{
	chip_type = CHIP_TYPE_ADSP2115;
	mstat_mask = 0x7f;
	imask_mask = 0x3f;
}
#endif


/*###################################################################################################
**	IMPORT CORE UTILITIES
**#################################################################################################*/

#include "2100ops.h"



/*###################################################################################################
**	IRQ HANDLING
**#################################################################################################*/

INLINE int adsp2100_generate_irq(int which)
{
	/* skip if masked */
	if (!(adsp2100.imask & (1 << which)))
		return 0;

	/* clear the latch */
	adsp2100.irq_latch[which] = 0;

	/* push the PC and the status */
	pc_stack_push();
	stat_stack_push();

	/* vector to location & stop idling */
	adsp2100.pc = which;
	adsp2100.idle = 0;

	/* mask other interrupts based on the nesting bit */
	if (adsp2100.icntl & 0x10) adsp2100.imask &= ~((2 << which) - 1);
	else adsp2100.imask &= ~0xf;

	return 1;
}


INLINE int adsp2101_generate_irq(int which, int indx)
{
	/* skip if masked */
	if (!(adsp2100.imask & (0x20 >> indx)))
		return 0;

	/* clear the latch */
	adsp2100.irq_latch[which] = 0;

	/* push the PC and the status */
	pc_stack_push();
	stat_stack_push();

	/* vector to location & stop idling */
	adsp2100.pc = 0x04 + indx * 4;
	adsp2100.idle = 0;

	/* mask other interrupts based on the nesting bit */
	if (adsp2100.icntl & 0x10) adsp2100.imask &= ~(0x3f >> indx);
	else adsp2100.imask &= ~0x3f;

	return 1;
}

static void check_irqs(void)
{
	UINT8 check;
	if (chip_type >= CHIP_TYPE_ADSP2101)
	{
		/* check IRQ2 */
		check = (adsp2100.icntl & 4) ? adsp2100.irq_latch[ADSP2101_IRQ2] : adsp2100.irq_state[ADSP2101_IRQ2];
		if (check && adsp2101_generate_irq(ADSP2101_IRQ2, 0))
			return;

		/* check SPORT0 transmit */
		check = adsp2100.irq_latch[ADSP2101_SPORT0_TX];
		if (check && adsp2101_generate_irq(ADSP2101_SPORT0_TX, 1))
			return;

		/* check SPORT0 receive */
		check = adsp2100.irq_latch[ADSP2101_SPORT0_RX];
		if (check && adsp2101_generate_irq(ADSP2101_SPORT0_RX, 2))
			return;

		/* check IRQ1/SPORT1 transmit */
		check = (adsp2100.icntl & 2) ? adsp2100.irq_latch[ADSP2101_IRQ1] : adsp2100.irq_state[ADSP2101_IRQ1];
		if (check && adsp2101_generate_irq(ADSP2101_IRQ1, 3))
			return;

		/* check IRQ0/SPORT1 receive */
		check = (adsp2100.icntl & 1) ? adsp2100.irq_latch[ADSP2101_IRQ0] : adsp2100.irq_state[ADSP2101_IRQ0];
		if (check && adsp2101_generate_irq(ADSP2101_IRQ0, 4))
			return;
	}
	else
	{
		/* check IRQ3 */
		check = (adsp2100.icntl & 8) ? adsp2100.irq_latch[ADSP2100_IRQ3] : adsp2100.irq_state[ADSP2100_IRQ3];
		if (check && adsp2100_generate_irq(ADSP2100_IRQ3))
			return;

		/* check IRQ2 */
		check = (adsp2100.icntl & 4) ? adsp2100.irq_latch[ADSP2100_IRQ2] : adsp2100.irq_state[ADSP2100_IRQ2];
		if (check && adsp2100_generate_irq(ADSP2100_IRQ2))
			return;

		/* check IRQ1 */
		check = (adsp2100.icntl & 2) ? adsp2100.irq_latch[ADSP2100_IRQ1] : adsp2100.irq_state[ADSP2100_IRQ1];
		if (check && adsp2100_generate_irq(ADSP2100_IRQ1))
			return;

		/* check IRQ0 */
		check = (adsp2100.icntl & 1) ? adsp2100.irq_latch[ADSP2100_IRQ0] : adsp2100.irq_state[ADSP2100_IRQ0];
		if (check && adsp2100_generate_irq(ADSP2100_IRQ0))
			return;
	}
}

void adsp2100_host_invoke_irq(int which, int indx, int cycleLimit)
{
	adsp2100.pc = 0xffff;
	adsp2101_generate_irq(which, indx);
	check_irqs();
	adsp2100_execute(cycleLimit);
}


/*###################################################################################################
**	INITIALIZATION AND SHUTDOWN
**#################################################################################################*/

void adsp2100_init(void)
{
	/* create the tables */
	if (!create_tables())
		exit(-1);
}

// external access to MSTAT register
void adsp2100_set_mstat(int val) { set_mstat(val); }

// external access to Ix registers
void adsp2100_set_Ix_reg(int x, INT32 val)
{
	adsp2100.i[x] = val & 0x3fff;
	adsp2100.base[x] = val & adsp2100.lmask[x];
}

void adsp2100_reset(void *param)
{
	/* ensure that zero is zero */
	adsp2100.core.zero.u = adsp2100.alt.zero.u = 0;

	/* recompute the memory registers with their current values */
	wr_l0(adsp2100.l[0]);  wr_i0(adsp2100.i[0]);
	wr_l1(adsp2100.l[1]);  wr_i1(adsp2100.i[1]);
	wr_l2(adsp2100.l[2]);  wr_i2(adsp2100.i[2]);
	wr_l3(adsp2100.l[3]);  wr_i3(adsp2100.i[3]);
	wr_l4(adsp2100.l[4]);  wr_i4(adsp2100.i[4]);
	wr_l5(adsp2100.l[5]);  wr_i5(adsp2100.i[5]);
	wr_l6(adsp2100.l[6]);  wr_i6(adsp2100.i[6]);
	wr_l7(adsp2100.l[7]);  wr_i7(adsp2100.i[7]);

	/* reset PC and loops */
	switch (chip_type)
	{
		case CHIP_TYPE_ADSP2100:
			adsp2100.pc = 4;
			break;

		case CHIP_TYPE_ADSP2101:
		case CHIP_TYPE_ADSP2105:
		case CHIP_TYPE_ADSP2115:
			adsp2100.pc = 0;
			break;

		default:
			// logerror( "ADSP2100 core: Unknown chip type!. Defaulting to ADSP2100.\n" );
			adsp2100.pc = 4;
			chip_type = CHIP_TYPE_ADSP2100;
			break;
	}

	adsp2100.ppc = -1;
	adsp2100.loop = 0xffff;
	adsp2100.loop_condition = 0;

	/* reset status registers */
	adsp2100.astat_clear = ~(CFLAG | VFLAG | NFLAG | ZFLAG);
	adsp2100.mstat = 0;
	adsp2100.sstat = 0x55;
	adsp2100.idle = 0;

	/* reset stacks */
	adsp2100.pc_sp = 0;
	adsp2100.cntr_sp = 0;
	adsp2100.stat_sp = 0;
	adsp2100.loop_sp = 0;

	/* reset external I/O */
	adsp2100.flagout = 0;
	adsp2100.flagin = 0;
	adsp2100.fl0 = 0;
	adsp2100.fl1 = 0;
	adsp2100.fl2 = 0;

	/* reset interrupts */
	adsp2100.imask = 0;
	adsp2100.irq_state[0] = 0;
	adsp2100.irq_state[1] = 0;
	adsp2100.irq_state[2] = 0;
	adsp2100.irq_state[3] = 0;
	adsp2100.irq_latch[0] = 0;
	adsp2100.irq_latch[1] = 0;
	adsp2100.irq_latch[2] = 0;
	adsp2100.irq_latch[3] = 0;
	adsp2100.interrupt_cycles = 0;
}


static int create_tables(void)
{
	int i;

	/* allocate the tables */
	if (!reverse_table)
		reverse_table = (UINT16 *)malloc(0x4000 * sizeof(UINT16));
	if (!mask_table)
		mask_table = (UINT16 *)malloc(0x4000 * sizeof(UINT16));
	if (!condition_table)
		condition_table = (UINT8 *)malloc(0x1000 * sizeof(UINT8));

	/* handle errors */
	if (!reverse_table || !mask_table || !condition_table)
		return 0;

	/* initialize the bit reversing table */
	for (i = 0; i < 0x4000; i++)
	{
		UINT16 data = 0;

		data |= (i >> 13) & 0x0001;
		data |= (i >> 11) & 0x0002;
		data |= (i >> 9)  & 0x0004;
		data |= (i >> 7)  & 0x0008;
		data |= (i >> 5)  & 0x0010;
		data |= (i >> 3)  & 0x0020;
		data |= (i >> 1)  & 0x0040;
		data |= (i << 1)  & 0x0080;
		data |= (i << 3)  & 0x0100;
		data |= (i << 5)  & 0x0200;
		data |= (i << 7)  & 0x0400;
		data |= (i << 9)  & 0x0800;
		data |= (i << 11) & 0x1000;
		data |= (i << 13) & 0x2000;

		reverse_table[i] = data;
	}

	// initialize the mask table
	for (i = 0; i < 0x4000; i++)
	{
		if (i > 0x2000)      mask_table[i] = 0x0000;
		else if (i > 0x1000) mask_table[i] = 0x2000;
		else if (i > 0x0800) mask_table[i] = 0x3000;
		else if (i > 0x0400) mask_table[i] = 0x3800;
		else if (i > 0x0200) mask_table[i] = 0x3c00;
		else if (i > 0x0100) mask_table[i] = 0x3e00;
		else if (i > 0x0080) mask_table[i] = 0x3f00;
		else if (i > 0x0040) mask_table[i] = 0x3f80;
		else if (i > 0x0020) mask_table[i] = 0x3fc0;
		else if (i > 0x0010) mask_table[i] = 0x3fe0;
		else if (i > 0x0008) mask_table[i] = 0x3ff0;
		else if (i > 0x0004) mask_table[i] = 0x3ff8;
		else if (i > 0x0002) mask_table[i] = 0x3ffc;
		else if (i > 0x0001) mask_table[i] = 0x3ffe;
		else                 mask_table[i] = 0x3fff;
	}

	// initialize the condition table
	for (i = 0; i < 0x100; i++)
	{
		int az = ((i & ZFLAG) != 0);
		int an = ((i & NFLAG) != 0);
		int av = ((i & VFLAG) != 0);
		int ac = ((i & CFLAG) != 0);
		int mv = ((i & MVFLAG) != 0);
		int as = ((i & SFLAG) != 0);

		condition_table[i | 0x000] = az;
		condition_table[i | 0x100] = !az;
		condition_table[i | 0x200] = !((an ^ av) | az);
		condition_table[i | 0x300] = (an ^ av) | az;
		condition_table[i | 0x400] = an ^ av;
		condition_table[i | 0x500] = !(an ^ av);
		condition_table[i | 0x600] = av;
		condition_table[i | 0x700] = !av;
		condition_table[i | 0x800] = ac;
		condition_table[i | 0x900] = !ac;
		condition_table[i | 0xa00] = as;
		condition_table[i | 0xb00] = !as;
		condition_table[i | 0xc00] = mv;
		condition_table[i | 0xd00] = !mv;
		condition_table[i | 0xf00] = 1;
	}
	return 1;
}


void adsp2100_exit(void)
{
	if (reverse_table)
		free(reverse_table);
	reverse_table = NULL;

	if (mask_table)
		free(mask_table);
	mask_table = NULL;

	if (condition_table)
		free(condition_table);
	condition_table = NULL;
}

/*###################################################################################################
**	INTERACTIVE DEBUGGER
**#################################################################################################*/

#define ADSP_DEBUGGER
#ifdef ADSP_DEBUGGER
#include <regex>
static bool debugger_enabled = false;
static int breakpointByAddr[0x4000];
struct Breakpoint
{
	bool inUse = false;
	uint32_t addr = 0;
};
static Breakpoint breakpoints[16];
static bool singleStepMode = true;
static uint32_t stepOverAddr = 0xFFFF;
extern unsigned adsp2100_dasm(char *buffer, unsigned long op);

void adsp2100_init_debugger()
{
	debugger_enabled = true;
}

void adsp2100_debug_break()
{
	singleStepMode = true;
}

static void update_breakpoints()
{
	memset(breakpointByAddr, 0, sizeof(breakpointByAddr));
	for (size_t i = 0 ; i < _countof(breakpoints) ; ++i)
	{
		if (breakpoints[i].inUse)
		{
			auto addr = breakpoints[i].addr;
			if (addr >= 0 && addr < 0x4000)
				breakpointByAddr[addr] = i + 1;
		}
	}
}

static uint32_t parse_debug_val(const char *p)
{
	for (; *p == ' ' ; ++p);
	if (*p == '$')
	{
		return strtol(p+1, nullptr, 16);
	}
	else
	{
		// parse as decimal
		return atol(p);
	}
}

static char debug_byte_print_rep(uint16_t data)
{
	return (data >= 32 && data < 127) || (data >= 128+32 && data < 255) ? static_cast<char>(data) : '.';
}

static void debugger()
{
	// assume we're not stopping
	bool stopping = false;

	// check for a breakpoint at the current location
	auto pc = adsp2100.pc;
	if (breakpointByAddr[pc] != 0)
	{
		printf("Stop at breakpoint #%d\n", breakpointByAddr[pc]);
		stopping = true;
	}

	// stop if we're in single-step mode
	if (singleStepMode)
		stopping = true;

	// stop if we're at the step-over/step-out address
	if (pc == stepOverAddr)
		stopping = true;

	// if we're not stopping, proceed with execution
	if (!stopping)
		return;

	// clear step modes
	singleStepMode = false;
	stepOverAddr = 0xFFFF;

	// show the current code location
	char buf[256];
	uint32_t opcode = adsp2100_op_rom[pc];
	adsp2100_dasm(buf, opcode);
	printf("%04x %02x %02x %02x  %s\n",
		pc, (opcode >> 16) & 0xFF, (opcode >> 8) & 0xFF, opcode & 0xFF, buf);

	// use the current PC as the implied repeat address for 'u' (unassemble)
	static uint32_t uRepeatAddr;
	uRepeatAddr = pc;

	static const char *repeatCommand = 0;
	for (;;)
	{
		// read a command
		printf("$ ");
		if (fgets(buf, sizeof(buf), stdin) == nullptr)
			strcpy_s(buf, "\x1A");

		// trim leading and trailing spaces
		char *p;
		for (p = buf + strlen(buf); p != buf && (*(p-1) == ' ' || *(p-1) == '\n' || *(p-1) == '\r'); *--p = 0);
		for (p = buf ; *p == ' '; ++p);

		// process the command
		static const char *repeatCommand = nullptr;
		static uint32_t repeatAddr = 0;

		if (*p == 0 && repeatCommand != nullptr)
		{
			// empty command - repeat last g, t, or p command
			strcpy_s(buf, repeatCommand);
		}

		// presume the command isn't one that auto-repeats
		repeatCommand = nullptr;

		// check the new command
		if (strcmp(p, "g") == 0 || *p == 26 || *p == 4)
		{
			// Go - return the interpreter loop
			repeatCommand = "g";
			return;
		}
		else if (strncmp(p, "g ", 2) == 0)
		{
			// Go with temporary breakpoint
			stepOverAddr = parse_debug_val(p + 2);
			return;
		}
		else if (strcmp(p, "t") == 0)
		{
			// Trace - single step to next instruction
			singleStepMode = true;
			repeatCommand = "t";
			return;
		}
		else if (strcmp(p, "p") == 0 || strcmp(p, "pl") == 0)
		{
			// Procedure trace (step over) - step until next instruction, 
			// skipping subroutines and (optionally) loops.
			if ((opcode & 0xFC0000) == 0x140000 && strcmp(p, "pl") == 0)
			{
				// on a LOOP instruction - stop at the next address after the end of the loop
				stepOverAddr = ((opcode >> 4) & 0x3FFF) + 1;
			}
			else if ((opcode & 0xFF0001) == 0x030001)
			{
				// CALL/JUMP on flag in - step to next instruction
				stepOverAddr = pc + 1;
			}
			else if ((opcode & 0xFFFF30) == 0x130010)
			{
				// conditional call indirect
				stepOverAddr = pc + 1;
			}
			else if ((opcode & 0xFC0000) == 0x1C0000)
			{
				// conditional call
				stepOverAddr = pc + 1;
			}
			else
			{
				// treat anything else as a simple trace
				singleStepMode = true;
			}
			repeatCommand = (p[1] == 'l' ? "pl" : "p");
			return;
		}
		else if (strcmp(p, "tl") == 0)
		{
			// Step out of loop
			if (adsp2100.loop != 0xFFFF)
			{
				stepOverAddr = adsp2100.loop + 1;
				repeatCommand = "tl";
				return;
			}
			else
				printf("Loop stack is empty\n");
		}
		else if (strcmp(p, "tr") == 0)
		{
			// Step out of subroutine
			if (adsp2100.pc_sp != 0)
			{
				stepOverAddr = pc_stack_top();
				repeatCommand = "tr";
				return;
			}
			else
				printf("PC stack is empty\n");
		}
		else if (strncmp(p, "bp ", 3) == 0)
		{
			// set a breakpoint at a PM() location

			// find a free breakpoint
			bool ok = false;
			for (size_t i = 0 ; i < _countof(breakpoints); ++i)
			{
				if (!breakpoints[i].inUse)
				{
					// add it
					breakpoints[i].inUse = true;
					breakpoints[i].addr = parse_debug_val(p + 3);
					ok = true;
					printf("Breakpoint #%d added at $%04x\n", i+1, breakpoints[i].addr);
					update_breakpoints();
					break;
				}
			}

			if (!ok)
				printf("No more breakpoint slots are available\n");
		}
		else if (strcmp(p, "bl") == 0)
		{
			// list breakpoints
			int cnt = 0;
			for (size_t i = 0 ; i < _countof(breakpoints); ++i)
			{
				if (breakpoints[i].inUse)
				{
					printf("#%-2d  $%04x\n", i+1, breakpoints[i].addr);
					++cnt;
				}
			}
			if (cnt == 0)
				printf("No breakpoints are currently set\n");
		}
		else if (strncmp(p, "bc ", 3) == 0)
		{
			// clear breakpoint
			int i = atoi(p+3) - 1;
			if (i >= 0 && i < _countof(breakpoints) && breakpoints[i].inUse)
			{
				breakpoints[i].inUse = false;
				update_breakpoints();
				printf("Breakpoint deleted\n");
			}
			else
				printf("No such breakpoint\n");
		}
		else if (strncmp(p, "dm ", 3) == 0 || strcmp(p, "dm") == 0
			|| strncmp(p, "dp ", 3) == 0 || strcmp(p, "dp") == 0)
		{
			// dump data memory / program memory
			char memType = p[1];

			// check arguments
			uint32_t addr;
			int count = 16;
			static char fmt = 'b';
			if (p[2] != 0)
			{
				// check for a format code
				for (p += 2 ; *p == ' ' ; ++p);
				if (*p == 'b' || *p == 'w' || *p == 'd')
					fmt = *p++;

				// check for a "*count" with no address
				for (; *p == ' ' ; ++p);
				if (*p != '*')
				{
					// no '*', so it's the starting address
					addr = parse_debug_val(p);
					for (; *p != 0 && *p != ' '; ++p);
				}
				else
				{
					// '*', so it's a count - continue from the last address
					addr = repeatAddr;
				}

				// check for a length, with or without '*'
				if (*p == '*')
					++p;
				if (*p != 0)
					count = parse_debug_val(p);
			}
			else
			{
				// no arguments - continue from the last address and use the default count
				addr = repeatAddr;
			}

			char cbuf[33];
			while (count != 0)
			{
				int i = 0, ci = 0;
				printf("%04x  ", addr);
				int perLine = fmt == 'd' ? 8 : 16;
				for ( ; i < perLine && count != 0 ; ++i, ++addr, --count)
				{
					uint32_t data = memType == 'm' ? RWORD_DATA(addr) : RWORD_PGM(addr);
					cbuf[ci++] = debug_byte_print_rep(data & 0xFF);
					cbuf[ci++] = debug_byte_print_rep((data >> 8) & 0xFF);
					if (fmt == 'd')
					{
						cbuf[ci++] = debug_byte_print_rep((data >> 16) & 0xFF);
						cbuf[ci++] = debug_byte_print_rep((data >> 24) & 0xFF);
					}
					if (fmt == 'b')
						printf("%02x %02x ", data & 0xFF, (data >> 8) & 0xFF);
					else if (fmt == 'w')
						printf("%04x ", data);
					else
						printf("%08x ", data);
				}
				for (; i < perLine ; ++i)
					printf(fmt == 'b' ? "      " : fmt == 'w' ? "     " : "         ");
				cbuf[ci] = 0;
				printf("%s\n", cbuf);
			}

			// this is repeatable at the next address
			repeatCommand = memType == 'm' ? "dm" : "dp";
			repeatAddr = addr;
		}
		else if (strcmp(p, "u") == 0 || strncmp(p, "u ", 2) == 0)
		{
			uint32_t addr = p[1] == 0 ? uRepeatAddr : parse_debug_val(p + 2);
			for (int i = 0 ; i < 16 ; ++i, ++addr)
			{
				uint32_t op = adsp2100_op_rom[addr];
				adsp2100_dasm(buf, op);
				printf("%04x %02x %02x %02x  %s\n", 
					addr, (op >> 16) & 0xFF, (op >> 8) & 0xFF, op & 0xFF, buf);
			}
			repeatCommand = "u";
			uRepeatAddr = addr;
		}
		else if (strcmp(p, "r") == 0)
		{
			auto &a = adsp2100;
			auto &core = adsp2100.core;
			printf("AX0=%04x AX1=%04x  AY0=%04x AY1=%04x  AR=%04x AF=%04x  PX=%02x\n"
				"MX0=%04x MX1=%04x  MY0=%04x MY1=%04x  MR=%012I64x MF=%04x\n"
				"SI=%04x  SE=%04x  SB=%04x  SR=%08x\n"
				"I0=%04x I1=%04x I2=%04x I3=%04x I4=%04x I5=%04x I6=%04x I7=%04x\n"
				"M0=%04x M1=%04x M2=%04x M3=%04x M4=%04x M5=%04x M6=%04x M7=%04x\n"
				"L0=%04x L1=%04x L2=%04x L3=%04x L4=%04x L5=%04x L6=%04x L7=%04x\n"
				"PC=%04x CNTR=%04x ASTAT=%04x SSTAT=%04x MSTAT=%04x\n",

				core.ax0.u, core.ax1.u, core.ay0.u, core.ay1.u, core.ar.u, core.af.u, adsp2100.px,
				core.mx0.u, core.mx1.u, core.my0.u, core.my1.u, core.mr.mr & 0xFFFFFFFFFFFF, core.mf.u,
				core.si.u, core.se.u, core.sb.u, core.sr.sr,
				a.i[0] & 0xFFFF, a.i[1] & 0xFFFF, a.i[2] & 0xFFFF, a.i[3] & 0xFFFF, a.i[4] & 0xFFFF, a.i[5] & 0xFFFF, a.i[6] & 0xFFFF, a.i[7] & 0xFFFF,
				a.m[0] & 0xFFFF, a.m[1] & 0xFFFF, a.m[2] & 0xFFFF, a.m[3] & 0xFFFF, a.m[4] & 0xFFFF, a.m[5] & 0xFFFF, a.m[6] & 0xFFFF, a.m[7] & 0xFFFF,
				a.l[0] & 0xFFFF, a.l[1] & 0xFFFF, a.l[2] & 0xFFFF, a.l[3] & 0xFFFF, a.l[4] & 0xFFFF, a.l[5] & 0xFFFF, a.l[6] & 0xFFFF, a.l[7] & 0xFFFF,
				a.pc, a.cntr, a.astat, a.sstat, a.mstat);

		}
		else if (strcmp(p, "?") == 0)
		{
			printf(
				"g [addr]    Go - resume execution, stopping at optional address\n"
				"t           Trace - single-step next opcode\n"
				"p           Procedure trace - step over next CALL\n"
				"pl          Procedure/loop trace - step over next CALL or DO UNTIL\n"
				"tl          Trace out of current loop\n"
				"tr          Trace out of current subroutine call (\"Step Out\")\n"
				"bp <addr>   Set a breakpoint at the given PM() address\n"
				"bl          List breakpoints\n"
				"bc <num>    Clear breakpoint (specified by breakpoint number)\n"
				"r           Show registers\n"
				"dm [type] [addr] [*count] Dump Data Memory at address; type is b (byte), w (word), or d (dword)\n"
				"pm [addr]   Dump Program Memroy (PM()) at address\n"
				"u [addr]    Unassemble instructions\n"
				"\n"
				"<Enter> repeats most commands (g, t, p, tl, tr, dm, pm, u), using the next\n"
				"address implicitly when an address is required.\n"
			);

		}
		else if (p[0] == 0)
		{
			// empty command with no repeat - do nothing
		}
		else
		{
			printf("Unknown command\n");
		}
	}
}

#else // defined(ADSP_DEBUGGER)
void adsp2100_init_debugger() { }
void adsp2100_debug_break() { }
#endif



/*###################################################################################################
**	CORE EXECUTION LOOP
**#################################################################################################*/

/* execute instructions on this CPU until icount expires */
int adsp2100_execute(int cycles)
{
	/* reset the core */
	set_mstat(adsp2100.mstat);

	/* count cycles and interrupt cycles */
	adsp2100_icount = cycles;
	adsp2100_icount -= adsp2100.interrupt_cycles;
	adsp2100.interrupt_cycles = 0;

	/* core execution loop */
	do
	{
		UINT32 op, temp;

		adsp2100.ppc = adsp2100.pc;	/* copy PC to previous PC */

#ifdef ADSP_DEBUGGER
		// if the debugger is enabled, check for a stop
		if (debugger_enabled)
			debugger();
#endif

		// instruction fetch
		op = ROPCODE();

		// debugging
		static uint32_t bp1 = 0xffff, bp2 = 0xffff, bp3 = 0xffff;
		if (adsp2100.ppc == bp1 || adsp2100.ppc == bp2 || adsp2100.ppc == bp3)
			bp1 = bp1;

		// advance to the next instruction, checking for a loop point first
		if (adsp2100.pc != adsp2100.loop)
		{
			adsp2100.pc++;
		}
		else if (CONDITION(adsp2100.loop_condition))
		{
			// looping, condition not met - keep looping
			adsp2100.pc = pc_stack_top();
		}
		else
		{
			// condition met; pop the PC and loop stacks and fall through
			loop_stack_pop();
			pc_stack_pop_val();
			adsp2100.pc++;
		}

		// parse the instruction
		switch ((op >> 16) & 0xff)
		{
		case 0x00:
			// 00000000 00000000 00000000  NOP
			break;

		case 0x01:
			// 00000001 xxxxxxxx xxxxxxxx  TRAP
			// Consume all remaining instructions and return control to the caller
			adsp2100_icount = 0;
			break;

		case 0x02:
			// 00000010 0000xxxx xxxxxxxx  modify flag out
			// 00000010 10000000 00000000  idle
			// 00000010 10000000 0000xxxx  idle (n)
			if (op & 0x008000)
			{
				adsp2100.idle = 1;
				adsp2100_icount = 0;
			}
			else
			{
				if (CONDITION(op & 15))
				{
					if (op & 0x020) adsp2100.flagout = 0;
					if (op & 0x010) adsp2100.flagout ^= 1;
					if (chip_type >= CHIP_TYPE_ADSP2101)
					{
						if (op & 0x080) adsp2100.fl0 = 0;
						if (op & 0x040) adsp2100.fl0 ^= 1;
						if (op & 0x200) adsp2100.fl1 = 0;
						if (op & 0x100) adsp2100.fl1 ^= 1;
						if (op & 0x800) adsp2100.fl2 = 0;
						if (op & 0x400) adsp2100.fl2 ^= 1;
					}
				}
			}
			break;
		case 0x03:
			// 00000011 xxxxxxxx xxxxxxxx  call or jump on flag in
			if (op & 0x000002)
			{
				if (adsp2100.flagin)
				{
					if (op & 0x000001)
						pc_stack_push();
					adsp2100.pc = ((op >> 4) & 0x0fff) | ((op << 10) & 0x3000);
				}
			}
			else
			{
				if (!adsp2100.flagin)
				{
					if (op & 0x000001)
						pc_stack_push();
					adsp2100.pc = ((op >> 4) & 0x0fff) | ((op << 10) & 0x3000);
				}
			}
			break;
		case 0x04:
			// 00000100 00000000 000xxxxx  stack control
			if (op & 0x000010) pc_stack_pop_val();
			if (op & 0x000008) loop_stack_pop();
			if (op & 0x000004) cntr_stack_pop();
			if (op & 0x000002)
			{
				if (op & 0x000001) stat_stack_pop();
				else stat_stack_push();
			}
			break;
		case 0x05:
			// 00000101 00000000 00000000  saturate MR
			if (GET_MV)
			{
				if (adsp2100.core.mr.mrx.mr2.u & 0x80)
					adsp2100.core.mr.mrx.mr2.u = 0xffff, adsp2100.core.mr.mrx.mr1.u = 0x8000, adsp2100.core.mr.mrx.mr0.u = 0x0000;
				else
					adsp2100.core.mr.mrx.mr2.u = 0x0000, adsp2100.core.mr.mrx.mr1.u = 0x7fff, adsp2100.core.mr.mrx.mr0.u = 0xffff;
			}
			break;
		case 0x06:
			// 00000110 000xxxxx 00000000  DIVS
		{
			int xop = (op >> 8) & 7;
			int yop = (op >> 11) & 3;

			xop = ALU_GETXREG_UNSIGNED(xop);
			yop = ALU_GETYREG_UNSIGNED(yop);

			temp = xop ^ yop;
			adsp2100.astat = (adsp2100.astat & ~QFLAG) | ((temp >> 10) & QFLAG);
			adsp2100.core.af.u = (yop << 1) | (adsp2100.core.ay0.u >> 15);
			adsp2100.core.ay0.u = (adsp2100.core.ay0.u << 1) | (temp >> 15);
		}
		break;
		case 0x07:
			// 00000111 00010xxx 00000000  DIVQ
		{
			int xop = (op >> 8) & 7;
			int res;

			xop = ALU_GETXREG_UNSIGNED(xop);

			if (GET_Q)
				res = adsp2100.core.af.u + xop;
			else
				res = adsp2100.core.af.u - xop;

			temp = res ^ xop;
			adsp2100.astat = (adsp2100.astat & ~QFLAG) | ((temp >> 10) & QFLAG);
			adsp2100.core.af.u = (res << 1) | (adsp2100.core.ay0.u >> 15);
			adsp2100.core.ay0.u = (adsp2100.core.ay0.u << 1) | ((~temp >> 15) & 0x0001);
		}
		break;
		case 0x08:
			// 00001000 00000000 0000xxxx  reserved
			break;
		case 0x09:
			// 00001001 00000000 000xxxxx  modify address register
			temp = (op >> 2) & 4;
			modify_address(temp + ((op >> 2) & 3), temp + (op & 3));
			break;
		case 0x0a:
			// 00001010 00000000 000xxxxx  conditional return
			if (CONDITION(op & 15))
			{
				pc_stack_pop();

				// RTI case
				if (op & 0x000010)
					stat_stack_pop();

				// if the program pointer is now invalid, this was a recursive
				// invocation of the interpreter from the host, so return to
				// the host
				if (adsp2100.pc == 0xFFFF)
					adsp2100_icount = 0;
			}
			break;
		case 0x0b:
			// 00001011 00000000 xxxxxxxx  conditional jump (indirect address)
			if (CONDITION(op & 15))
			{
				if (op & 0x000010)
					pc_stack_push();
				adsp2100.pc = adsp2100.i[4 + ((op >> 6) & 3)] & 0x3fff;
			}
			break;
		case 0x0c:
			// 00001100 xxxxxxxx xxxxxxxx  mode control
			temp = adsp2100.mstat;
			if (chip_type >= CHIP_TYPE_ADSP2101)
			{
				if (op & 0x000008) temp = (temp & ~MSTAT_GOMODE) | ((op << 5) & MSTAT_GOMODE);
				if (op & 0x002000) temp = (temp & ~MSTAT_INTEGER) | ((op >> 8) & MSTAT_INTEGER);
				if (op & 0x008000) temp = (temp & ~MSTAT_TIMER) | ((op >> 9) & MSTAT_TIMER);
			}
			if (op & 0x000020) temp = (temp & ~MSTAT_BANK) | ((op >> 4) & MSTAT_BANK);
			if (op & 0x000080) temp = (temp & ~MSTAT_REVERSE) | ((op >> 5) & MSTAT_REVERSE);
			if (op & 0x000200) temp = (temp & ~MSTAT_STICKYV) | ((op >> 6) & MSTAT_STICKYV);
			if (op & 0x000800) temp = (temp & ~MSTAT_SATURATE) | ((op >> 7) & MSTAT_SATURATE);
			set_mstat(temp);
			break;
		case 0x0d:
			// 00001101 0000xxxx xxxxxxxx  internal data move
			WRITE_REG((op >> 10) & 3, (op >> 4) & 15, READ_REG((op >> 8) & 3, op & 15));
			break;
		case 0x0e:
			// 00001110 0xxxxxxx xxxxxxxx  conditional shift
			if (CONDITION(op & 15)) shift_op(op);
			break;
		case 0x0f:
			// 00001111 0xxxxxxx xxxxxxxx  shift immediate
			shift_op_imm(op);
			break;
		case 0x10:
			// 00010000 0xxxxxxx xxxxxxxx  shift with internal data register move
			shift_op(op);
			temp = READ_REG(0, op & 15);
			WRITE_REG(0, (op >> 4) & 15, temp);
			break;
		case 0x11:
			// 00010001 xxxxxxxx xxxxxxxx  shift with pgm memory read/write
			if (op & 0x8000)
			{
				pgm_write_dag2(op, READ_REG(0, (op >> 4) & 15));
				shift_op(op);
			}
			else
			{
				shift_op(op);
				WRITE_REG(0, (op >> 4) & 15, pgm_read_dag2(op));
			}
			break;
		case 0x12:
			// 00010010 xxxxxxxx xxxxxxxx  shift with data memory read/write DAG1
			if (op & 0x8000)
			{
				data_write_dag1(op, READ_REG(0, (op >> 4) & 15));
				shift_op(op);
			}
			else
			{
				shift_op(op);
				WRITE_REG(0, (op >> 4) & 15, data_read_dag1(op));
			}
			break;
		case 0x13:
			// 00010011 xxxxxxxx xxxxxxxx  shift with data memory read/write DAG2
			if (op & 0x8000)
			{
				data_write_dag2(op, READ_REG(0, (op >> 4) & 15));
				shift_op(op);
			}
			else
			{
				shift_op(op);
				WRITE_REG(0, (op >> 4) & 15, data_read_dag2(op));
			}
			break;
		case 0x14: case 0x15: case 0x16: case 0x17:
			// 000101xx xxxxxxxx xxxxxxxx  do until
			loop_stack_push(op & 0x3ffff);
			pc_stack_push();
			break;
		case 0x18: case 0x19: case 0x1a: case 0x1b:
			// 000110xx xxxxxxxx xxxxxxxx  conditional jump (immediate addr)
			if (CONDITION(op & 15))
			{
				adsp2100.pc = (op >> 4) & 0x3fff;
				// check for a busy loop
				if (adsp2100.pc == adsp2100.ppc)
					adsp2100_icount = 0;
			}
			break;
		case 0x1c: case 0x1d: case 0x1e: case 0x1f:
			// 000111xx xxxxxxxx xxxxxxxx  conditional call (immediate addr)
			if (CONDITION(op & 15))
			{
				pc_stack_push();
				adsp2100.pc = (op >> 4) & 0x3fff;
			}
			break;
		case 0x20: case 0x21:
			// 0010000x xxxxxxxx xxxxxxxx  conditional MAC to MR
			if (CONDITION(op & 15)) mac_op_mr(op);
			break;
		case 0x22: case 0x23:
			// 0010001x xxxxxxxx xxxxxxxx  conditional ALU to AR
			if (CONDITION(op & 15)) alu_op_ar(op);
			break;
		case 0x24: case 0x25:
			// 0010010x xxxxxxxx xxxxxxxx  conditional MAC to MF
			if (CONDITION(op & 15)) mac_op_mf(op);
			break;
		case 0x26: case 0x27:
			// 0010011x xxxxxxxx xxxxxxxx  conditional ALU to AF
			if (CONDITION(op & 15)) alu_op_af(op);
			break;
		case 0x28: case 0x29:
			// 0010100x xxxxxxxx xxxxxxxx  MAC to MR with internal data register move
			temp = READ_REG(0, op & 15);
			mac_op_mr(op);
			WRITE_REG(0, (op >> 4) & 15, temp);
			break;
		case 0x2a: case 0x2b:
			// 0010101x xxxxxxxx xxxxxxxx  ALU to AR with internal data register move
			temp = READ_REG(0, op & 15);
			alu_op_ar(op);
			WRITE_REG(0, (op >> 4) & 15, temp);
			break;
		case 0x2c: case 0x2d:
			// 0010110x xxxxxxxx xxxxxxxx  MAC to MF with internal data register move
			temp = READ_REG(0, op & 15);
			mac_op_mf(op);
			WRITE_REG(0, (op >> 4) & 15, temp);
			break;
		case 0x2e: case 0x2f:
			// 0010111x xxxxxxxx xxxxxxxx  ALU to AF with internal data register move
			temp = READ_REG(0, op & 15);
			alu_op_af(op);
			WRITE_REG(0, (op >> 4) & 15, temp);
			break;
		case 0x30: case 0x31: case 0x32: case 0x33:
			// 001100xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 0)
			WRITE_REG(0, op & 15, (INT32)(op << 14) >> 18);
			break;
		case 0x34: case 0x35: case 0x36: case 0x37:
			// 001101xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 1)
			WRITE_REG(1, op & 15, (INT32)(op << 14) >> 18);
			break;
		case 0x38: case 0x39: case 0x3a: case 0x3b:
			// 001110xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 2)
			WRITE_REG(2, op & 15, (INT32)(op << 14) >> 18);
			break;
		case 0x3c: case 0x3d: case 0x3e: case 0x3f:
			// 001111xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 3)
			WRITE_REG(3, op & 15, (INT32)(op << 14) >> 18);
			break;
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
		case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
			// 0100xxxx xxxxxxxx xxxxxxxx  load data register immediate
			WRITE_REG(0, op & 15, (op >> 4) & 0xffff);
			break;
		case 0x50: case 0x51:
			// 0101000x xxxxxxxx xxxxxxxx  MAC to MR with pgm memory read
			mac_op_mr(op);
			WRITE_REG(0, (op >> 4) & 15, pgm_read_dag2(op));
			break;
		case 0x52: case 0x53:
			// 0101001x xxxxxxxx xxxxxxxx  ALU to AR with pgm memory read
			alu_op_ar(op);
			WRITE_REG(0, (op >> 4) & 15, pgm_read_dag2(op));
			break;
		case 0x54: case 0x55:
			// 0101010x xxxxxxxx xxxxxxxx  MAC to MF with pgm memory read
			mac_op_mf(op);
			WRITE_REG(0, (op >> 4) & 15, pgm_read_dag2(op));
			break;
		case 0x56: case 0x57:
			// 0101011x xxxxxxxx xxxxxxxx  ALU to AF with pgm memory read
			alu_op_af(op);
			WRITE_REG(0, (op >> 4) & 15, pgm_read_dag2(op));
			break;
		case 0x58: case 0x59:
			// 0101100x xxxxxxxx xxxxxxxx  MAC to MR with pgm memory write
			pgm_write_dag2(op, READ_REG(0, (op >> 4) & 15));
			mac_op_mr(op);
			break;
		case 0x5a: case 0x5b:
			// 0101101x xxxxxxxx xxxxxxxx  ALU to AR with pgm memory write
			pgm_write_dag2(op, READ_REG(0, (op >> 4) & 15));
			alu_op_ar(op);
			break;
		case 0x5c: case 0x5d:
			// 0101110x xxxxxxxx xxxxxxxx  ALU to MR with pgm memory write
			pgm_write_dag2(op, READ_REG(0, (op >> 4) & 15));
			mac_op_mf(op);
			break;
		case 0x5e: case 0x5f:
			// 0101111x xxxxxxxx xxxxxxxx  ALU to MF with pgm memory write
			pgm_write_dag2(op, READ_REG(0, (op >> 4) & 15));
			alu_op_af(op);
			break;
		case 0x60: case 0x61:
			// 0110000x xxxxxxxx xxxxxxxx  MAC to MR with data memory read DAG1
			mac_op_mr(op);
			WRITE_REG(0, (op >> 4) & 15, data_read_dag1(op));
			break;
		case 0x62: case 0x63:
			// 0110001x xxxxxxxx xxxxxxxx  ALU to AR with data memory read DAG1
			alu_op_ar(op);
			WRITE_REG(0, (op >> 4) & 15, data_read_dag1(op));
			break;
		case 0x64: case 0x65:
			// 0110010x xxxxxxxx xxxxxxxx  MAC to MF with data memory read DAG1
			mac_op_mf(op);
			WRITE_REG(0, (op >> 4) & 15, data_read_dag1(op));
			break;
		case 0x66: case 0x67:
			// 0110011x xxxxxxxx xxxxxxxx  ALU to AF with data memory read DAG1
			alu_op_af(op);
			WRITE_REG(0, (op >> 4) & 15, data_read_dag1(op));
			break;
		case 0x68: case 0x69:
			// 0110100x xxxxxxxx xxxxxxxx  MAC to MR with data memory write DAG1
			data_write_dag1(op, READ_REG(0, (op >> 4) & 15));
			mac_op_mr(op);
			break;
		case 0x6a: case 0x6b:
			// 0110101x xxxxxxxx xxxxxxxx  ALU to AR with data memory write DAG1
			data_write_dag1(op, READ_REG(0, (op >> 4) & 15));
			alu_op_ar(op);
			break;
		case 0x6c: case 0x6d:
			// 0111110x xxxxxxxx xxxxxxxx  MAC to MF with data memory write DAG1
			data_write_dag1(op, READ_REG(0, (op >> 4) & 15));
			mac_op_mf(op);
			break;
		case 0x6e: case 0x6f:
			// 0111111x xxxxxxxx xxxxxxxx  ALU to AF with data memory write DAG1
			data_write_dag1(op, READ_REG(0, (op >> 4) & 15));
			alu_op_af(op);
			break;
		case 0x70: case 0x71:
			// 0111000x xxxxxxxx xxxxxxxx  MAC to MR with data memory read DAG2
			mac_op_mr(op);
			WRITE_REG(0, (op >> 4) & 15, data_read_dag2(op));
			break;
		case 0x72: case 0x73:
			// 0111001x xxxxxxxx xxxxxxxx  ALU to AR with data memory read DAG2
			alu_op_ar(op);
			WRITE_REG(0, (op >> 4) & 15, data_read_dag2(op));
			break;
		case 0x74: case 0x75:
			// 0111010x xxxxxxxx xxxxxxxx  MAC to MF with data memory read DAG2
			mac_op_mf(op);
			WRITE_REG(0, (op >> 4) & 15, data_read_dag2(op));
			break;
		case 0x76: case 0x77:
			// 0111011x xxxxxxxx xxxxxxxx  ALU to AF with data memory read DAG2
			alu_op_af(op);
			WRITE_REG(0, (op >> 4) & 15, data_read_dag2(op));
			break;
		case 0x78: case 0x79:
			// 0111100x xxxxxxxx xxxxxxxx  MAC to MR with data memory write DAG2
			data_write_dag2(op, READ_REG(0, (op >> 4) & 15));
			mac_op_mr(op);
			break;
		case 0x7a: case 0x7b:
			// 0111101x xxxxxxxx xxxxxxxx  ALU to AR with data memory write DAG2
			data_write_dag2(op, READ_REG(0, (op >> 4) & 15));
			alu_op_ar(op);
			break;
		case 0x7c: case 0x7d:
			// 0111110x xxxxxxxx xxxxxxxx  MAC to MF with data memory write DAG2
			data_write_dag2(op, READ_REG(0, (op >> 4) & 15));
			mac_op_mf(op);
			break;
		case 0x7e: case 0x7f:
			// 0111111x xxxxxxxx xxxxxxxx  ALU to AF with data memory write DAG2
			data_write_dag2(op, READ_REG(0, (op >> 4) & 15));
			alu_op_af(op);
			break;
		case 0x80: case 0x81: case 0x82: case 0x83:
			// 100000xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 0
			WRITE_REG(0, op & 15, RWORD_DATA((op >> 4) & 0x3fff));
			break;
		case 0x84: case 0x85: case 0x86: case 0x87:
			// 100001xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 1
			WRITE_REG(1, op & 15, RWORD_DATA((op >> 4) & 0x3fff));
			break;
		case 0x88: case 0x89: case 0x8a: case 0x8b:
			// 100010xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 2
			WRITE_REG(2, op & 15, RWORD_DATA((op >> 4) & 0x3fff));
			break;
		case 0x8c: case 0x8d: case 0x8e: case 0x8f:
			// 100011xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 3
			WRITE_REG(3, op & 15, RWORD_DATA((op >> 4) & 0x3fff));
			break;
		case 0x90: case 0x91: case 0x92: case 0x93:
			// 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 0
			WWORD_DATA((op >> 4) & 0x3fff, READ_REG(0, op & 15));
			break;
		case 0x94: case 0x95: case 0x96: case 0x97:
			// 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 1
			WWORD_DATA((op >> 4) & 0x3fff, READ_REG(1, op & 15));
			break;
		case 0x98: case 0x99: case 0x9a: case 0x9b:
			// 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 2
			WWORD_DATA((op >> 4) & 0x3fff, READ_REG(2, op & 15));
			break;
		case 0x9c: case 0x9d: case 0x9e: case 0x9f:
			// 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 3
			WWORD_DATA((op >> 4) & 0x3fff, READ_REG(3, op & 15));
			break;
		case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
		case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
			// 1010xxxx xxxxxxxx xxxxxxxx  data memory write (immediate) DAG1
			data_write_dag1(op, (op >> 4) & 0xffff);
			break;
		case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
		case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
			// 1011xxxx xxxxxxxx xxxxxxxx  data memory write (immediate) DAG2
			data_write_dag2(op, (op >> 4) & 0xffff);
			break;
		case 0xc0: case 0xc1:
			// 1100000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to AY0
			mac_op_mr(op);
			adsp2100.core.ax0.u = data_read_dag1(op);
			adsp2100.core.ay0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xc2: case 0xc3:
			// 1100001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to AY0
			alu_op_ar(op);
			adsp2100.core.ax0.u = data_read_dag1(op);
			adsp2100.core.ay0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xc4: case 0xc5:
			// 1100010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to AY0
			mac_op_mr(op);
			adsp2100.core.ax1.u = data_read_dag1(op);
			adsp2100.core.ay0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xc6: case 0xc7:
			// 1100011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to AY0
			alu_op_ar(op);
			adsp2100.core.ax1.u = data_read_dag1(op);
			adsp2100.core.ay0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xc8: case 0xc9:
			// 1100100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to AY0
			mac_op_mr(op);
			adsp2100.core.mx0.u = data_read_dag1(op);
			adsp2100.core.ay0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xca: case 0xcb:
			// 1100101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to AY0
			alu_op_ar(op);
			adsp2100.core.mx0.u = data_read_dag1(op);
			adsp2100.core.ay0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xcc: case 0xcd:
			// 1100110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to AY0
			mac_op_mr(op);
			adsp2100.core.mx1.u = data_read_dag1(op);
			adsp2100.core.ay0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xce: case 0xcf:
			// 1100111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to AY0
			alu_op_ar(op);
			adsp2100.core.mx1.u = data_read_dag1(op);
			adsp2100.core.ay0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xd0: case 0xd1:
			// 1101000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to AY1
			mac_op_mr(op);
			adsp2100.core.ax0.u = data_read_dag1(op);
			adsp2100.core.ay1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xd2: case 0xd3:
			// 1101001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to AY1
			alu_op_ar(op);
			adsp2100.core.ax0.u = data_read_dag1(op);
			adsp2100.core.ay1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xd4: case 0xd5:
			// 1101010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to AY1
			mac_op_mr(op);
			adsp2100.core.ax1.u = data_read_dag1(op);
			adsp2100.core.ay1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xd6: case 0xd7:
			// 1101011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to AY1
			alu_op_ar(op);
			adsp2100.core.ax1.u = data_read_dag1(op);
			adsp2100.core.ay1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xd8: case 0xd9:
			// 1101100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to AY1
			mac_op_mr(op);
			adsp2100.core.mx0.u = data_read_dag1(op);
			adsp2100.core.ay1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xda: case 0xdb:
			// 1101101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to AY1
			alu_op_ar(op);
			adsp2100.core.mx0.u = data_read_dag1(op);
			adsp2100.core.ay1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xdc: case 0xdd:
			// 1101110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to AY1
			mac_op_mr(op);
			adsp2100.core.mx1.u = data_read_dag1(op);
			adsp2100.core.ay1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xde: case 0xdf:
			// 1101111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to AY1
			alu_op_ar(op);
			adsp2100.core.mx1.u = data_read_dag1(op);
			adsp2100.core.ay1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xe0: case 0xe1:
			// 1110000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to MY0
			mac_op_mr(op);
			adsp2100.core.ax0.u = data_read_dag1(op);
			adsp2100.core.my0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xe2: case 0xe3:
			// 1110001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to MY0
			alu_op_ar(op);
			adsp2100.core.ax0.u = data_read_dag1(op);
			adsp2100.core.my0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xe4: case 0xe5:
			// 1110010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to MY0
			mac_op_mr(op);
			adsp2100.core.ax1.u = data_read_dag1(op);
			adsp2100.core.my0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xe6: case 0xe7:
			// 1110011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to MY0
			alu_op_ar(op);
			adsp2100.core.ax1.u = data_read_dag1(op);
			adsp2100.core.my0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xe8: case 0xe9:
			// 1110100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to MY0
			mac_op_mr(op);
			adsp2100.core.mx0.u = data_read_dag1(op);
			adsp2100.core.my0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xea: case 0xeb:
			// 1110101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to MY0
			alu_op_ar(op);
			adsp2100.core.mx0.u = data_read_dag1(op);
			adsp2100.core.my0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xec: case 0xed:
			// 1110110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to MY0
			mac_op_mr(op);
			adsp2100.core.mx1.u = data_read_dag1(op);
			adsp2100.core.my0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xee: case 0xef:
			// 1110111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to MY0
			alu_op_ar(op);
			adsp2100.core.mx1.u = data_read_dag1(op);
			adsp2100.core.my0.u = pgm_read_dag2(op >> 4);
			break;
		case 0xf0: case 0xf1:
			// 1111000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to MY1
			mac_op_mr(op);
			adsp2100.core.ax0.u = data_read_dag1(op);
			adsp2100.core.my1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xf2: case 0xf3:
			// 1111001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to MY1
			alu_op_ar(op);
			adsp2100.core.ax0.u = data_read_dag1(op);
			adsp2100.core.my1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xf4: case 0xf5:
			// 1111010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to MY1
			mac_op_mr(op);
			adsp2100.core.ax1.u = data_read_dag1(op);
			adsp2100.core.my1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xf6: case 0xf7:
			// 1111011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to MY1
			alu_op_ar(op);
			adsp2100.core.ax1.u = data_read_dag1(op);
			adsp2100.core.my1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xf8: case 0xf9:
			// 1111100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to MY1
			mac_op_mr(op);
			adsp2100.core.mx0.u = data_read_dag1(op);
			adsp2100.core.my1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xfa: case 0xfb:
			// 1111101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to MY1
			alu_op_ar(op);
			adsp2100.core.mx0.u = data_read_dag1(op);
			adsp2100.core.my1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xfc: case 0xfd:
			// 1111110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to MY1
			mac_op_mr(op);
			adsp2100.core.mx1.u = data_read_dag1(op);
			adsp2100.core.my1.u = pgm_read_dag2(op >> 4);
			break;
		case 0xfe: case 0xff:
			// 1111111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to MY1
			alu_op_ar(op);
			adsp2100.core.mx1.u = data_read_dag1(op);
			adsp2100.core.my1.u = pgm_read_dag2(op >> 4);
			break;

		default:
			// invalid instruction
			break;
		}

		adsp2100_icount--;
	} while (adsp2100_icount > 0);

	adsp2100_icount -= adsp2100.interrupt_cycles;
	adsp2100.interrupt_cycles = 0;

	// The speedups execute lots of simulated cycles without interruption.  If we overshot,
	// only claim the number of cycles that we were asked to run.  Reporting an excess can
	// confuse the interrupt timing.  (adsp2100_icount is our budget of cycles remaining,
	// so a negative number means we consumed more than we were asked to.)
	if (adsp2100_icount < 0)
		adsp2100_icount = 0;

	// Return the number of cycles we executed, which we can figure as the original budget
	// we were given minus the number of cycles still remaining in our working budget.
	return cycles - adsp2100_icount;
}



// --------------------------------------------------------------------------
//
// ADSP-2102 subtype
//

void adsp2101_init(void) { adsp2100_init(); }

void adsp2101_reset(void *param)
{
	set_core_2101();
	adsp2100_reset(param);
}

void adsp2101_exit(void)
{
	adsp2100_exit();
	sport_rx_callback = 0;
	sport_tx_callback = 0;
}
int adsp2101_execute(int cycles) { return adsp2100_execute(cycles); }


// --------------------------------------------------------------------------
//
// ADSP-2105 subtype
//

void adsp2105_init(void) { adsp2100_init(); }

void adsp2105_reset(void *param)
{
	set_core_2105();
	adsp2100_reset(param);
}

void adsp2105_exit(void)
{
	adsp2100_exit();
	sport_rx_callback = 0;
	sport_tx_callback = 0;
}
int adsp2105_execute(int cycles) { return adsp2100_execute(cycles); }

void adsp2105_load_boot_data(const uint8_t *srcdata, uint32_t *dstdata)
{
	/* see how many words we need to copy */
#ifdef LSB_FIRST
	UINT32 size = 8 * (srcdata[3] + 1), i;
#else
	UINT32 size = 8 * (srcdata[2] + 1), i;
#endif
	for (i = 0; i < size; i++)
	{
#ifdef LSB_FIRST
		UINT32 opcode = (srcdata[i*4+0] << 16) | (srcdata[i*4+1] << 8) | srcdata[i*4+2];
#else
		UINT32 opcode = (srcdata[i*4+1] << 16) | (srcdata[i*4+0] << 8) | srcdata[i*4+3];
#endif
		ADSP2100_WRPGM(&dstdata[i], opcode);
	}
}
