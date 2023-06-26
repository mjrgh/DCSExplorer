#pragma once

#if !defined(LSB_FIRST) && !defined(MSB_FIRST)
#if _WIN32
#define LSB_FIRST
#else
#error "One of LSB_FIRST or MSB_FIRST must be #defined to specify platform byte order"
#endif
#elif defined(LSB_FIRST) && defined(MSB_FIRST)
#error "Only one of LSB_FIRST -or- MSB_FIRST can be #defined"
#endif

#include <stdint.h>
typedef uint64_t uint64_t;


/*###################################################################################################
**	SERIAL PORT CALLBACKS
**#################################################################################################*/

/* transmit and receive data callbacks types */
typedef int32_t (*RX_CALLBACK)(int port);
typedef void  (*TX_CALLBACK)(int port, int32_t data);


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
	uint16_t	u;
	int16_t	s;
} ADSPREG16;


/* the SHIFT result register is 32 bits */
typedef union
{
#ifdef LSB_FIRST
	struct { ADSPREG16 sr0, sr1; } srx;
#else
	struct { ADSPREG16 sr1, sr0; } srx;
#endif
	uint32_t sr;
} SHIFTRESULT;


/* the MAC result register is 40 bits */
typedef union
{
#ifdef LSB_FIRST
	struct { ADSPREG16 mr0, mr1, mr2, mrzero; } mrx;
	struct { uint32_t mr0, mr1; } mry;
#else
	struct { ADSPREG16 mrzero, mr2, mr1, mr0; } mrx;
	struct { UINT32 mr1, mr0; } mry;
#endif
	uint64_t mr;
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
	uint32_t		i[8];
	int32_t		m[8];
	uint32_t		l[8];
	uint32_t		lmask[8];
	uint32_t		base[8];
	uint8_t		px;

	/* other CPU registers */
	uint32_t		pc;
	uint32_t		ppc;
	uint32_t		loop;
	uint32_t		loop_condition;
	uint32_t		cntr;

	/* status registers */
	uint32_t		astat;
	uint32_t		sstat;
	uint32_t		mstat;
	uint32_t		astat_clear;
	uint32_t		idle;

	/* stacks */
	uint32_t		loop_stack[ADSP2100_LOOP_STACK_DEPTH];
	uint32_t		cntr_stack[ADSP2100_CNTR_STACK_DEPTH];
	uint32_t		pc_stack[ADSP2100_PC_STACK_DEPTH];
	uint8_t		stat_stack[ADSP2100_STAT_STACK_DEPTH][3];
	int32_t		pc_sp;
	int32_t		cntr_sp;
	int32_t		stat_sp;
	int32_t		loop_sp;

	/* external I/O */
	uint8_t		flagout;
	uint8_t		flagin;
	uint8_t		fl0;
	uint8_t		fl1;
	uint8_t		fl2;

	/* interrupt handling */
	uint8_t		imask;
	uint8_t		icntl;
	uint16_t		ifc;
	uint8_t		irq_state[5];
	uint8_t		irq_latch[5];
	int32_t		interrupt_cycles;
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
