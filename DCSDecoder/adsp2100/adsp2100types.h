#pragma once

#if !defined(LSB_FIRST) && !defined(MSB_FIRST)
#ifdef WIN32
#define LSB_FIRST
#else
#error "One of LSB_FIRST or MSB_FIRST must be #defined to specify platform byte order"
#endif
#elif defined(LSB_FIRST) && defined(MSB_FIRST)
#error "Only one of LSB_FIRST -or- MSB_FIRST can be #defined"
#endif

// The ADSP emulator code uses Windows-style xINTx types for the
// definite-bit-width integer sizes.  Define these in terms of the
// portable stdint.h types.
#include <stdint.h>
typedef int8_t INT8;
typedef uint8_t UINT8;
typedef int16_t INT16;
typedef uint16_t UINT16;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef int64_t INT64;
typedef uint64_t UINT64;


/*###################################################################################################
**	SERIAL PORT CALLBACKS
**#################################################################################################*/

/* transmit and receive data callbacks types */
typedef INT32 (*RX_CALLBACK)(int port);
typedef void  (*TX_CALLBACK)(int port, INT32 data);


/*###################################################################################################
**	REGISTER ENUMERATION
**#################################################################################################*/

enum
{
	ADSP2100_PC=1,
	ADSP2100_AX0, ADSP2100_AX1, ADSP2100_AY0, ADSP2100_AY1, ADSP2100_AR, ADSP2100_AF,
	ADSP2100_MX0, ADSP2100_MX1, ADSP2100_MY0, ADSP2100_MY1, ADSP2100_MR0, ADSP2100_MR1, ADSP2100_MR2, ADSP2100_MF,
	ADSP2100_SI, ADSP2100_SE, ADSP2100_SB, ADSP2100_SR0, ADSP2100_SR1,
	ADSP2100_I0, ADSP2100_I1, ADSP2100_I2, ADSP2100_I3, ADSP2100_I4, ADSP2100_I5, ADSP2100_I6, ADSP2100_I7,
	ADSP2100_L0, ADSP2100_L1, ADSP2100_L2, ADSP2100_L3, ADSP2100_L4, ADSP2100_L5, ADSP2100_L6, ADSP2100_L7,
	ADSP2100_M0, ADSP2100_M1, ADSP2100_M2, ADSP2100_M3, ADSP2100_M4, ADSP2100_M5, ADSP2100_M6, ADSP2100_M7,
	ADSP2100_PX, ADSP2100_CNTR, ADSP2100_ASTAT, ADSP2100_SSTAT, ADSP2100_MSTAT,
	ADSP2100_PCSP, ADSP2100_CNTRSP, ADSP2100_STATSP, ADSP2100_LOOPSP,
	ADSP2100_IMASK, ADSP2100_ICNTL, ADSP2100_IRQSTATE0, ADSP2100_IRQSTATE1, ADSP2100_IRQSTATE2, ADSP2100_IRQSTATE3,
	ADSP2100_FLAGIN, ADSP2100_FLAGOUT, ADSP2100_FL0, ADSP2100_FL1, ADSP2100_FL2,
	ADSP2100_AX0_SEC, ADSP2100_AX1_SEC, ADSP2100_AY0_SEC, ADSP2100_AY1_SEC, ADSP2100_AR_SEC, ADSP2100_AF_SEC,
	ADSP2100_MX0_SEC, ADSP2100_MX1_SEC, ADSP2100_MY0_SEC, ADSP2100_MY1_SEC, ADSP2100_MR0_SEC, ADSP2100_MR1_SEC, ADSP2100_MR2_SEC, ADSP2100_MF_SEC,
	ADSP2100_SI_SEC, ADSP2100_SE_SEC, ADSP2100_SB_SEC, ADSP2100_SR0_SEC, ADSP2100_SR1_SEC
};


/*###################################################################################################
**	INTERRUPT CONSTANTS
**#################################################################################################*/

#define ADSP2100_IRQ0		0		/* IRQ0 */
#define ADSP2100_SPORT1_RX	0		/* SPORT1 receive IRQ */
#define ADSP2100_IRQ1		1		/* IRQ1 */
#define ADSP2100_SPORT1_TX	1		/* SPORT1 transmit IRQ */
#define ADSP2100_IRQ2		2		/* IRQ2 */
#define ADSP2100_IRQ3		3		/* IRQ3 */


/*###################################################################################################
**  STRUCTURES & TYPEDEFS
**#################################################################################################*/

/* stack depths */
#define	ADSP2100_PC_STACK_DEPTH		16
#define ADSP2100_CNTR_STACK_DEPTH	4
#define ADSP2100_STAT_STACK_DEPTH	4
#define ADSP2100_LOOP_STACK_DEPTH	4


/* 16-bit registers that can be loaded signed or unsigned */
typedef union
{
	UINT16	u;
	INT16	s;
} ADSPREG16;


/* the SHIFT result register is 32 bits */
typedef union
{
#ifdef LSB_FIRST
	struct { ADSPREG16 sr0, sr1; } srx;
#else
	struct { ADSPREG16 sr1, sr0; } srx;
#endif
	UINT32 sr;
} SHIFTRESULT;


/* the MAC result register is 40 bits */
typedef union
{
#ifdef LSB_FIRST
	struct { ADSPREG16 mr0, mr1, mr2, mrzero; } mrx;
	struct { UINT32 mr0, mr1; } mry;
#else
	struct { ADSPREG16 mrzero, mr2, mr1, mr0; } mrx;
	struct { UINT32 mr1, mr0; } mry;
#endif
	UINT64 mr;
} MACRESULT;

/* there are two banks of "core" registers */
typedef struct ADSPCORE
{
	/* ALU registers */
	ADSPREG16	ax0, ax1;
	ADSPREG16	ay0, ay1;
	ADSPREG16	ar;
	ADSPREG16	af;

	/* MAC registers */
	ADSPREG16	mx0, mx1;
	ADSPREG16	my0, my1;
	MACRESULT	mr;
	ADSPREG16	mf;

	/* SHIFT registers */
	ADSPREG16	si;
	ADSPREG16	se;
	ADSPREG16	sb;
	SHIFTRESULT	sr;

	/* dummy registers */
	ADSPREG16	zero;
} ADSPCORE;


/* ADSP-2100 Registers */
typedef struct
{
	/* Core registers, 2 banks */
	ADSPCORE	core;
	ADSPCORE	alt;

	/* Memory addressing registers */
	UINT32		i[8];
	INT32		m[8];
	UINT32		l[8];
	UINT32		lmask[8];
	UINT32		base[8];
	UINT8		px;

	/* other CPU registers */
	UINT32		pc;
	UINT32		ppc;
	UINT32		loop;
	UINT32		loop_condition;
	UINT32		cntr;

	/* status registers */
	UINT32		astat;
	UINT32		sstat;
	UINT32		mstat;
	UINT32		astat_clear;
	UINT32		idle;

	/* stacks */
	UINT32		loop_stack[ADSP2100_LOOP_STACK_DEPTH];
	UINT32		cntr_stack[ADSP2100_CNTR_STACK_DEPTH];
	UINT32		pc_stack[ADSP2100_PC_STACK_DEPTH];
	UINT8		stat_stack[ADSP2100_STAT_STACK_DEPTH][3];
	INT32		pc_sp;
	INT32		cntr_sp;
	INT32		stat_sp;
	INT32		loop_sp;

	/* external I/O */
	UINT8		flagout;
	UINT8		flagin;
	UINT8		fl0;
	UINT8		fl1;
	UINT8		fl2;

	/* interrupt handling */
	UINT8		imask;
	UINT8		icntl;
	UINT16		ifc;
	UINT8		irq_state[5];
	UINT8		irq_latch[5];
	INT32		interrupt_cycles;
	int			(*irq_callback)(int irqline);
} adsp2100_Regs;


/*###################################################################################################
**  ADSP-21xx FAMILY SUBTYPE SPECIALIZATIONS
**#################################################################################################*/

/**************************************************************************
 * ADSP2101 section
 **************************************************************************/
#if (HAS_ADSP2101)

#define ADSP2101_IRQ0		0		/* IRQ0 */
#define ADSP2101_SPORT1_RX	0		/* SPORT1 receive IRQ */
#define ADSP2101_IRQ1		1		/* IRQ1 */
#define ADSP2101_SPORT1_TX	1		/* SPORT1 transmit IRQ */
#define ADSP2101_IRQ2		2		/* IRQ2 */
#define ADSP2101_SPORT0_RX	3		/* SPORT0 receive IRQ */
#define ADSP2101_SPORT0_TX	4		/* SPORT0 transmit IRQ */

#endif // HAS_ADSP2101



/**************************************************************************
 * ADSP2105 section
 **************************************************************************/
#if (HAS_ADSP2105)

#define ADSP2105_IRQ0		0		/* IRQ0 */
#define ADSP2105_SPORT1_RX	0		/* SPORT1 receive IRQ */
#define ADSP2105_IRQ1		1		/* IRQ1 */
#define ADSP2105_SPORT1_TX	1		/* SPORT1 transmit IRQ */
#define ADSP2105_IRQ2		2		/* IRQ2 */

#endif // HAS_ADSP2105
