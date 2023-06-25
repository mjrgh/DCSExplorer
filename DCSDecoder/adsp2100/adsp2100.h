// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    adsp2100.h

    ADSP-21xx series emulator.

***************************************************************************/

#include "adsp2100types.h"

/*###################################################################################################
**	PUBLIC GLOBALS
* 
**#################################################################################################*/

// instruction cycle execution counter, for metering processor time slices
extern int adsp2100_icount;

// get the CPU's register file
adsp2100_Regs& adsp2100_get_regs();



/*###################################################################################################
* **  EMULATED PROCESS MEMORY SPACE ACCESS
*
* These are externals, provided by the host system, to implement the processor's
* emulated memory space.
* 
**#################################################################################################*/

// CPU ROM - memory space provided by the client program
extern UINT32 *adsp2100_op_rom;

// These routines are special-cased for the DCSDecoderEmulator.  We redirect
// memory reads and writes to external handlers provided by the client program.
// Note that PM() read/write operations are only directed to the client for
// the special location PM($3000) - other addresses are handled directly via
// the oprom array.
extern UINT32 adsp2100_host_read_dm(UINT32 addr);
extern void adsp2100_host_write_dm(UINT32 addr, UINT32 data);
extern UINT32 adsp2100_host_read_pm(UINT32 addr);
extern void adsp2100_host_write_pm(UINT32 addr, UINT32 data);

/*###################################################################################################
**	PUBLIC FUNCTIONS
**#################################################################################################*/

extern void adsp2100_init(void);
extern void adsp2100_reset(void *param);
extern void adsp2100_exit(void);
extern int adsp2100_execute(int cycles);
extern void adsp2100_set_irq_line(int irqline, int state);
extern void adsp2100_set_irq_callback(int (*callback)(int irqline));
extern void adsp2100_host_invoke_irq(int which, int indx, int cycleLimit);

// external access to registers
void adsp2100_set_mstat(int val);
void adsp2100_set_Ix_reg(int x, INT32 val);

// debugger interface, if available
extern void adsp2100_init_debugger();
extern void adsp2100_debug_break();


/**************************************************************************
 * ADSP2101 section
 **************************************************************************/
#if (HAS_ADSP2101)

extern void adsp2101_init(void);
extern void adsp2101_reset(void *param);
extern void adsp2101_exit(void);
extern int adsp2101_execute(int cycles);    /* NS 970908 */
extern void adsp2101_set_irq_line(int irqline, int state);
extern void adsp2101_set_irq_callback(int (*callback)(int irqline));
extern void adsp2101_set_rx_callback(RX_CALLBACK cb);
extern void adsp2101_set_tx_callback(TX_CALLBACK cb);

#endif // HAS_ADSP2101

/**************************************************************************
 * ADSP2105 section
 **************************************************************************/
#if (HAS_ADSP2105)

extern void adsp2105_init(void);
extern void adsp2105_reset(void *param);
extern void adsp2105_exit(void);
extern int adsp2105_execute(int cycles);    /* NS 970908 */
extern void adsp2105_set_irq_line(int irqline, int state);
extern void adsp2105_set_irq_callback(int (*callback)(int irqline));
extern void adsp2105_set_rx_callback(RX_CALLBACK cb);
extern void adsp2105_set_tx_callback(TX_CALLBACK cb);

extern void adsp2105_load_boot_data(const uint8_t *srcdata, uint32_t *dstdata);

#endif // HAS_ADSP2105

