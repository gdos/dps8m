/*
 Copyright (c) 2007-2013 Michael Mondy
 Copyright 2012-2016 by Harry Reed
 Copyright 2013-2016 by Charles Anthony
 Copyright 2017 by Michal Tomek

 All rights reserved.

 This software is made available under the terms of the
 ICU License -- ICU 1.8.1 and later.
 See the LICENSE file at the top-level directory of this distribution and
 at https://sourceforge.net/p/dps8m/code/ci/master/tree/LICENSE
 */

 /** * \file dps8_cpu.c
 * \project dps8
 * \date 9/17/12
 * \copyright Copyright (c) 2012 Harry Reed. All rights reserved.
*/

#include <stdio.h>
#include <unistd.h>

#include "dps8.h"
#include "dps8_addrmods.h"
#include "dps8_sys.h"
#include "dps8_faults.h"
#include "dps8_cpu.h"
#include "dps8_append.h"
#include "dps8_ins.h"
#include "dps8_loader.h"
#include "dps8_math.h"
#include "dps8_scu.h"
#include "dps8_utils.h"
#include "dps8_iefp.h"
#include "dps8_console.h"
#ifdef FNP2
#include "dps8_fnp2.h"
#else
#include "dps8_fnp.h"
#endif
#include "dps8_iom.h"
#include "dps8_cable.h"
#include "dps8_crdrdr.h"
#include "dps8_absi.h"
#ifdef M_SHARED
#include "shm.h"
#endif
#ifdef HDBG
#include "hdbg.h"
#endif
#include "dps8_opcodetable.h"

#ifdef FNP2
#else
#include "fnp_defs.h"
#include "fnp_cmds.h"
#endif

#include "sim_defs.h"

// XXX Use this when we assume there is only a single cpu unit
#define ASSUME0 0

static void cpu_init_array (void);
static bool clear_TEMPORARY_ABSOLUTE_mode (void);
static void set_TEMPORARY_ABSOLUTE_mode (void);
static void setCpuCycle (cycles_t cycle);

// CPU data structures

// The DPS8M had only 4 ports

static UNIT cpu_unit [N_CPU_UNITS_MAX] =
  {
    { UDATA (NULL, UNIT_FIX|UNIT_BINK, MEMSIZE), 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL },
    { UDATA (NULL, UNIT_FIX|UNIT_BINK, MEMSIZE), 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL },
    { UDATA (NULL, UNIT_FIX|UNIT_BINK, MEMSIZE), 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL },
    { UDATA (NULL, UNIT_FIX|UNIT_BINK, MEMSIZE), 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL },
    { UDATA (NULL, UNIT_FIX|UNIT_BINK, MEMSIZE), 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL },
    { UDATA (NULL, UNIT_FIX|UNIT_BINK, MEMSIZE), 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL },
    { UDATA (NULL, UNIT_FIX|UNIT_BINK, MEMSIZE), 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL },
    { UDATA (NULL, UNIT_FIX|UNIT_BINK, MEMSIZE), 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL }
  };
#define UNIT_NUM(uptr) ((uptr) - cpu_unit)

static t_stat cpu_show_config(FILE *st, UNIT *uptr, int val, const void *desc);
static t_stat cpu_set_config (UNIT * uptr, int32 value, const char * cptr, void * desc);
static t_stat cpu_set_initialize_and_clear (UNIT * uptr, int32 value, const char * cptr, void * desc);
#ifndef SPEED
static int cpu_show_stack(FILE *st, UNIT *uptr, int val, const void *desc);
#endif
static t_stat cpu_show_nunits(FILE *st, UNIT *uptr, int val, const void *desc);
static t_stat cpu_set_nunits (UNIT * uptr, int32 value, const char * cptr, void * desc);

#ifdef EV_POLL
static uv_loop_t * ev_poll_loop;
static uv_timer_t ev_poll_handle;
#endif

static MTAB cpu_mod[] = {
    {
#ifdef ROUND_ROBIN
      MTAB_XTD | MTAB_VUN | MTAB_VDV | MTAB_NMO | MTAB_VALR, /* mask */
#else
      MTAB_XTD | MTAB_VDV | MTAB_NMO /* | MTAB_VALR */, /* mask */
#endif
      0,            /* match */
      "CONFIG",     /* print string */
      "CONFIG",         /* match string */
      cpu_set_config,         /* validation routine */
      cpu_show_config, /* display routine */
      NULL,          /* value descriptor */
      NULL // help
    },
    {
#ifdef ROUND_ROBIN
      MTAB_XTD | MTAB_VUN | MTAB_VDV | MTAB_NMO | MTAB_VALR, /* mask */
#else
      MTAB_XTD | MTAB_VDV | MTAB_NMO /* | MTAB_VALR */, /* mask */
#endif
      0,            /* match */
      "INITIALIZEANDCLEAR",     /* print string */
      "INITIALIZEANDCLEAR",         /* match string */
      cpu_set_initialize_and_clear,         /* validation routine */
      NULL, /* display routine */
      NULL,          /* value descriptor */
      NULL // help
    },
    {
      MTAB_XTD | MTAB_VDV | MTAB_NMO | MTAB_VALR, /* mask */
      0,            /* match */
      "NUNITS",     /* print string */
      "NUNITS",         /* match string */
      cpu_set_nunits,         /* validation routine */
      cpu_show_nunits, /* display routine */
      NULL,          /* value descriptor */
      NULL // help
    },
#ifndef SPEED
    {
#ifdef ROUND_ROBIN
      MTAB_XTD | MTAB_VUN | MTAB_VDV | MTAB_NMO | MTAB_VALR, /* mask */
#else
      MTAB_XTD | MTAB_VDV | MTAB_NMO /* | MTAB_VALR */, /* mask */
#endif
      0, "STACK", NULL,
      NULL, cpu_show_stack, NULL, NULL
    },
#endif
    { 0, 0, NULL, NULL, NULL, NULL, NULL, NULL }
};

static DEBTAB cpu_dt[] = {
    { "TRACE",      DBG_TRACE, NULL       },
    { "TRACEEXT",   DBG_TRACEEXT, NULL    },
    { "MESSAGES",   DBG_MSG, NULL         },

    { "REGDUMPAQI", DBG_REGDUMPAQI, NULL  },
    { "REGDUMPIDX", DBG_REGDUMPIDX, NULL  },
    { "REGDUMPPR",  DBG_REGDUMPPR, NULL   },
    { "REGDUMPPPR", DBG_REGDUMPPPR, NULL  },
    { "REGDUMPDSBR",DBG_REGDUMPDSBR, NULL },
    { "REGDUMPFLT", DBG_REGDUMPFLT, NULL  },
    { "REGDUMP",    DBG_REGDUMP, NULL     }, // don't move as it messes up DBG message

    { "ADDRMOD",    DBG_ADDRMOD, NULL     },
    { "APPENDING",  DBG_APPENDING, NULL   },

    { "NOTIFY",     DBG_NOTIFY, NULL      },
    { "INFO",       DBG_INFO, NULL        },
    { "ERR",        DBG_ERR, NULL         },
    { "WARN",       DBG_WARN, NULL        },
    { "DEBUG",      DBG_DEBUG, NULL       },
    { "ALL",        DBG_ALL, NULL         }, // don't move as it messes up DBG message

    { "FAULT",      DBG_FAULT, NULL       },
    { "INTR",       DBG_INTR, NULL        },
    { "CORE",       DBG_CORE, NULL        },
    { "CYCLE",      DBG_CYCLE, NULL       },
    { "CAC",        DBG_CAC, NULL         },
    { "FINAL",      DBG_FINAL, NULL       },
    { NULL,         0, NULL               }
};

// This is part of the simh interface
const char *sim_stop_messages[] = {
    "Unknown error",           // SCPE_OK
    "Unimplemented Opcode",    // STOP_UNIMP
    "DIS instruction",         // STOP_DIS
    "Breakpoint",              // STOP_BKPT
    "BUG",                     // STOP_BUG
    "Fault cascade",           // STOP_FLT_CASCADE
    "Halt",                    // STOP_HALT
    "Illegal Opcode",          // STOP_ILLOP
    "Simulation stop",         // STOP_STOP
};

/* End of simh interface */

/* Processor configuration switches 
 *
 * From AM81-04 Multics System Maintainance Procedures
 *
 * "A level 68 IOM system may contain a maximum of 7 CPUs, 4 IOMs, 8 SCUs and 
 * 16MW of memory
 * [CAC]: but AN87 says multics only supports two IOMs
 * 
 * ASSIGNMENT: 3 toggle switches determine the base address of the SCU 
 * connected to the port. The base address (in KW) is the product of this 
 * number and the value defined by the STORE SIZE patch plug for the port.
 *
 * ADDRESS RANGE: toggle FULL/HALF. Determines the size of the SCU as full or 
 * half of the STORE SIZE patch.
 *
 * PORT ENABLE: (4? toggles)
 *
 * INITIALIZE ENABLE: (4? toggles) These switches enable the receipt of an 
 * initialize signal from the SCU connected to the ports. This signal is used
 * during the first part of bootload to set all CPUs to a known (idle) state.
 * The switch for each port connected to an SCU should be ON, otherwise off.
 *
 * INTERLACE: ... All INTERLACE switches should be OFF for Multics operation.
 *
 */

/*
 * init_opcodes()
 *
 * This initializes the is_eis[] array which we use to detect whether or
 * not an instruction is an EIS instruction.
 *
 * TODO: Change the array values to show how many operand words are
 * used.  This would allow for better symbolic disassembly.
 *
 * BUG: unimplemented instructions may not be represented
 */

static bool watchBits [MEMSIZE];

static int is_eis[1024];    // hack

// XXX PPR.IC oddly incremented. ticket #6

void init_opcodes (void)
  {
    memset(is_eis, 0, sizeof(is_eis));
    
#define IS_EIS(opc) is_eis [(opc << 1) | 1] = 1;
    IS_EIS (opcode1_cmpc);
    IS_EIS (opcode1_scd);
    IS_EIS (opcode1_scdr);
    IS_EIS (opcode1_scm);
    IS_EIS (opcode1_scmr);
    IS_EIS (opcode1_tct);
    IS_EIS (opcode1_tctr);
    IS_EIS (opcode1_mlr);
    IS_EIS (opcode1_mrl);
    IS_EIS (opcode1_mve);
    IS_EIS (opcode1_mvt);
    IS_EIS (opcode1_cmpn);
    IS_EIS (opcode1_mvn);
    IS_EIS (opcode1_mvne);
    IS_EIS (opcode1_csl);
    IS_EIS (opcode1_csr);
    IS_EIS (opcode1_cmpb);
    IS_EIS (opcode1_sztl);
    IS_EIS (opcode1_sztr);
    IS_EIS (opcode1_btd);
    IS_EIS (opcode1_dtb);
    IS_EIS (opcode1_dv3d);
  }


/*
 * initialize segment table according to the contents of DSBR ...
 */

static t_stat dpsCmd_InitUnpagedSegmentTable ()
  {
    if (cpu.DSBR.U == 0)
      {
        sim_printf  ("Cannot initialize unpaged segment table because DSBR.U says it is \"paged\"\n");
        return SCPE_OK;    // need a better return value
      }
    
    if (cpu.DSBR.ADDR == 0) // DSBR *probably* not initialized. Issue warning and ask....
      {
        if (! get_yn ("DSBR *probably* uninitialized (DSBR.ADDR == 0). Proceed anyway [N]?", FALSE))
          {
            return SCPE_OK;
          }
      }
    
    word15 segno = 0;
    while (2 * segno < (16 * (cpu.DSBR.BND + 1)))
      {
        //generate target segment SDW for DSBR.ADDR + 2 * segno.
        word24 a = cpu.DSBR.ADDR + 2u * segno;
        
        // just fill with 0's for now .....
        core_write ((a + 0) & PAMASK, 0, __func__);
        core_write ((a + 1) & PAMASK, 0, __func__);
        
        segno ++; // onto next segment SDW
      }
    
    if ( !sim_quiet)
      sim_printf("zero-initialized segments 0 .. %d\n", segno - 1);
    return SCPE_OK;
  }

#ifdef WAM
static t_stat dpsCmd_InitSDWAM ()
  {
#ifdef ROUND_ROBIN
    uint save = currentRunningCPUnum;
    for (uint i = 0; i < N_CPU_UNITS_MAX; i ++)
      {
        setCPUnum (i);
        memset (cpu.SDWAM, 0, sizeof (cpu.SDWAM));
      }
    setCPUnum (save);
#else
    memset (cpu.SDWAM, 0, sizeof (cpu.SDWAM));
#endif
    
    if (! sim_quiet)
      sim_printf ("zero-initialized SDWAM\n");
    return SCPE_OK;
  }
#endif

// Assumes unpaged DSBR

_sdw0 *fetchSDW (word15 segno)
  {
    word36 SDWeven, SDWodd;
    
    core_read2 ((cpu.DSBR.ADDR + 2u * segno) & PAMASK, & SDWeven, & SDWodd, __func__);
    
    // even word
    
    _sdw0 *SDW = &cpu._s;
    memset (SDW, 0, sizeof (cpu._s));
    
    SDW -> ADDR = (SDWeven >> 12) & 077777777;
    SDW -> R1 = (SDWeven >> 9) & 7;
    SDW -> R2 = (SDWeven >> 6) & 7;
    SDW -> R3 = (SDWeven >> 3) & 7;
    SDW -> DF = TSTBIT(SDWeven, 2);
    SDW -> FC = SDWeven & 3;
    
    // odd word
    SDW -> BOUND = (SDWodd >> 21) & 037777;
    SDW -> R = TSTBIT(SDWodd, 20);
    SDW -> E = TSTBIT(SDWodd, 19);
    SDW -> W = TSTBIT(SDWodd, 18);
    SDW -> P = TSTBIT(SDWodd, 17);
    SDW -> U = TSTBIT(SDWodd, 16);
    SDW -> G = TSTBIT(SDWodd, 15);
    SDW -> C = TSTBIT(SDWodd, 14);
    SDW -> EB = SDWodd & 037777;
    
    return SDW;
  }

static char * strDSBR (void)
  {
    static char buff [256];
    sprintf (buff, "DSBR: ADDR=%06o BND=%05o U=%o STACK=%04o", cpu.DSBR.ADDR, cpu.DSBR.BND, cpu.DSBR.U, cpu.DSBR.STACK);
    return buff;
  }

static void printDSBR (void)
  {
    sim_printf ("%s\n", strDSBR ());
  }


char * strSDW0 (_sdw0 * SDW)
  {
    static char buff [256];
    
    //if (SDW->ADDR == 0 && SDW->BOUND == 0) // need a better test
    //if (! SDW -> DF) 
    //  sprintf (buff, "*** Uninitialized ***");
    //else
      sprintf (buff, "ADDR=%06o R1=%o R2=%o R3=%o F=%o FC=%o BOUND=%o R=%o E=%o W=%o P=%o U=%o G=%o C=%o EB=%o",
               SDW -> ADDR, SDW -> R1, SDW -> R2, SDW -> R3, SDW -> DF,
               SDW -> FC, SDW -> BOUND, SDW -> R, SDW -> E, SDW -> W,
               SDW -> P, SDW -> U, SDW -> G, SDW -> C, SDW -> EB);
    return buff;
 }

static void printSDW0 (_sdw0 *SDW)
  {
    sim_printf ("%s\n", strSDW0 (SDW));
  }

static t_stat dpsCmd_DumpSegmentTable()
{
    sim_printf("*** Descriptor Segment Base Register (DSBR) ***\n");
    printDSBR();
    if (cpu.DSBR.U) {
        sim_printf("*** Descriptor Segment Table ***\n");
        for(word15 segno = 0; 2 * segno < 16 * (cpu.DSBR.BND + 1); segno += 1)
        {
            sim_printf("Seg %d - ", segno);
            _sdw0 *s = fetchSDW(segno);
            printSDW0(s);
        }
    } else {
        sim_printf("*** Descriptor Segment Table (Paged) ***\n");
        sim_printf("Descriptor segment pages\n");
        for(word15 segno = 0; 2 * segno < 16 * (cpu.DSBR.BND + 1); segno += 512)
        {
            word24 y1 = (2u * segno) % 1024u;
            word24 x1 = (2u * segno - y1) / 1024u;
            word36 PTWx1;
            core_read ((cpu.DSBR.ADDR + x1) & PAMASK, & PTWx1, __func__);

            _ptw0 PTW1;
            PTW1.ADDR = GETHI(PTWx1);
            PTW1.U = TSTBIT(PTWx1, 9);
            PTW1.M = TSTBIT(PTWx1, 6);
            PTW1.DF = TSTBIT(PTWx1, 2);
            PTW1.FC = PTWx1 & 3;
           
            //if (PTW1.DF == 0)
            //    continue;
            sim_printf ("%06o  Addr %06o U %o M %o F %o FC %o\n", 
                        segno, PTW1.ADDR, PTW1.U, PTW1.M, PTW1.DF, PTW1.FC);
            sim_printf ("    Target segment page table\n");
            for (word15 tspt = 0; tspt < 512; tspt ++)
            {
                word36 SDWeven, SDWodd;
                core_read2(((PTW1.ADDR << 6) + tspt * 2u) & PAMASK, & SDWeven, & SDWodd, __func__);
                _sdw0 SDW0;
                // even word
                SDW0.ADDR = (SDWeven >> 12) & PAMASK;
                SDW0.R1 = (SDWeven >> 9) & 7;
                SDW0.R2 = (SDWeven >> 6) & 7;
                SDW0.R3 = (SDWeven >> 3) & 7;
                SDW0.DF = TSTBIT(SDWeven, 2);
                SDW0.FC = SDWeven & 3;

                // odd word
                SDW0.BOUND = (SDWodd >> 21) & 037777;
                SDW0.R = TSTBIT(SDWodd, 20);
                SDW0.E = TSTBIT(SDWodd, 19);
                SDW0.W = TSTBIT(SDWodd, 18);
                SDW0.P = TSTBIT(SDWodd, 17);
                SDW0.U = TSTBIT(SDWodd, 16);
                SDW0.G = TSTBIT(SDWodd, 15);
                SDW0.C = TSTBIT(SDWodd, 14);
                SDW0.EB = SDWodd & 037777;

                //if (SDW0.DF == 0)
                //    continue;
                sim_printf ("    %06o Addr %06o %o,%o,%o F%o BOUND %06o %c%c%c%c%c\n",
                          tspt, SDW0.ADDR, SDW0.R1, SDW0.R2, SDW0.R3, SDW0.DF, SDW0.BOUND, SDW0.R ? 'R' : '.', SDW0.E ? 'E' : '.', SDW0.W ? 'W' : '.', SDW0.P ? 'P' : '.', SDW0.U ? 'U' : '.');
                //for (word18 offset = 0; ((offset >> 4) & 037777) <= SDW0.BOUND; offset += 1024)
                if (SDW0.U == 0)
                {
                    for (word18 offset = 0; offset < 16u * (SDW0.BOUND + 1u); offset += 1024u)
                    {
                        word24 y2 = offset % 1024;
                        word24 x2 = (offset - y2) / 1024;

                        // 10. Fetch the target segment PTW(x2) from SDW(segno).ADDR + x2.

                        word36 PTWx2;
                        core_read ((SDW0.ADDR + x2) & PAMASK, & PTWx2, __func__);

                        _ptw0 PTW_2;
                        PTW_2.ADDR = GETHI(PTWx2);
                        PTW_2.U = TSTBIT(PTWx2, 9);
                        PTW_2.M = TSTBIT(PTWx2, 6);
                        PTW_2.DF = TSTBIT(PTWx2, 2);
                        PTW_2.FC = PTWx2 & 3;

                         sim_printf ("        %06o  Addr %06o U %o M %o F %o FC %o\n", 
                                     offset, PTW_2.ADDR, PTW_2.U, PTW_2.M, PTW_2.DF, PTW_2.FC);

                      }
                  }
            }
        }
    }

    return SCPE_OK;
}

//! custom command "dump"
t_stat dpsCmd_Dump (UNUSED int32 arg, const char *buf)
{
    char cmds [256][256];
    memset(cmds, 0, sizeof(cmds));  // clear cmds buffer
    
    int nParams = sscanf(buf, "%s %s %s %s", cmds[0], cmds[1], cmds[2], cmds[3]);
    if (nParams == 2 && !strcasecmp(cmds[0], "segment") && !strcasecmp(cmds[1], "table"))
        return dpsCmd_DumpSegmentTable();
#ifdef WAM
    if (nParams == 1 && !strcasecmp(cmds[0], "sdwam"))
        return dumpSDWAM();
#endif
    
    return SCPE_OK;
}

// Doesn't work
#if 0
static word36 getKST (word24 offset)
  {
    word18 kst_seg = 067; // From system_book
    char * msg;
    word24 fa;
    if (dbgLookupAddress (kst_seg, offset, & fa, & msg))
      {
        sim_printf ("address xlate failed for %05o:%06o (%s)\n",
                    kst_seg, offset, msg);
        return 0llu;
      }
    return M [fa];
   }

// dcl 1 kst aligned based (kstp),                 /* KST header declaration */

// Word 0: 000000000230

//     2 lowseg fixed bin (17),                            /* lowest segment number described by kst */

// Word 1: 000000007775

//     2 highseg fixed bin (17),                           /* highest segment number described by kst */
//     2 highest_used_segno fixed bin (17),                /* highest segment number yet used  */
//     2 lvs fixed bin (8),                                /* number of private LVs this process is connected to */

// Word 2,3: 000000000331 000000000000

//     2 time_of_bootload fixed bin (71),                  /* bootload time during prelinking */

// Word 4: 

//     2 garbage_collections fixed bin (17) unaligned,     /* KST garbage collections */
//     2 entries_collected fixed bin (17) unaligned,               /* KST entries recovered by garbage collection */

//     2 free_list bit (18) unaligned,                     /* relative pointer to first free kste */
//     2 prelinked_ring (7) bit (1) unaligned,             /* rings prelinked in process */
//     2 template bit (1) unaligned,                       /* this is a template kst if set */
//     2 allow_256K_connect bit (1) unaligned,             /* can use 256K segments */
//     2 unused_2 bit (9) unaligned,
//     2 uid_hash_bucket (0 : 127) bit (18) unaligned,     /* hash buckets */
//     2 kst_entry (0 refer (kst.lowseg):0 refer (kst.highseg)) aligned like kste, /* kst entries */
//     2 lv (1:256) bit (36),                              /* private logical volume connection list */
//     2 end_of_kst bit (36);






// dcl 1 kste based (kstep) aligned,                           /* KST entry declaration */

// word 0:

//     2 fp bit (18) unaligned,                                /* forward rel pointer */
//     2 segno fixed bin (17) unaligned,                       /* segment number of this kste */

// word 1, 2:

//     2 usage_count (0:7) fixed bin (8) unaligned,            /* outstanding initiates/ring */

/// word 3, 4:

//     2 entryp ptr unaligned,                                 /* branch pointer */
//                                                             /* See WARNING below for requirements to use entryp. */

// word 5:

//     2 uid bit (36) aligned,                                 /* unique identifier */


//     2 access_information unaligned,

// word 6:

//       3 dtbm bit (36),                                      /* date time branch modified */

// word 7:

//       3 extended_access bit (33),                           /* extended access from the branch */
//       3 access bit (3),                                     /* rew */

// word 8;

//       3 ex_rb (3) bit (3),                                  /* ring brackets from branch */
//     2 pad1 bit (3) unaligned,
//     2 flags unaligned,
//       3 dirsw bit (1),                                      /* directory switch */
//       3 allow_write bit (1),                                /* set if initiated with write permission */
//       3 priv_init bit (1),                                  /* privileged initiation */
//       3 tms bit (1),                                        /* transparent modification switch */
//       3 tus bit (1),                                        /* transparent usage switch */
//       3 tpd bit (1),                                        /* transparent paging device switch */
//       3 audit bit (1),                                      /* audit switch */
//       3 explicit_deact_ok bit (1),                          /* set if I am willing to have a user force deactivate */
//       3 pad bit (3),
//     2 infcount fixed bin (12) unaligned;                    /* _^Hi_^Hf dirsw _^Ht_^Hh_^He_^Hn inferior count _^He_^Hl_^Hs_^He lv index */

// 9 words total



t_stat dumpKST (UNUSED int32 arg, UNUSED char * buf)
  {
#if 0
    for (word18 offset = 0; offset < 1024 * 64; offset ++)
      {
        sim_printf ("%06o %012"PRIo64"\n", offset, getKST (offset));
      }
#endif

    //sim_printf ("lowseg  %06"PRIo64"\n", getKST (0) >> 18);
    //sim_printf ("highseg %06"PRIo64"\n", getKST (0) & MASK18);
    sim_printf ("lowseg  %06"PRIo64"\n", getKST (0) & MASK18);
    sim_printf ("highseg %06"PRIo64"\n", getKST (1) & MASK18);

    word18 start = 0110;
    for (word18 i = 0; ; i ++)
      {
        word36 w0 = getKST (start + i * 8);
        if ((w0 & MASK18) == 0)
          break;
        sim_printf ("%4d %06"PRIo64"\n", i, w0 & MASK18);
        sim_printf ("    %012"PRIo64" %012"PRIo64"\n", getKST (start + i * 8 + 3), getKST (start + i * 8 + 4));
      }
    return SCPE_OK;
  }
#endif

//! custom command "init"
t_stat dpsCmd_Init (UNUSED int32 arg, const char *buf)
{
    char cmds [8][32];
    memset(cmds, 0, sizeof(cmds));  // clear cmds buffer
    
    int nParams = sscanf(buf, "%s %s %s %s", cmds[0], cmds[1], cmds[2], cmds[3]);
    if (nParams == 2 && !strcasecmp(cmds[0], "segment") && !strcasecmp(cmds[1], "table"))
        return dpsCmd_InitUnpagedSegmentTable();
#ifdef WAM
    if (nParams == 1 && !strcasecmp(cmds[0], "sdwam"))
        return dpsCmd_InitSDWAM();
#endif
    //if (nParams == 2 && !strcasecmp(cmds[0], "stack"))
    //    return createStack((int)strtoll(cmds[1], NULL, 8));
    
    return SCPE_OK;
}

//! custom command "segment" - stuff to do with deferred segments
t_stat dpsCmd_Segment (UNUSED int32  arg, const char *buf)
{
    char cmds [8][32];
    memset(cmds, 0, sizeof(cmds));  // clear cmds buffer
    
    /*
      cmds   0     1      2     3
     segment ??? remove
     segment ??? segref remove ????
     segment ??? segdef remove ????
     */
    int nParams = sscanf(buf, "%s %s %s %s %s", cmds[0], cmds[1], cmds[2], cmds[3], cmds[4]);
    if (nParams == 2 && !strcasecmp(cmds[0], "remove"))
        return removeSegment(cmds[1]);
    if (nParams == 4 && !strcasecmp(cmds[1], "segref") && !strcasecmp(cmds[2], "remove"))
        return removeSegref(cmds[0], cmds[3]);
    if (nParams == 4 && !strcasecmp(cmds[1], "segdef") && !strcasecmp(cmds[2], "remove"))
        return removeSegdef(cmds[0], cmds[3]);
    return SCPE_ARG;
}

//! custom command "segments" - stuff to do with deferred segments
t_stat dpsCmd_Segments (UNUSED int32 arg, const char *buf)
{
    bool bVerbose = !sim_quiet;

    char cmds [8][32];
    memset(cmds, 0, sizeof(cmds));  // clear cmds buffer
    
    /*
     * segments resolve
     * segments load deferred
     * segments remove ???
     */
    int nParams = sscanf(buf, "%s %s %s %s", cmds[0], cmds[1], cmds[2], cmds[3]);
    //if (nParams == 2 && !strcasecmp(cmds[0], "segment") && !strcasecmp(cmds[1], "table"))
    //    return dpsCmd_InitUnpagedSegmentTable();
    //if (nParams == 1 && !strcasecmp(cmds[0], "sdwam"))
    //    return dpsCmd_InitSDWAM();
    if (nParams == 1 && !strcasecmp(cmds[0], "resolve"))
        return resolveLinks(bVerbose);    // resolve external reverences in deferred segments
   
    if (nParams == 2 && !strcasecmp(cmds[0], "load") && !strcasecmp(cmds[1], "deferred"))
        return loadDeferredSegments(bVerbose);    // load all deferred segments
    
    if (nParams == 2 && !strcasecmp(cmds[0], "remove"))
        return removeSegment(cmds[1]);

    if (nParams == 2 && !strcasecmp(cmds[0], "lot") && !strcasecmp(cmds[1], "create"))
        return createLOT(bVerbose);
    if (nParams == 2 && !strcasecmp(cmds[0], "lot") && !strcasecmp(cmds[1], "snap"))
        return snapLOT(bVerbose);

    if (nParams == 3 && !strcasecmp(cmds[0], "create") && !strcasecmp(cmds[1], "stack"))
    {
        int _n = (int)strtoll(cmds[2], NULL, 8);
        return createStack(_n, bVerbose);
    }
    return SCPE_ARG;
}

static t_stat cpu_boot (UNUSED int32 unit_num, UNUSED DEVICE * dptr)
{
    // The boot button on the cpu is conneted to the boot button on the IOM
    // XXX is this true? Which IOM is it connected to?
    //return iom_boot (ASSUME0, & iom_dev);
    sim_printf ("Try 'BOOT IOMn'\n");
    return SCPE_ARG;
}

void setup_scbank_map (void)
  {
    sim_debug (DBG_DEBUG, & cpu_dev, "setup_scbank_map: SCBANK %d N_SCBANKS %d MEM_SIZE_MAX %d\n", SCBANK, N_SCBANKS, MEM_SIZE_MAX);

    // Initalize to unmapped
    for (uint pg = 0; pg < N_SCBANKS; pg ++)
      {
        // The port number that the page of memory can be accessed through
        cpu.scbank_map [pg] = -1; 
        // The offset in M of the page of memory on the other side of the
        // port
        cpu.scbank_pg_os [pg] = -1; 
      }

    // For each port (which is connected to a SCU
    for (int port_num = 0; port_num < N_CPU_PORTS; port_num ++)
      {
        if (! cpu.switches.enable [port_num])
          continue;
        // Simplifing assumption: simh SCU unit 0 is the SCU with the
        // low 4MW of memory, etc...
        int scu_unit_num = cables ->
          cablesFromScuToCpu[currentRunningCPUnum].ports[port_num].scu_unit_num;

        // Calculate the amount of memory in the SCU in words
        uint store_size = cpu.switches.store_size [port_num];
        // Map store size configuration switch (0-8) to memory size.
#ifdef DPS8M
        uint store_table [8] = 
          { 32768, 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304 };
#endif
#ifdef L68
#ifdef ISOLTS
// ISOLTS sez:
// for DPS88:
//   3. set store size switches to 2222.
// for L68:
//   3. remove the right free-edge connector on the 645pq wwb at slot ab28.
//
// During ISOLTS initialization, it requires that the memory switch be set to
// '3' for all eight ports; this corresponds to '2' for the DPS8M (131072)
// Then:
// isolts: a "lda 65536" (64k) failed to produce a store fault
//
// So it seems that the memory size is expected to be 64K, not 128K as per
// the swithes; presumably step 3 causes this. Fake it by tweaking store table:
//
        uint store_table [8] = 
          { 32768, 65536, 4194304, 65536, 524288, 1048576, 2097152, 262144 };
#else
        uint store_table [8] = 
          { 32768, 65536, 4194304, 131072, 524288, 1048576, 2097152, 262144 };
#endif
#endif
        //uint sz = 1 << (store_size + 15);
        uint sz = store_table [store_size];
        // Calculate the base address of the memory in words
        uint assignment = cpu.switches.assignment [port_num];
        uint base = assignment * sz;
        //sim_printf ("setup_scbank_map SCU %d base %08o sz %08o (%d)\n", port_num, base, sz, sz);

        // Now convert to SCBANK (number of pages, page number)
        uint sz_pages = sz / SCBANK;
        base = base / SCBANK;

        sim_debug (DBG_DEBUG, & cpu_dev, "setup_scbank_map: port:%d ss:%u as:%u sz_pages:%u ba:%u\n", port_num, store_size, assignment, sz_pages, base);

        for (uint pg = 0; pg < sz_pages; pg ++)
          {
            uint scpg = base + pg;
            if (scpg < N_SCBANKS)
              {
                if (cpu.scbank_map [scpg] != -1)
                  {
                    sim_printf ("scbank overlap scpg %d (%o) old port %d newport %d\n", scpg, scpg, cpu.scbank_map [scpg], port_num);
                  }
                else
                  {
                    cpu.scbank_map [scpg] = port_num;
                    cpu.scbank_pg_os [scpg] = (int) ((uint) scu_unit_num * 4u * 1024u * 1024u + scpg * SCBANK);
                  }
              }
            else
              {
                sim_printf ("scpg too big port %d scpg %d (%o), limit %d (%o)\n", port_num, scpg, scpg, N_SCBANKS, N_SCBANKS);
              }
          }
      }
    for (uint pg = 0; pg < N_SCBANKS; pg ++)
      sim_debug (DBG_DEBUG, & cpu_dev, "setup_scbank_map: %d:%d\n", pg, cpu.scbank_map [pg]);
    //for (uint pg = 0; pg < N_SCBANKS; pg ++)
      //sim_printf ("scbank_pg_os: CPU %c %d:%08o\n", currentRunningCPUnum + 'A', pg, cpu.scbank_pg_os [pg]);
  }

int query_scbank_map (word24 addr)
  {
    uint scpg = addr / SCBANK;
    if (scpg < N_SCBANKS)
      return cpu.scbank_map [scpg];
    return -1;
  }

//
// serial.txt format
//
//      sn:  number[,number]
//
//  Additional numbers will be for multi-cpu systems.
//  Other fields to be added.

static void getSerialNumber (void)
  {
    bool havesn = false;
    FILE * fp = fopen ("./serial.txt", "r");
    while (fp && ! feof (fp))
      {
        char buffer [81] = "";
        fgets (buffer, sizeof (buffer), fp);
#ifdef ROUND_ROBIN
        uint cpun, sn;
        if (sscanf (buffer, "sn%u: %u", & cpun, & sn) == 2)
          {
            if (cpun < N_CPU_UNITS_MAX)
              {
                uint save = setCPUnum (cpun);
                cpu.switches.serno = sn;
                sim_printf ("Serial number of CPU %u is %u\n", cpun, cpu.switches.serno);
                setCPUnum (save);
                havesn = true;
              }
          }
#else
        if (sscanf (buffer, "sn: %u", & cpu.switches.serno) == 1)
          {
            sim_printf ("Serial number is %u\n", cpu.switches.serno);
            havesn = true;
          }
#endif
      }
    if (! havesn)
      {
        sim_printf ("Please register your system at https://ringzero.wikidot.com/wiki:register\n");
        sim_printf ("or create the file 'serial.txt' containing the line 'sn: 0'.\n");
      }
    if (fp)
      fclose (fp);
  }


#ifdef EV_POLL
// The 100Hz timer as expired; poll I/O

static void ev_poll_cb (uv_timer_t * UNUSED handle)
  {
    // Call the one hertz stuff every 100 loops
    static uint oneHz = 0;
    if (oneHz ++ >= sys_opts.sys_slow_poll_interval) // ~ 1Hz
      {
        oneHz = 0;
        rdrProcessEvent (); 
      }
    scpProcessEvent (); 
    fnpProcessEvent (); 
    consoleProcess ();
#ifndef FNP2
    dequeue_fnp_command ();
#endif
#ifndef __MINGW64__
    absiProcessEvent ();
#endif

// Update the TR

// The Timer register runs at 512Khz; in 1/100 of a second it
// decrements 5120.

// Will it pass through zero?

    if (cpu.rTR <= 5120)
      {
        //sim_debug (DBG_TRACE, & cpu_dev, "rTR %09o %09"PRIo64"\n", rTR, MASK27);
        if (cpu.switches.tro_enable)
        setG7fault (currentRunningCPUnum, FAULT_TRO, (_fault_subtype) {.bits=0});
      }
    cpu.rTR -= 5120;
    cpu.rTR &= MASK27;
#if ISOLTS
    cpu.shadowTR = cpu.rTR;
#endif
  }
#endif


    
// called once initialization

void cpu_init (void)
  {

// !!!! Do not use 'cpu' in this routine; usage of 'cpus' violates 'restrict'
// !!!! attribute

#ifdef M_SHARED
    if (! M)
      {
        M = (word36 *) create_shm ("M", getsid (0), MEMSIZE * sizeof (word36));
      }
#else
    if (! M)
      {
        M = (word36 *) calloc (MEMSIZE, sizeof (word36));
      }
#endif
    if (! M)
      {
        sim_printf ("create M failed\n");
        sim_err ("create M failed\n");
      }

#ifdef M_SHARED
    if (! cpus)
      {
        cpus = (cpu_state_t *) create_shm ("cpus", getsid (0), N_CPU_UNITS_MAX * sizeof (cpu_state_t));
      }
    if (! cpus)
      {
        sim_printf ("create cpus failed\n");
        sim_err ("create cpus failed\n");
      }
#endif

    memset (& watchBits, 0, sizeof (watchBits));

    setCPUnum (0);

#ifdef ROUND_ROBIN
    memset (cpus, 0, sizeof (cpu_state_t) * N_CPU_UNITS_MAX);
    cpus [0].switches.FLT_BASE = 2; // Some of the UnitTests assume this
#else
    memset (& cpu, 0, sizeof (cpu));
    cpu.switches.FLT_BASE = 2; // Some of the UnitTests assume this
#endif
    cpu_init_array ();

    getSerialNumber ();

#ifdef EV_POLL
    ev_poll_loop = uv_default_loop ();
    uv_timer_init (ev_poll_loop, & ev_poll_handle);
    // 10 ms == 100Hz
    uv_timer_start (& ev_poll_handle, ev_poll_cb, sys_opts.sys_poll_interval, sys_opts.sys_poll_interval);
#endif
  }

// DPS8M Memory of 36 bit words is implemented as an array of 64 bit words.
// Put state information into the unused high order bits.
#define MEM_UNINITIALIZED 0x4000000000000000LLU

static void cpun_reset2 (UNUSED uint cpun)
{
#ifdef ROUND_ROBIN
    setCPUnum (cpun);
#endif
    cpu.rA = 0;
    cpu.rQ = 0;
    
    cpu.PPR.IC = 0;
    cpu.PPR.PRR = 0;
    cpu.PPR.PSR = 0;
    cpu.PPR.P = 1;
    cpu.RSDWH_R1 = 0;
    cpu.rTR = 0;
#if ISOLTS
    cpu.shadowTR = 0;
#endif
 
    set_addr_mode(ABSOLUTE_mode);
    SET_I_NBAR;
    
    cpu.CMR.luf = 3;    // default of 16 mS
#ifdef WAM
    cpu.cu.SD_ON = 1;
    cpu.cu.PT_ON = 1;
#else
// If WAM emulation is not enabled in the build, mark it disabled
    cpu.cu.SD_ON = 0;
    cpu.cu.PT_ON = 0;
#endif
 
    setCpuCycle (FETCH_cycle);

    cpu.wasXfer = false;
    cpu.wasInhibited = false;

    cpu.interrupt_flag = false;
    cpu.g7_flag = false;

    cpu.faultRegister [0] = 0;
    cpu.faultRegister [1] = 0;

#ifdef RAPRx
    cpu.apu.lastCycle = UNKNOWN_CYCLE;
#endif

    memset (& cpu.PPR, 0, sizeof (struct _ppr));

    setup_scbank_map ();

    tidy_cu ();
}

static void cpu_reset2 (void)
{
    for (uint i = 0; i < N_CPU_UNITS_MAX; i ++)
      {
        cpun_reset2 (i);
      }

#ifdef ROUND_ROBIN
    setCPUnum (0);
#endif

    sim_brk_types = sim_brk_dflt = SWMASK ('E');

    sys_stats.total_cycles = 0;
    for (int i = 0; i < N_FAULTS; i ++)
      sys_stats.total_faults [i] = 0;
    
    
    // XXX free up previous deferred segments (if any)
    
    
#ifdef USE_IDLE
    sim_set_idle (cpu_unit, 512*1024, NULL, NULL);
#endif
    sim_debug (DBG_INFO, & cpu_dev, "CPU reset: Running\n");

    // TODO: reset *all* other structures to zero
    
    memset(&sys_stats, 0, sizeof(sys_stats));
    
#ifdef MATRIX
    initializeTheMatrix();
#endif

}

static t_stat cpu_reset (UNUSED DEVICE *dptr)
{
    //memset (M, -1, MEMSIZE * sizeof (word36));

    // Fill DPS8M memory with zeros, plus a flag only visible to the emulator
    // marking the memory as uninitialized.

    for (uint i = 0; i < MEMSIZE; i ++)
      M [i] = MEM_UNINITIALIZED;

    cpu_reset2 ();
    return SCPE_OK;
}

/*! Memory examine */
//  t_stat examine_routine (t_val *eval_array, t_addr addr, UNIT *uptr, int32 switches) – Copy 
//  sim_emax consecutive addresses for unit uptr, starting at addr, into eval_array. The switch 
//  variable has bit<n> set if the n’th letter was specified as a switch to the examine command. 
// Not true...

static t_stat cpu_ex (t_value *vptr, t_addr addr, UNUSED UNIT * uptr, UNUSED int32 sw)
{
    if (addr>= MEMSIZE)
        return SCPE_NXM;
    if (vptr != NULL)
      {
        *vptr = M[addr] & DMASK;
      }
    return SCPE_OK;
}

/*! Memory deposit */
static t_stat cpu_dep (t_value val, t_addr addr, UNUSED UNIT * uptr, 
                       UNUSED int32 sw)
{
    if (addr >= MEMSIZE) return SCPE_NXM;
    M[addr] = val & DMASK;
    return SCPE_OK;
}



/*
 * register stuff ...
 */

#ifdef M_SHARED
// simh has to have a statically allocated IC to refer to.
static word18 dummyIC;
#endif

static REG cpu_reg[] = {
#ifdef M_SHARED
    { ORDATA (IC, dummyIC, VASIZE), 0, 0, 0 },// Must be the first; see sim_PC.
    //{ ORDATA (IC, cpus[0].PPR.IC, VASIZE), 0, 0, 0 },// Must be the first; see sim_PC.
#else
    { ORDATA (IC, cpus[0].PPR.IC, VASIZE), 0, 0, 0 },// Must be the first; see sim_PC.
    //{ ORDATA (IR, cpu.cu.IR, 18), 0, 0, 0 },
    //{ ORDATADF (IR, cpu.cu.IR, 18, "Indicator Register", dps8_IR_bits), 0, 0, 0 },
    
    //    { FLDATA (Zero, cpu.cu.IR, F_V_A), 0, 0 },
    //    { FLDATA (Negative, cpu.cu.IR, F_V_B), 0, 0 },
    //    { FLDATA (Carry, cpu.cu.IR, F_V_C), 0, 0 },
    //    { FLDATA (Overflow, cpu.cu.IR, F_V_D), 0, 0 },
    //    { FLDATA (ExpOvr, cpu.cu.IR, F_V_E), 0, 0 },
    //    { FLDATA (ExpUdr, cpu.cu.IR, F_V_F), 0, 0 },
    //    { FLDATA (OvrMask, cpu.cu.IR, F_V_G), 0, 0 },
    //    { FLDATA (TallyRunOut, cpu.cu.IR, F_V_H), 0, 0 },
    //    { FLDATA (ParityErr, cpu.cu.IR, F_V_I), 0, 0 }, ///< Yeah, right!
    //    { FLDATA (ParityMask, cpu.cu.IR, F_V_J), 0, 0 },
    //    { FLDATA (NotBAR, cpu.cu.IR, F_V_K), 0, 0 },
    //    { FLDATA (Trunc, cpu.cu.IR, F_V_L), 0, 0 },
    //    { FLDATA (MidInsFlt, cpu.cu.IR, F_V_M), 0, 0 },
    //    { FLDATA (AbsMode, cpu.cu.IR, F_V_N), 0, 0 },
    //    { FLDATA (HexMode, cpu.cu.IR, F_V_O), 0, 0 },
    
    { ORDATA (A, cpus[0].rA, 36), 0, 0, 0 },
    { ORDATA (Q, cpus[0].rQ, 36), 0, 0, 0 },
    { ORDATA (E, cpus[0].rE, 8), 0, 0, 0 },
    
    { ORDATA (X0, cpus[0].rX [0], 18), 0, 0, 0 },
    { ORDATA (X1, cpus[0].rX [1], 18), 0, 0, 0 },
    { ORDATA (X2, cpus[0].rX [2], 18), 0, 0, 0 },
    { ORDATA (X3, cpus[0].rX [3], 18), 0, 0, 0 },
    { ORDATA (X4, cpus[0].rX [4], 18), 0, 0, 0 },
    { ORDATA (X5, cpus[0].rX [5], 18), 0, 0, 0 },
    { ORDATA (X6, cpus[0].rX [6], 18), 0, 0, 0 },
    { ORDATA (X7, cpus[0].rX [7], 18), 0, 0, 0 },
    
    { ORDATA (PPR.IC,  cpus[0].PPR.IC,  18), 0, 0, 0 },
    { ORDATA (PPR.PRR, cpus[0].PPR.PRR,  3), 0, 0, 0 },
    { ORDATA (PPR.PSR, cpus[0].PPR.PSR, 15), 0, 0, 0 },
    { ORDATA (PPR.P,   cpus[0].PPR.P,    1), 0, 0, 0 },
    
    { ORDATA (RALR,    cpus[0].rRALR,    3), 0, 0, 0 },
    
    { ORDATA (DSBR.ADDR,  cpus[0].DSBR.ADDR,  24), 0, 0, 0 },
    { ORDATA (DSBR.BND,   cpus[0].DSBR.BND,   14), 0, 0, 0 },
    { ORDATA (DSBR.U,     cpus[0].DSBR.U,      1), 0, 0, 0 },
    { ORDATA (DSBR.STACK, cpus[0].DSBR.STACK, 12), 0, 0, 0 },
    
    { ORDATA (BAR.BASE,  cpus[0].BAR.BASE,  9), 0, 0, 0 },
    { ORDATA (BAR.BOUND, cpus[0].BAR.BOUND, 9), 0, 0, 0 },
    
    //{ ORDATA (FAULTBASE, rFAULTBASE, 12), 0, 0 }, ///< only top 7-msb are used
    
    { ORDATA (PR0.SNR, cpus[0].PR[0].SNR, 18), 0, 0, 0 },
    { ORDATA (PR1.SNR, cpus[0].PR[1].SNR, 18), 0, 0, 0 },
    { ORDATA (PR2.SNR, cpus[0].PR[2].SNR, 18), 0, 0, 0 },
    { ORDATA (PR3.SNR, cpus[0].PR[3].SNR, 18), 0, 0, 0 },
    { ORDATA (PR4.SNR, cpus[0].PR[4].SNR, 18), 0, 0, 0 },
    { ORDATA (PR5.SNR, cpus[0].PR[5].SNR, 18), 0, 0, 0 },
    { ORDATA (PR6.SNR, cpus[0].PR[6].SNR, 18), 0, 0, 0 },
    { ORDATA (PR7.SNR, cpus[0].PR[7].SNR, 18), 0, 0, 0 },
    
    { ORDATA (PR0.RNR, cpus[0].PR[0].RNR, 18), 0, 0, 0 },
    { ORDATA (PR1.RNR, cpus[0].PR[1].RNR, 18), 0, 0, 0 },
    { ORDATA (PR2.RNR, cpus[0].PR[2].RNR, 18), 0, 0, 0 },
    { ORDATA (PR3.RNR, cpus[0].PR[3].RNR, 18), 0, 0, 0 },
    { ORDATA (PR4.RNR, cpus[0].PR[4].RNR, 18), 0, 0, 0 },
    { ORDATA (PR5.RNR, cpus[0].PR[5].RNR, 18), 0, 0, 0 },
    { ORDATA (PR6.RNR, cpus[0].PR[6].RNR, 18), 0, 0 , 0},
    { ORDATA (PR7.RNR, cpus[0].PR[7].RNR, 18), 0, 0, 0 },
    
    //{ ORDATA (PR0.BITNO, PR[0].PBITNO, 18), 0, 0, 0 },
    //{ ORDATA (PR1.BITNO, PR[1].PBITNO, 18), 0, 0, 0 },
    //{ ORDATA (PR2.BITNO, PR[2].PBITNO, 18), 0, 0, 0 },
    //{ ORDATA (PR3.BITNO, PR[3].PBITNO, 18), 0, 0, 0 },
    //{ ORDATA (PR4.BITNO, PR[4].PBITNO, 18), 0, 0, 0 },
    //{ ORDATA (PR5.BITNO, PR[5].PBITNO, 18), 0, 0, 0 },
    //{ ORDATA (PR6.BITNO, PR[6].PBITNO, 18), 0, 0, 0 },
    //{ ORDATA (PR7.BITNO, PR[7].PBITNO, 18), 0, 0, 0 },
    
    //{ ORDATA (AR0.BITNO, PR[0].ABITNO, 18), 0, 0, 0 },
    //{ ORDATA (AR1.BITNO, PR[1].ABITNO, 18), 0, 0, 0 },
    //{ ORDATA (AR2.BITNO, PR[2].ABITNO, 18), 0, 0, 0 },
    //{ ORDATA (AR3.BITNO, PR[3].ABITNO, 18), 0, 0, 0 },
    //{ ORDATA (AR4.BITNO, PR[4].ABITNO, 18), 0, 0, 0 },
    //{ ORDATA (AR5.BITNO, PR[5].ABITNO, 18), 0, 0, 0 },
    //{ ORDATA (AR6.BITNO, PR[6].ABITNO, 18), 0, 0, 0 },
    //{ ORDATA (AR7.BITNO, PR[7].ABITNO, 18), 0, 0, 0 },
    
    { ORDATA (PR0.WORDNO, cpus[0].PR[0].WORDNO, 18), 0, 0, 0 },
    { ORDATA (PR1.WORDNO, cpus[0].PR[1].WORDNO, 18), 0, 0, 0 },
    { ORDATA (PR2.WORDNO, cpus[0].PR[2].WORDNO, 18), 0, 0, 0 },
    { ORDATA (PR3.WORDNO, cpus[0].PR[3].WORDNO, 18), 0, 0, 0 },
    { ORDATA (PR4.WORDNO, cpus[0].PR[4].WORDNO, 18), 0, 0, 0 },
    { ORDATA (PR5.WORDNO, cpus[0].PR[5].WORDNO, 18), 0, 0, 0 },
    { ORDATA (PR6.WORDNO, cpus[0].PR[6].WORDNO, 18), 0, 0, 0 },
    { ORDATA (PR7.WORDNO, cpus[0].PR[7].WORDNO, 18), 0, 0, 0 },
    
    //{ ORDATA (PR0.CHAR, PR[0].CHAR, 18), 0, 0, 0 },
    //{ ORDATA (PR1.CHAR, PR[1].CHAR, 18), 0, 0, 0 },
    //{ ORDATA (PR2.CHAR, PR[2].CHAR, 18), 0, 0, 0 },
    //{ ORDATA (PR3.CHAR, PR[3].CHAR, 18), 0, 0, 0 },
    //{ ORDATA (PR4.CHAR, PR[4].CHAR, 18), 0, 0, 0 },
    //{ ORDATA (PR5.CHAR, PR[5].CHAR, 18), 0, 0, 0 },
    //{ ORDATA (PR6.CHAR, PR[6].CHAR, 18), 0, 0, 0 },
    //{ ORDATA (PR7.CHAR, PR[7].CHAR, 18), 0, 0, 0 },
    
    /*
     { ORDATA (EBR, ebr, EBR_N_EBR), 0, 0, 0 },
     { FLDATA (PGON, ebr, EBR_V_PGON), 0, 0, 0 },
     { FLDATA (T20P, ebr, EBR_V_T20P), 0, 0, 0 },
     { ORDATA (UBR, ubr, 36), 0, 0, 0 },
     { GRDATA (CURAC, ubr, 8, 3, UBR_V_CURAC), REG_RO, 0 },
     { GRDATA (PRVAC, ubr, 8, 3, UBR_V_PRVAC), 0 },
     { ORDATA (SPT, spt, 36), 0, 0, 0 },
     { ORDATA (CST, cst, 36), 0, 0, 0 },
     { ORDATA (PUR, pur, 36), 0, 0, 0 },
     { ORDATA (CSTM, cstm, 36), 0, 0, 0 },
     { ORDATA (HSB, hsb, 36), 0, 0, 0 },
     { ORDATA (DBR1, dbr1, PASIZE), 0, 0, 0 },
     { ORDATA (DBR2, dbr2, PASIZE), 0, 0, 0 },
     { ORDATA (DBR3, dbr3, PASIZE), 0, 0, 0 },
     { ORDATA (DBR4, dbr4, PASIZE), 0, 0, 0 },
     { ORDATA (PCST, pcst, 36), 0, 0, 0 },
     { ORDATA (PIENB, pi_enb, 7), 0, 0, 0 },
     { FLDATA (PION, pi_on, 0), 0, 0, 0 },
     { ORDATA (PIACT, pi_act, 7), 0, 0, 0 },
     { ORDATA (PIPRQ, pi_prq, 7), 0, 0, 0 },
     { ORDATA (PIIOQ, pi_ioq, 7), REG_RO, 0 },
     { ORDATA (PIAPR, pi_apr, 7), REG_RO, 0 },
     { ORDATA (APRENB, apr_enb, 8), 0, 0, 0 },
     { ORDATA (APRFLG, apr_flg, 8), 0, 0, 0 },
     { ORDATA (APRLVL, apr_lvl, 3), 0, 0, 0 },
     { ORDATA (RLOG, rlog, 10), 0, 0, 0 },
     { FLDATA (F1PR, its_1pr, 0), 0, 0, 0 },
     { BRDATA (PCQ, pcq, 8, VASIZE, PCQ_SIZE), REG_RO+REG_CIRC, 0 },
     { ORDATA (PCQP, pcq_p, 6), REG_HRO, 0 },
     { DRDATA (INDMAX, ind_max, 8), PV_LEFT + REG_NZ, 0 },
     { DRDATA (XCTMAX, xct_max, 8), PV_LEFT + REG_NZ, 0 },
     { ORDATA (WRU, sim_int_char, 8), 0, 0, 0 },
     { FLDATA (STOP_ILL, stop_op0, 0), 0, 0, 0 },
     { BRDATA (REG, acs, 8, 36, AC_NUM * AC_NBLK), 0 },
     */
#endif
    { NULL, NULL, 0, 0, 0, 0, NULL, NULL, 0, 0, 0 }
};

/*
 * simh interface
 */

REG *sim_PC = &cpu_reg[0];

/*! CPU device descriptor */
DEVICE cpu_dev = {
    "CPU",          /*!< name */
    cpu_unit,       /*!< units */
    cpu_reg,        /*!< registers */
    cpu_mod,        /*!< modifiers */
    N_CPU_UNITS,    /*!< #units */
    8,              /*!< address radix */
    PASIZE,         /*!< address width */
    1,              /*!< addr increment */
    8,              /*!< data radix */
    36,             /*!< data width */
    &cpu_ex,        /*!< examine routine */
    &cpu_dep,       /*!< deposit routine */
    &cpu_reset,     /*!< reset routine */
    &cpu_boot,           /*!< boot routine */
    NULL,           /*!< attach routine */
    NULL,           /*!< detach routine */
    NULL,           /*!< context */
    DEV_DEBUG,      /*!< device flags */
    0,              /*!< debug control flags */
    cpu_dt,         /*!< debug flag names */
    NULL,           /*!< memory size change */
    NULL,           /*!< logical name */
    NULL,           // help
    NULL,           // attach help
    NULL,           // help context
    NULL,           // description
    NULL
};

#ifdef M_SHARED
cpu_state_t * cpus = NULL;
#else
cpu_state_t cpus [N_CPU_UNITS_MAX];
#endif
cpu_state_t * restrict cpup; 

#ifdef ROUND_ROBIN
uint currentRunningCPUnum;
#endif

// Scan the SCUs; it one has an interrupt present, return the fault pair
// address for the highest numbered interrupt on that SCU. If no interrupts
// are found, return 1.
 
static uint get_highest_intr (void)
  {
    for (uint scuUnitNum = 0; scuUnitNum < N_SCU_UNITS_MAX; scuUnitNum ++)
      {
        if (cpu.events.XIP [scuUnitNum])
          {
            uint fp = scuGetHighestIntr (scuUnitNum);
            if (fp != 1)
              return fp;
          }
      }
    return 1;
  }

bool sample_interrupts (void)
  {
    cpu.lufCounter = 0;
    for (uint scuUnitNum = 0; scuUnitNum < N_SCU_UNITS_MAX; scuUnitNum ++)
      {
        if (cpu.events.XIP [scuUnitNum])
          {
            return true;
          }
      }
    return false;
  }

t_stat simh_hooks (void)
  {
    int reason = 0;

    if (stop_cpu)
      return STOP_STOP;

#ifdef ISOLTS
    if (currentRunningCPUnum == 0)
#endif
    // check clock queue 
    if (sim_interval <= 0)
      {
//int32 int0 = sim_interval;
        reason = sim_process_event ();
//sim_printf ("int delta %d\n", sim_interval - int0);
        if (reason)
          return reason;
      }
        
    sim_interval --;

// This is needed for BCE_TRAP in install scripts
    // breakpoint? 
    //if (sim_brk_summ && sim_brk_test (PPR.IC, SWMASK ('E')))
    // sim_brk_test expects a 32 bit address; PPR.IC into the low 18, and
    // PPR.PSR into the high 12
    if (sim_brk_summ &&
        sim_brk_test ((cpu.PPR.IC & 0777777) |
                      ((((t_addr) cpu.PPR.PSR) & 037777) << 18),
                      SWMASK ('E')))  /* breakpoint? */
      return STOP_BKPT; /* stop simulation */
#ifndef SPEED
    if (sim_deb_break && sim_timell () >= sim_deb_break)
      return STOP_BKPT; /* stop simulation */
#endif

    return reason;
  }       

static char * cycleStr (cycles_t cycle)
  {
    switch (cycle)
      {
        //case ABORT_cycle:
          //return "ABORT_cycle";
        case FAULT_cycle:
          return "FAULT_cycle";
        case EXEC_cycle:
          return "EXEC_cycle";
        case FAULT_EXEC_cycle:
          return "FAULT_EXEC_cycle";
        case FAULT_EXEC2_cycle:
          return "FAULT_EXEC2_cycle";
        case INTERRUPT_cycle:
          return "INTERRUPT_cycle";
        case INTERRUPT_EXEC_cycle:
          return "INTERRUPT_EXEC_cycle";
        case INTERRUPT_EXEC2_cycle:
          return "INTERRUPT_EXEC2_cycle";
        case FETCH_cycle:
          return "FETCH_cycle";
        case SYNC_FAULT_RTN_cycle:
          return "SYNC_FAULT_RTN_cycle";
        default:
          return "unknown cycle";
     }
  }

static void setCpuCycle (cycles_t cycle)
  {
    sim_debug (DBG_CYCLE, & cpu_dev, "Setting cycle to %s\n",
               cycleStr (cycle));
    cpu.cycle = cycle;
  }


uint setCPUnum (UNUSED uint cpuNum)
  {
    uint prev = currentRunningCPUnum;
#ifdef ROUND_ROBIN
    currentRunningCPUnum = cpuNum;
#endif
    cpup = & cpus [currentRunningCPUnum];
    return prev;
  }

uint getCPUnum (void)
  {
    return currentRunningCPUnum;
  }

#ifdef PANEL
static void panelProcessEvent (void)
  {
    // INITIALIZE pressed; treat at as a BOOT.
    if (cpu.panelInitialize) 
      {
         // Wait for release
         while (cpu.panelInitialize) 
           ;
         if (cpu.DATA_panel_init_sw)
           cpu_reset (& cpu_dev); // INITIALIZE & CLEAR
         else
           cpu_reset2 (); // INITIALIZE
         // XXX Until a boot switch is wired up
         doBoot ();
      }
    // EXECUTE pressed; EXECUTE PB set, EXECUTE FAULT set
    if (cpu.DATA_panel_execute_sw && // EXECUTE buttton
        cpu.DATA_panel_scope_sw && // 'EXECUTE PB/SCOPE REPEAT' set to PB
        cpu.DATA_panel_exec_sw == 0) // 'EXECUTE SWITCH/EXECUTE FAULT' set to FAULT
      {
        // Wait for release
        while (cpu.DATA_panel_execute_sw) 
          ;

        if (cpu.DATA_panel_exec_sw) // EXECUTE SWITCH
          {
            cpu_reset2 ();
            cpu.cu.IWB = cpu.switches.data_switches;
            setCpuCycle (EXEC_cycle);
          }
         else // EXECUTE FAULT
          {
            setG7fault (currentRunningCPUnum, FAULT_EXF, (_fault_subtype) {.bits=0});
          }
      }
  }
#endif

//
// Okay, lets treat this as a state machine
//
//  INTERRUPT_cycle
//     clear interrupt, load interrupt pair into instruction buffer
//     set INTERRUPT_EXEC_cycle
//  INTERRUPT_EXEC_cycle
//     execute instruction in instruction buffer
//     if (! transfer) set INTERUPT_EXEC2_cycle 
//     else set FETCH_cycle
//  INTERRUPT_EXEC2_cycle
//     execute odd instruction in instruction buffer
//     set INTERUPT_EXEC2_cycle 
//
//  FAULT_cycle
//     fetch fault pair into instruction buffer
//     set FAULT_EXEC_cycle
//  FAULT_EXEC_cycle
//     execute instructions in instruction buffer
//     if (! transfer) set FAULT_EXE2_cycle 
//     else set FETCH_cycle
//  FAULT_EXEC2_cycle
//     execute odd instruction in instruction buffer
//     set FETCH_cycle
//
//  FETCH_cycle
//     fetch instruction into instruction buffer
//     set EXEC_cycle
//
//  EXEC_cycle
//     execute instruction in instruction buffer
//     if (repeat conditions) keep cycling
//     if (pair) set EXEC2_cycle
//     else set FETCH_cycle
//  EXEC2_cycle
//     execute odd instruction in instruction buffer
//
//  XEC_cycle
//     load instruction into instruction buffer
//     set EXEC_cyvle
//
//  XED_cycle
//     load instruction pair into instruction buffer
//     set EXEC_cyvle
//  
// other extant cycles:
//  ABORT_cycle

#ifdef EV_POLL
static uint fastQueueSubsample = 0;
#endif

// This is part of the simh interface
t_stat sim_instr (void)
  {
    t_stat reason = 0;
#ifdef USE_IDLE
    sim_rtcn_init (0, 0);
#endif
      
#ifdef FNP2
#else
    mux(SLS, 0, 0);

    UNIT *u = &mux_unit;
    if (u->filename == NULL || strlen(u->filename) == 0)
        sim_printf("Warning: MUX not attached.\n");
#endif
 
#ifdef M_SHARED
// simh needs to have the IC statically allocated, so a placeholder was
// created. Copy the placeholder in so the IC can be set by simh.

    setCPUnum (0);
    cpus [0].PPR.IC = dummyIC;
#endif

    setCPUnum (0);
#ifdef ROUND_ROBIN
    cpu.isRunning = true;
    setCPUnum (cpu_dev.numunits - 1);

setCPU:;
    uint current = currentRunningCPUnum;
    uint c;
    for (c = 0; c < cpu_dev.numunits; c ++)
      {
        setCPUnum (c);
        if (cpu.isRunning)
          break;
      }
    if (c == cpu_dev.numunits)
      {
        sim_printf ("All CPUs stopped\n");
        goto leave;
      }
    setCPUnum ((current + 1) % cpu_dev.numunits);
    if (! cpu . isRunning)
      goto setCPU;
#endif

    // This allows long jumping to the top of the state machine
    int val = setjmp(cpu.jmpMain);

    switch (val)
    {
        case JMP_ENTRY:
        case JMP_REENTRY:
            reason = 0;
            break;
        case JMP_SYNC_FAULT_RETURN:
            setCpuCycle (SYNC_FAULT_RTN_cycle);
            break;
        case JMP_STOP:
            reason = STOP_HALT;
            goto leave;
        case JMP_REFETCH:

            // Not necessarily so, but the only times
            // this path is taken is after an RCU returning
            // from an interrupt, which could only happen if
            // was xfer was false; or in a DIS cycle, in
            // which case we want it false so interrupts 
            // can happen.
            cpu.wasXfer = false;
             
            setCpuCycle (FETCH_cycle);
            break;
        case JMP_RESTART:
            setCpuCycle (EXEC_cycle);
            break;
        default:
          sim_printf ("longjmp value of %d unhandled\n", val);
            goto leave;
    }

    // Main instruction fetch/decode loop 

    DCDstruct * ci = & cpu.currentInstruction;

    do
      {
        reason = 0;

        // Process deferred events and breakpoints
        reason = simh_hooks ();
        if (reason)
          {
            //sim_printf ("reason: %d\n", reason);
            break;
          }

#ifdef EV_POLL
// The event poll is consuming 40% of the CPU according to pprof.
// We only want to process at 100Hz; yet we are testing at ~1MHz.
// If we only test every 1000 cycles, we shouldn't miss by more then
// 10%...

        //static uint fastQueueSubsample = 0;
        if (fastQueueSubsample ++ > sys_opts.sys_poll_check_rate) // ~ 1KHz
          {
            fastQueueSubsample = 0;
            uv_run (ev_poll_loop, UV_RUN_NOWAIT);
            PNL (panelProcessEvent ());
          }
#else
        static uint slowQueueSubsample = 0;
        if (slowQueueSubsample ++ > 1024000) // ~ 1Hz
          {
            slowQueueSubsample = 0;
            rdrProcessEvent (); 
          }
        static uint queueSubsample = 0;
        if (queueSubsample ++ > 10240) // ~ 100Hz
          {
            queueSubsample = 0;
            scpProcessEvent (); 
            fnpProcessEvent (); 
            consoleProcess ();
#ifdef FNP2
#else
            dequeue_fnp_command ();
#endif
            absiProcessEvent ();
            PNL (panelProcessEvent ());
          }
#endif

#if 0
        if (sim_gtime () % 1024 == 0)
          {
            t_stat ch = sim_poll_kbd ();
            if (ch != SCPE_OK)
              {
                //sim_printf ("%o\n", ch);
                if (ch == 010033) // Escape
                  console_attn (NULL);
              }
          }
#else
        if (check_attn_key ())
          console_attn (NULL);
#endif

#ifndef EV_POLL
        // Manage the timer register
             // XXX this should be sync to the EXECUTE cycle, not the
             // simh clock cycle; move down...
             // Acutally have FETCH jump to EXECUTE
             // instead of breaking.

        // Sync. the TR with the emulator clock.
        cpu.rTRlsb ++;
        // The emulator clock runs about 7x as fast at the Timer Register;
        // see wiki page "CAC 08-Oct-2014"
        //if (cpu.rTRlsb >= cpu.switches.trlsb)
        if (cpu.rTRlsb >= 12)
          {
            cpu.rTRlsb = 0;
            cpu.rTR = (cpu.rTR - 1) & MASK27;
            //sim_debug (DBG_TRACE, & cpu_dev, "rTR %09o\n", rTR);
            if (cpu.rTR == 0) // passing thorugh 0...
              {
                //sim_debug (DBG_TRACE, & cpu_dev, "rTR %09o %09"PRIo64"\n", rTR, MASK27);
                if (cpu.switches.tro_enable)
                  setG7fault (currentRunningCPUnum, FAULT_TRO, (_fault_subtype) {.bits=0});
              }
          }
#endif

        sim_debug (DBG_CYCLE, & cpu_dev, "Cycle switching to %s\n",
                   cycleStr (cpu.cycle));

#ifdef PANEL
        //memset (cpu.cpt, 0, sizeof (cpu.cpt));
#endif

        switch (cpu.cycle)
          {
            case INTERRUPT_cycle:
              {
                CPT (cpt1U, 0); // Interupt cycle
                // In the INTERRUPT CYCLE, the processor safe-stores
                // the Control Unit Data (see Section 3) into 
                // program-invisible holding registers in preparation 
                // for a Store Control Unit (scu) instruction, enters 
                // temporary absolute mode, and forces the current 
                // ring of execution C(PPR.PRR) to
                // 0. It then issues an XEC system controller command 
                // to the system controller on the highest priority 
                // port for which there is a bit set in the interrupt 
                // present register.  

                uint intr_pair_addr = get_highest_intr ();
#ifdef LOOPTRC
{ void elapsedtime (void);
elapsedtime ();
 sim_printf (" intr %u PSR:IC %05o:%06o\r\n", intr_pair_addr, cpu.PPR.PSR, cpu.PPR.IC);
}
#endif
                cpu.cu.FI_ADDR = (word5) (intr_pair_addr / 2);
                cu_safe_store ();

                CPT (cpt1U, 1); // safe store complete
                // Temporary absolute mode
                set_TEMPORARY_ABSOLUTE_mode ();

                // Set to ring 0
                cpu.PPR.PRR = 0;
                cpu.TPR.TRR = 0;

                // Check that an interrupt is actually pending
                if (cpu.interrupt_flag)
                  {
                    CPT (cpt1U, 2); // interrupt pending
                    // clear interrupt, load interrupt pair into instruction 
                    // buffer; set INTERRUPT_EXEC_cycle.

                    // In the h/w this is done later, but doing it now allows 
                    // us to avoid clean up for no interrupt pending.

                    if (intr_pair_addr != 1) // no interrupts 
                      {

                        CPT (cpt1U, 3); // interrupt identified
                        sim_debug (DBG_INTR, & cpu_dev, "intr_pair_addr %u\n", 
                                   intr_pair_addr);

#ifndef SPEED
                        if_sim_debug (DBG_INTR, & cpu_dev) 
                          traceInstruction (DBG_INTR);
#endif

                        // get interrupt pair
                        core_read2 (intr_pair_addr, cpu.instr_buf, cpu.instr_buf + 1, __func__);

                        CPT (cpt1U, 4); // interrupt pair fetched
                        cpu.interrupt_flag = false;
                        setCpuCycle (INTERRUPT_EXEC_cycle);
                        break;
                      } // int_pair != 1
                  } // interrupt_flag

                // If we get here, there was no interrupt

                CPT (cpt1U, 5); // interrupt pair spurious
                cpu.interrupt_flag = false;
                clear_TEMPORARY_ABSOLUTE_mode ();
                // Restores addressing mode 
                cu_safe_restore ();
                // We can only get here if wasXfer was
                // false, so we can assume it still is.
                cpu.wasXfer = false;
// The only place cycle is set to INTERRUPT_cycle in FETCH_cycle; therefore
// we can safely assume that is the state that should be restored.
                setCpuCycle (FETCH_cycle);
              }
              break;

            case INTERRUPT_EXEC_cycle:
            case INTERRUPT_EXEC2_cycle:
              {
                //     execute instruction in instruction buffer
                //     if (! transfer) set INTERUPT_EXEC2_cycle 

                if (cpu.cycle == INTERRUPT_EXEC_cycle)
                  {
                    CPT (cpt1U, 6); // exec cycle
                    cpu.cu.IWB = cpu.instr_buf [0];
                  }
                else
                  {
                    CPT (cpt1U, 7); // exec2 cycle
                  cpu.cu.IWB = cpu.instr_buf [1];
                  }

                if (GET_I (cpu.cu.IWB))
                  cpu.wasInhibited = true;

                t_stat ret = executeInstruction ();

                if (ret > 0)
                  {
                     reason = ret;
                     break;
                  }

                if (ret == CONT_TRA)
                  {
                    CPT (cpt1U, 8); // was xfer
                    cpu.wasXfer = true;
                  }

                addCUhist ();

                if (ret == CONT_TRA)
                  {

// BAR mode:  [NBAR] is set ON (taking the processor
// out of BAR mode) by the execution of any transfer instruction
// other than tss during a fault or interrupt trap.

                    if (! (cpu.currentInstruction.opcode == 0715 &&
                           cpu.currentInstruction.opcodeX == 0))
                      {
                        CPT (cpt1U, 9); // nbar set
                        SET_I_NBAR;
                      }

                    setCpuCycle (FETCH_cycle);
                    if (!clear_TEMPORARY_ABSOLUTE_mode ())
                      {
sim_debug (DBG_TRACE, & cpu_dev, "setting ABS mode\n");
                        CPT (cpt1U, 10); // temporary absolute mode
                        set_addr_mode (ABSOLUTE_mode);
                      }
else sim_debug (DBG_TRACE, & cpu_dev, "not setting ABS mode\n");
                    break;
                  }

                if (ret == CONT_DIS)
                  {
                    // ISOLTS does this....
                    CPT (cpt1U, 11); // dis in interrupt pair
                    sim_warn ("DIS in interrupt cycle\n");
                    break;
                  }

                if (cpu.cycle == INTERRUPT_EXEC_cycle)
                  {
                    setCpuCycle (INTERRUPT_EXEC2_cycle);
                    break;
                  }
                clear_TEMPORARY_ABSOLUTE_mode ();
                cu_safe_restore ();
// The only place cycle is set to INTERRUPT_cycle in FETCH_cycle; therefore
// we can safely assume that is the state that should be restored.
                // We can only get here if wasXfer was
                // false, so we can assume it still is.
                cpu.wasXfer = false;
                CPT (cpt1U, 12); // cu restored
                setCpuCycle (FETCH_cycle);
              }
              break;

            case FETCH_cycle:
              {
#ifdef PANEL
                memset (cpu.cpt, 0, sizeof (cpu.cpt));
#endif
                CPT (cpt1U, 13); // fetch cycle

                PNL (L68_ (cpu.INS_FETCH = false;))

// "If the interrupt inhibit bit is not set in the currect instruction 
// word at the point of the next sequential instruction pair virtual
// address formation, the processor samples the [group 7 and interrupts]."

// Since we do not do concurrent instruction fetches, we must remember 
// the inhibit bits (cpu.wasInhibited).


// If the instruction pair virtual address being formed is the result of a 
// transfer of control condition or if the current instruction is 
// Execute (xec), Execute Double (xed), Repeat (rpt), Repeat Double (rpd), 
// or Repeat Link (rpl), the group 7 faults and interrupt present lines are 
// not sampled.

// Group 7 Faults
// 
// Shutdown
//
// An external power shutdown condition has been detected. DC POWER shutdown
// will occur in approximately one millisecond.
//
// Timer Runout
//
// The timer register has decremented to or through the value zero. If the
// processor is in privileged mode or absolute mode, recognition of this fault
// is delayed until a return to normal mode or BAR mode. Counting in the timer
// register continues.  
//
// Connect
//
// A connect signal ($CON strobe) has been received from a system controller.
// This event is to be distinguished from a Connect Input/Output Channel (cioc)
// instruction encountered in the program sequence.

                if ((! cpu.wasInhibited) &&
                    (cpu.PPR.IC % 2) == 0 &&
                    (! cpu.wasXfer) &&
                    (! (cpu.cu.xde | cpu.cu.xdo |
                        cpu.cu.rpt | cpu.cu.rd | cpu.cu.rl)))
                  {
                    CPT (cpt1U, 14); // sampling interrupts
                    cpu.interrupt_flag = sample_interrupts ();
                    //cpu.g7_flag = bG7Pending ();
                    // Don't check timer runout if in absolute mode, privledged, or
                    // interrupts inhibited.
                    bool noCheckTR = (get_addr_mode () == ABSOLUTE_mode) || 
                                      is_priv_mode ()  ||
                                      GET_I (cpu.cu.IWB);
                    cpu.g7_flag = noCheckTR ? bG7PendingNoTRO () : bG7Pending ();
                  }

                // The cpu.wasInhibited accumulates across the even and 
                // odd intruction. If the IC is even, reset it for
                // the next pair.

                if ((cpu.PPR.IC % 2) == 0)
                  cpu.wasInhibited = false;

                if (cpu.g7_flag)
                  {
                    cpu.g7_flag = false;
                    doG7Fault ();
                  }
                if (cpu.interrupt_flag)
                  {
// This is the only place cycle is set to INTERRUPT_cycle; therefore
// return from interrupt can safely assume the it should set the cycle
// to FETCH_cycle.
                    CPT (cpt1U, 15); // interrupt
                    setCpuCycle (INTERRUPT_cycle);
                    break;
                  }
                cpu.lufCounter ++;

                // Assume CPU clock ~ 1Mhz. lockup time is 32 ms
                if (cpu.lufCounter > 32000)
                  {
                    CPT (cpt1U, 16); // LUF
                    cpu.lufCounter = 0;
                    doFault (FAULT_LUF, (_fault_subtype) {.bits=0}, "instruction cycle lockup");
                  }

#if 0
                if (cpu.interrupt_flag && 
                    ((cpu.PPR.IC % 2) == 0) &&
                    (! (cpu.cu.xde | cpu.cu.xdo |
                        cpu.cu.rpt | cpu.cu.rd | cpu.cu.rl)))
                  {
// This is the only place cycle is set to INTERRUPT_cycle; therefore
// return from interrupt can safely assume the it should set the cycle
// to FETCH_cycle.
                    setCpuCycle (INTERRUPT_cycle);
                    break;
                  }
                if (cpu.g7_flag)
                  {
                    cpu.g7_flag = false;
                    //setCpuCycle (FAULT_cycle);
                    doG7Fault ();
                  }
#endif

                // If we have done the even of an XED, do the odd
                if (cpu.cu.xde == 0 && cpu.cu.xdo == 1)
                  {
                    CPT (cpt1U, 17); // do XED odd
                    // Get the odd
                    cpu.cu.IWB = cpu.cu.IRODD;
                    // Do nothing next time
                    cpu.cu.xde = cpu.cu.xdo = 0;
                    cpu.isExec = true;
                    cpu.isXED = true;
                  }
                // If we have done neither of the XED
                else if (cpu.cu.xde == 1 && cpu.cu.xdo == 1)
                  {
                    CPT (cpt1U, 18); // do XED even
                    // Do the even this time and the odd the next time
                    cpu.cu.xde = 0;
                    cpu.cu.xdo = 1;
                    cpu.isExec = true;
                    cpu.isXED = true;
                  }
                // If we have not yet done the XEC
                else if (cpu.cu.xde == 1)
                  {
                    CPT (cpt1U, 19); // do XEC
                    // do it this time, and nothing next time
                    cpu.cu.xde = cpu.cu.xdo = 0;
                    cpu.isExec = true;
                    cpu.isXED = false;
                  }
                else
                  {
                    CPT (cpt1U, 20); // not XEC or RPx
                    cpu.isExec = false;
                    cpu.isXED = false;
                    //processorCycle = INSTRUCTION_FETCH;
                    // fetch next instruction into current instruction struct
                    clr_went_appending (); // XXX not sure this is the right place
                    cpu.cu.TSN_VALID [0] = 0;
                    PNL (cpu.prepare_state = ps_PIA);
                    PNL (L68_ (cpu.INS_FETCH = true;))
                    fetchInstruction (cpu.PPR.IC);
                  }


#if 0
                // XXX The conditions are more rigorous: see AL39, pg 327
           // ci is not set up yet; check the inhibit bit in the IWB!
                //if (PPR.IC % 2 == 0 && // Even address
                    //ci -> i == 0) // Not inhibited
                //if (GET_I (cpu.cu.IWB) == 0) // Not inhibited
// If the instruction pair virtual address being formed is the result of a 
// transfer of control condition or if the current instruction is 
// Execute (xec), Execute Double (xed), Repeat (rpt), Repeat Double (rpd), 
// or Repeat Link (rpl), the group 7 faults and interrupt present lines are 
// not sampled.
                if (PPR.IC % 2 == 0 && // Even address
                    GET_I (cpu.cu.IWB) == 0 &&  // Not inhibited
                    (! (cpu.cu.xde | cpu.cu.xdo |
                        cpu.cu.rpt | cpu.cu.rd | cpu.cu.rl)))
                  {
                    cpu.interrupt_flag = sample_interrupts ();
                    cpu.g7_flag = bG7Pending ();
                  }
                else
                  {
                    cpu.interrupt_flag = false;
                    cpu.g7_flag = false;
                  }
#endif

                CPT (cpt1U, 21); // go to exec cycle
                advanceG7Faults ();
                setCpuCycle (EXEC_cycle);
              }
              break;

            case EXEC_cycle:
              {
                CPT (cpt1U, 22); // exec cycle

                // The only time we are going to execute out of IRODD is
                // during RPD, at which time interrupts are automatically
                // inhibited; so the following can igore RPD harmelessly
                if (GET_I (cpu.cu.IWB))
                  cpu.wasInhibited = true;

                t_stat ret = executeInstruction ();

                CPT (cpt1U, 23); // execution complete
                if (ret > 0)
                  {
                     reason = ret;
                     break;
                  }

                if (ret == CONT_TRA)
                  {
                    CPT (cpt1U, 24); // transfer instruction
                    cpu.cu.xde = cpu.cu.xdo = 0;
                    cpu.isExec = false;
                    cpu.isXED = false;
                    cpu.wasXfer = true;
                    if (TST_I_ABS && get_went_appending ())
                      {
                        set_addr_mode (APPEND_mode);
                      }

                    setCpuCycle (FETCH_cycle);
                    break;   // don't bump PPR.IC, instruction already did it
                  }

                if (ret == CONT_DIS)
                  {
                    CPT (cpt1U, 25); // DIS instruction


// If we get here, we have encountered a DIS instruction in EXEC_cycle.
//
// We need to idle the CPU until one of the following conditions:
//
//  An external interrupt occurs.
//  The Timer Register underflows.
//  The emulator polled devices need polling.
//
// The external interrupt will only be posted to the CPU engine if the
// device poll posts an interrupt. This means that we do not need to
// detect the interrupts here; if we wake up and poll the devices, the 
// interrupt will be detected by the DIS instruction when it is re-executed.
//
// The Timer Register is a fast, high-precision timer but Multics uses it 
// in only two ways: detecting I/O lockup during early boot, and process
// quantum scheduling (1/4 second for Multics).
//
// Neither of these require high resolution or high accuracy.
//
// The goal of the polling code is sample at about 100Hz; updating the timer
// register at that rate should suffice.
//
//    sleep for 1/100 of a second
//    update the polling state to trigger a poll
//    update the timer register by 1/100 of a second
//    force the simh queues to process
//    continue processing
//


// The usleep logic is not smart enough w.r.t. ROUND_ROBIN/ISOLTS.
// The sleep should only happen if all running processors are in
// DIS mode.
#ifndef ROUND_ROBIN
                    // 1/100 is .01 secs.
                    // *1000 is 10  milliseconds
                    // *1000 is 10000 microseconds
                    // in uSec;
                    usleep (sys_opts.sys_poll_interval * 1000/*10000*/);
#ifdef EV_POLL
                    // Trigger I/O polling
                    uv_run (ev_poll_loop, UV_RUN_NOWAIT);
                    fastQueueSubsample = 0;
#endif

#ifndef EV_POLL
                    // this ignores the amount of time since the last poll;
                    // worst case is the poll delay of 1/50th of a second.
                    slowQueueSubsample += 10240; // ~ 1Hz
                    queueSubsample += 10240; // ~100Hz
#endif

                    sim_interval = 0;
                    // Timer register runs at 512 KHz
                    // 512000 is 1 second
                    // 512000/100 -> 5120  is .01 second
         
                    // Would we have underflowed while sleeping?
                    //if (cpu.rTR <= 5120)

                    // Timer register runs at 512 KHz
                    // 512Khz / 512 is millisecods
                    if (cpu.rTR <= sys_opts.sys_poll_interval * 512)
                    
                      {
                        if (cpu.switches.tro_enable)
                          setG7fault (currentRunningCPUnum, FAULT_TRO, (_fault_subtype) {.bits=0});
                      }
                    cpu.rTR = (cpu.rTR - 5120) & MASK27;
#endif
                    break;
                  }

                cpu.wasXfer = false;

                if (ret < 0)
                  {
                    sim_printf ("execute instruction returned %d?\n", ret);
                    break;
                  }

                if ((! cpu.cu.repeat_first) &&
                    (cpu.cu.rpt ||
                     (cpu.cu.rd && (cpu.PPR.IC & 1)) ||
                     cpu.cu.rl))
                  {
                    CPT (cpt1U, 26); // RPx instruction
                    if (cpu.cu.rd)
                      -- cpu.PPR.IC;
                    cpu.wasXfer = false; 
                    setCpuCycle (FETCH_cycle);
                    break;
                  }

                if (cpu.cu.xde || cpu.cu.xdo) // we are starting or are in an XEC/XED
                  {
                    CPT (cpt1U, 27); // XEx instruction
                    cpu.wasXfer = false; 
                    setCpuCycle (FETCH_cycle);
                    break;
                  }

                cpu.cu.xde = cpu.cu.xdo = 0;
                cpu.isExec = false;
                cpu.isXED = false;
                cpu.PPR.IC ++;
                if (ci->info->ndes > 0)
                  cpu.PPR.IC += ci->info->ndes;

                CPT (cpt1U, 28); // enter fetch cycle
                cpu.wasXfer = false; 
                setCpuCycle (FETCH_cycle);
              }
              break;

            case SYNC_FAULT_RTN_cycle:
              {
                CPT (cpt1U, 29); // sync. fault return
                cpu.wasXfer = false; 
                // cu_safe_restore should have restored CU.IWB, so
                // we can determine the instruction length.
                // decodeInstruction() restores ci->info->ndes
                decodeInstruction (IWB_IRODD, & cpu.currentInstruction);

                cpu.PPR.IC += ci->info->ndes;
                cpu.PPR.IC ++;
                cpu.wasXfer = false; 
                setCpuCycle (FETCH_cycle);
              }
              break;

            case FAULT_cycle:
              {
                CPT (cpt1U, 30); // fault cycle
#if 0
                // Interrupts need to be processed at the beginning of the
                // FAULT CYCLE as part of the H/W 'fetch instruction pair.'

                cpu.interrupt_flag = sample_interrupts ();
                cpu.g7_flag = bG7Pending ();
#endif
                // In the FAULT CYCLE, the processor safe-stores the Control
                // Unit Data (see Section 3) into program-invisible holding
                // registers in preparation for a Store Control Unit ( scu)
                // instruction, then enters temporary absolute mode, forces the
                // current ring of execution C(PPR.PRR) to 0, and generates a
                // computed address for the fault trap pair by concatenating
                // the setting of the FAULT BASE switches on the processor
                // configuration panel with twice the fault number (see Table
                // 7-1).  This computed address and the operation code for the
                // Execute Double (xed) instruction are forced into the
                // instruction register and executed as an instruction. Note
                // that the execution of the instruction is not done in a
                // normal EXECUTE CYCLE but in the FAULT CYCLE with the
                // processor in temporary absolute mode.

                //sim_debug (DBG_FAULT, & cpu_dev, "fault cycle [%"PRId64"]\n", sim_timell ());
    

#if EMULATOR_ONLY
                if (cpu.switches.report_faults == 1 ||
                    (cpu.switches.report_faults == 2 &&
                     cpu.faultNumber == FAULT_OFL))
                  {
#ifdef TESTING
                    emCallReportFault ();
#endif
                    clearFaultCycle ();
                    cpu.wasXfer = false; 
                    setCpuCycle (FETCH_cycle);
                    cpu.PPR.IC += ci->info->ndes;
                    cpu.PPR.IC ++;
                    break;
                  }
#endif

                cu_safe_store ();
                CPT (cpt1U, 31); // safe store complete

                // Temporary absolute mode
                set_TEMPORARY_ABSOLUTE_mode ();

                // Set to ring 0
                cpu.PPR.PRR = 0;
                cpu.TPR.TRR = 0;

                // (12-bits of which the top-most 7-bits are used)
                uint fltAddress = (cpu.switches.FLT_BASE << 5) & 07740;
#ifdef L68
                if (cpu.is_FFV)
                  {
                    cpu.is_FFV = false;
                    CPTUR (cptUseMR);
                    // The high 15 bits
                    fltAddress = (cpu.MR.FFV & MASK15) << 3;
//IF1 sim_printf ("fltAddress %06o\n", fltAddress);
                  }
#endif

                // absolute address of fault YPair
                word24 addr = fltAddress + 2 * cpu.faultNumber;
  
                core_read2 (addr, cpu.instr_buf, cpu.instr_buf + 1, __func__);

                CPT (cpt1U, 33); // set fault exec cycle
                setCpuCycle (FAULT_EXEC_cycle);

                break;
              }

            case FAULT_EXEC_cycle:
            case FAULT_EXEC2_cycle:
              {
                //     execute instruction in instruction buffer
                //     if (! transfer) set INTERUPT_EXEC2_cycle 

                if (cpu.cycle == FAULT_EXEC_cycle)
                  {
                    CPT (cpt1U, 34); // fault exec even cycle
                    cpu.cu.IWB = cpu.instr_buf [0];
                  }
                else
                  {
                    CPT (cpt1U, 35); // fault exec odd cycle
                    cpu.cu.IWB = cpu.instr_buf [1];
                  }

                if (GET_I (cpu.cu.IWB))
                  cpu.wasInhibited = true;

                t_stat ret = executeInstruction ();

                CPT (cpt1L, 0); // fault instruction complete
                if (ret > 0)
                  {
                     reason = ret;
                     break;
                  }

                if (ret == CONT_TRA)
                  {
                    CPT (cpt1L, 1); // fault instruction was transfer
                    //sim_debug (DBG_TRACE, & cpu_dev, "tra in fault\n");
                    //sim_debug (DBG_TRACE,& cpu_dev,
                                //"fault CONT_TRA; was_appending %d\n",
                                //get_went_appending () ? 1 : 0);

// BAR mode:  [NBAR] is set ON (taking the processor
// out of BAR node) by the execution of any transfer instruction
// other than tss during a fault or interrupt trap.

                    if (! (cpu.currentInstruction.opcode == 0715 &&
                           cpu.currentInstruction.opcodeX == 0))
                      {
                        SET_I_NBAR;
                      }

                    cpu.wasXfer = true; 
                    setCpuCycle (FETCH_cycle);
                    clearFaultCycle ();
                    if (!clear_TEMPORARY_ABSOLUTE_mode ())
                      {
                        //sim_debug (DBG_TRACE, & cpu_dev, "tra in fault sets ABSOLUTE_mode\n");
                        //brkbrk(0, NULL);
                        //sim_debug (DBG_TRACE, & cpu_dev,
                                   //"CONR_TRA: went_appending was false, so setting absolute mode\n");
                        CPT (cpt1L, 2); // set abs mode
                        set_addr_mode (ABSOLUTE_mode);
                      }
                    break;
                  }

                if (ret == CONT_DIS)
                  {
                    CPT (cpt1L, 3); // DIS in fault pait
                    // ISOLTS does this....
                    sim_warn ("DIS in fault cycle\n");
                    break;
                  }

                if (cpu.cycle == FAULT_EXEC_cycle)
                  {
                    CPT (cpt1L, 4); // go to fault exec odd cycle
                    setCpuCycle (FAULT_EXEC2_cycle);
                    break;
                  }
                CPT (cpt1L, 5); // go to exec cycle
                // Done with FAULT_EXEC2_cycle
                // Restores cpu.cycle and addressing mode
                clear_TEMPORARY_ABSOLUTE_mode ();
                cu_safe_restore ();
                cpu.wasXfer = false; 
                setCpuCycle (FETCH_cycle);
                clearFaultCycle ();

// XXX Is this needed? Are EIS instructions allowed in fault pairs?

                // cu_safe_restore should have restored CU.IWB, so
                // we can determine the instruction length.
                // decodeInstruction() restores ci->info->ndes
                decodeInstruction (IWB_IRODD, & cpu.currentInstruction);

                cpu.PPR.IC += ci->info->ndes;
                cpu.PPR.IC ++;
                break;
              }

#if 0
            default:
              {
                sim_printf ("cpu.cycle %d?\n", cpu.cycle);
                return SCPE_UNK;
              }
#endif
          }  // switch (cpu.cycle)
      } 
#ifdef ROUND_ROBIN
    while (0);
   if (reason == 0)
     goto setCPU;
#else
    while (reason == 0);
#endif

leave:

#ifdef HDBG
    hdbgPrint ();
#endif
    sim_printf("\nsimCycles = %"PRId64"\n", sim_timell ());
    sim_printf("\ncpuCycles = %"PRId64"\n", sys_stats.total_cycles);
    for (int i = 0; i < N_FAULTS; i ++)
      {
        if (sys_stats.total_faults [i])
          sim_printf("%s faults = %"PRId64"\n", faultNames [i], sys_stats.total_faults [i]);
     }
    
#ifdef M_SHARED
// simh needs to have the IC statically allocated, so a placeholder was
// created. Update the placeholder in so the IC can be seen by simh, and
// restarting sim_instr doesn't lose the place.

    setCPUnum (0);
    dummyIC = cpu.PPR.IC;
#endif

    return reason;
  }

/*!
 cd@libertyhaven.com - sez ....
 If the instruction addresses a block of four words, the target of the
instruction is supposed to be an address that is aligned on a four-word
boundary (0 mod 4). If not, the processor will grab the four-word block
containing that address that begins on a four-word boundary, even if it has to
go back 1 to 3 words. Analogous explanation for 8, 16, and 32 cases.
 
 olin@olinsibert.com - sez ...
 It means that the appropriate low bits of the address are forced to zero. So
it's the previous words, not the succeeding words, that are used to satisfy the
request.
 
 -- Olin

 */

int OPSIZE (void)
{
    DCDstruct * i = & cpu.currentInstruction;
    if (i->info->flags & (READ_OPERAND | STORE_OPERAND))
        return 1;
    else if (i->info->flags & (READ_YPAIR | STORE_YPAIR))
        return 2;
    else if (i->info->flags & (READ_YBLOCK8 | STORE_YBLOCK8))
        return 8;
    else if (i->info->flags & (READ_YBLOCK16 | STORE_YBLOCK16))
        return 16;
    else if (i->info->flags & (READ_YBLOCK32 | STORE_YBLOCK32))
        return 32;
    return 0;
}

// read instruction operands
t_stat ReadOP (word18 addr, _processor_cycle_type cyctyp)
{
    CPT (cpt1L, 6); // ReadOP

    switch (OPSIZE ())
    {
        case 1:
            CPT (cpt1L, 7); // word
            Read (addr, &cpu.CY, cyctyp);
            return SCPE_OK;
        case 2:
            CPT (cpt1L, 8); // double word
            addr &= 0777776;   // make even
            Read2 (addr, cpu.Ypair, cyctyp);
            break;
        case 8:
            CPT (cpt1L, 9); // oct word
            addr &= 0777770;   // make on 8-word boundary
            Read8 (addr, cpu.Yblock8, false);
            break;
        case 16:
            CPT (cpt1L, 10); // 16 words
            addr &= 0777770;   // make on 8-word boundary
            Read16 (addr, cpu.Yblock16);
            break;
        case 32:
#ifdef TESTING
{ static bool first = true;
if (first) {
first = false;
sim_printf ("XXX Read32 w.r.t. lastCycle == indirect\n");
}}
#endif
            CPT (cpt1L, 11); // 32 words
            addr &= 0777740;   // make on 32-word boundary
            for (uint j = 0 ; j < 32 ; j += 1)
                Read (addr + j, cpu.Yblock32 + j, cyctyp);
            
            break;
    }
    //cpu.TPR.CA = addr;  // restore address
    
    return SCPE_OK;

}

// write instruction operands
t_stat WriteOP(word18 addr, UNUSED _processor_cycle_type cyctyp)
{
    switch (OPSIZE ())
    {
        case 1:
            CPT (cpt1L, 12); // word
            Write (addr, cpu.CY, OPERAND_STORE);
            break;
        case 2:
            CPT (cpt1L, 13); // double word
            addr &= 0777776;   // make even
            Write2 (addr + 0, cpu.Ypair, OPERAND_STORE);
            break;
        case 8:
            CPT (cpt1L, 14); // 8 words
            addr &= 0777770;   // make on 8-word boundary
            Write8 (addr, cpu.Yblock8, false);
            break;
        case 16:
            CPT (cpt1L, 15); // 16 words
            addr &= 0777770;   // make on 8-word boundary
            Write16 (addr, cpu.Yblock16);
            break;
        case 32:
            CPT (cpt1L, 16); // 32 words
            addr &= 0777740;   // make on 32-word boundary
            for (uint j = 0 ; j < 32 ; j += 1)
                Write (addr + j, cpu.Yblock32[j], OPERAND_STORE);
            break;
    }
    
    return SCPE_OK;
    
}

t_stat memWatch (int32 arg, const char * buf)
  {
    //sim_printf ("%d <%s>\n", arg, buf);
    if (strlen (buf) == 0)
      {
        if (arg)
          {
            sim_printf ("no argument to watch?\n");
            return SCPE_ARG;
          }
        sim_printf ("Clearing all watch points\n");
        memset (& watchBits, 0, sizeof (watchBits));
        return SCPE_OK;
      }
    char * end;
    long int n = strtol (buf, & end, 0);
    if (* end || n < 0 || n >= MEMSIZE)
      {
        sim_printf ("invalid argument to watch?\n");
        return SCPE_ARG;
      }
    watchBits [n] = arg != 0;
    return SCPE_OK;
  }

/*!
 * "Raw" core interface ....
 */

#ifndef SPEED
static void nem_check (word24 addr, char * context)
  {
    if (query_scbank_map (addr) < 0)
      {
        //sim_printf ("nem %o [%"PRId64"]\n", addr, sim_timell ());
        doFault (FAULT_STR, (_fault_subtype) {.fault_str_subtype=flt_str_nea},  context);
      }
  }
#endif

#ifndef SPEED
int32 core_read(word24 addr, word36 *data, const char * ctx)
{
    PNL (cpu.portBusy = true;)
#ifdef ISOLTS
    if (cpu.switches.useMap)
      {
        uint pgnum = addr / SCBANK;
        int os = cpu.scbank_pg_os [pgnum];
//sim_printf ("%s addr %08o pgnum %06o os %6d new %08o\n", __func__, addr, pgnum, os, os + addr % SCBANK);
        if (os < 0)
          { 
            doFault (FAULT_STR, (_fault_subtype) {.fault_str_subtype=flt_str_nea},  __func__);
          }
        addr = (uint) os + addr % SCBANK;
      }
    else
#endif
      nem_check (addr,  "core_read nem");

#if 0 // XXX Controlled by TEST/NORMAL switch
#ifdef ISOLTS
    if (cpu.MR.sdpap)
      {
        sim_warn ("failing to implement sdpap\n");
        cpu.MR.sdpap = 0;
      }
    if (cpu.MR.separ)
      {
        sim_warn ("failing to implement separ\n");
        cpu.MR.separ = 0;
      }
#endif
#endif
    if (M[addr] & MEM_UNINITIALIZED)
      {
        sim_debug (DBG_WARN, & cpu_dev, "Unitialized memory accessed at address %08o; IC is 0%06o:0%06o (%s(\n", addr, cpu.PPR.PSR, cpu.PPR.IC, ctx);
      }
    if (watchBits [addr])
    //if (watchBits [addr] && M[addr]==0)
      {
        //sim_debug (0, & cpu_dev, "read   %08o %012"PRIo64" (%s)\n",addr, M [addr], ctx);
        sim_printf ("WATCH [%"PRId64"] read   %08o %012"PRIo64" (%s)\n", sim_timell (), addr, M [addr], ctx);
        traceInstruction (0);
      }
    *data = M[addr] & DMASK;
    sim_debug (DBG_CORE, & cpu_dev,
               "core_read  %08o %012"PRIo64" (%s)\n",
                addr, * data, ctx);
    PNL (trackport (addr, * data));
    return 0;
}
#endif

#ifndef SPEED
int core_write(word24 addr, word36 data, const char * ctx) {
    PNL (cpu.portBusy = true;)
#ifdef ISOLTS
    if (cpu.switches.useMap)
      {
        uint pgnum = addr / SCBANK;
        int os = cpu.scbank_pg_os [pgnum];
        if (os < 0)
          { 
            doFault (FAULT_STR, (_fault_subtype) {.fault_str_subtype=flt_str_nea},  __func__);
          }
        addr = (uint) os + addr % SCBANK;
      }
    else
#endif
      nem_check (addr,  "core_write nem");
#ifdef ISOLTS
    if (cpu.MR.sdpap)
      {
        sim_warn ("failing to implement sdpap\n");
        cpu.MR.sdpap = 0;
      }
    if (cpu.MR.separ)
      {
        sim_warn ("failing to implement separ\n");
        cpu.MR.separ = 0;
      }
#endif
    M[addr] = data & DMASK;
    if (watchBits [addr])
    //if (watchBits [addr] && M[addr]==0)
      {
        //sim_debug (0, & cpu_dev, "write  %08o %012"PRIo64" (%s)\n",addr, M [addr], ctx);
        sim_printf ("WATCH [%"PRId64"] write  %08o %012"PRIo64" (%s)\n", sim_timell (), addr, M [addr], ctx);
        traceInstruction (0);
      }
    sim_debug (DBG_CORE, & cpu_dev,
               "core_write %08o %012"PRIo64" (%s)\n",
                addr, data, ctx);
    PNL (trackport (addr, data));
    return 0;
}
#endif

#ifndef SPEED
int core_read2(word24 addr, word36 *even, word36 *odd, const char * ctx) {
    PNL (cpu.portBusy = true;)
    if(addr & 1) {
        sim_debug(DBG_MSG, &cpu_dev,"warning: subtracting 1 from pair at %o in core_read2 (%s)\n", addr, ctx);
        addr &= (word24)~1; /* make it an even address */
    }
#ifdef ISOLTS
    if (cpu.switches.useMap)
      {
        uint pgnum = addr / SCBANK;
        int os = cpu.scbank_pg_os [pgnum];
//sim_printf ("%s addr %08o pgnum %06o os %6d new %08o\n", __func__, addr, pgnum, os, os + addr % SCBANK);
        if (os < 0)
          { 
            doFault (FAULT_STR, (_fault_subtype) {.fault_str_subtype=flt_str_nea},  __func__);
          }
        addr = (uint) os + addr % SCBANK;
      }
    else
#endif
      nem_check (addr,  "core_read2 nem");

#if 0 // XXX Controlled by TEST/NORMAL switch
#ifdef ISOLTS
    if (cpu.MR.sdpap)
      {
        sim_warn ("failing to implement sdpap\n");
        cpu.MR.sdpap = 0;
      }
    if (cpu.MR.separ)
      {
        sim_warn ("failing to implement separ\n");
        cpu.MR.separ = 0;
      }
#endif
#endif
    if (M[addr] & MEM_UNINITIALIZED)
    {
        sim_debug (DBG_WARN, & cpu_dev, "Unitialized memory accessed at address %08o; IC is 0%06o:0%06o (%s)\n", addr, cpu.PPR.PSR, cpu.PPR.IC, ctx);
    }
    if (watchBits [addr])
    //if (watchBits [addr] && M[addr]==0)
      {
        //sim_debug (0, & cpu_dev, "read2  %08o %012"PRIo64" (%s)\n",addr, M [addr], ctx);
        sim_printf ("WATCH [%"PRId64"] read2  %08o %012"PRIo64" (%s)\n", sim_timell (), addr, M [addr], ctx);
        traceInstruction (0);
      }
    *even = M[addr++] & DMASK;
    sim_debug (DBG_CORE, & cpu_dev,
               "core_read2 %08o %012"PRIo64" (%s)\n",
                addr - 1, * even, ctx);
    PNL (trackport (addr - 1, * even));

    // if the even address is OK, the odd will be
    //nem_check (addr,  "core_read2 nem");
    if (M[addr] & MEM_UNINITIALIZED)
    {
        sim_debug (DBG_WARN, & cpu_dev, "Unitialized memory accessed at address %08o; IC is 0%06o:0%06o (%s)\n", addr, cpu.PPR.PSR, cpu.PPR.IC, ctx);
    }
    if (watchBits [addr])
    //if (watchBits [addr] && M[addr]==0)
      {
        //sim_debug (0, & cpu_dev, "read2  %08o %012"PRIo64" (%s)\n",addr, M [addr], ctx);
        sim_printf ("WATCH [%"PRId64"] read2  %08o %012"PRIo64" (%s)\n", sim_timell (), addr, M [addr], ctx);
        traceInstruction (0);
      }

    *odd = M[addr] & DMASK;
    sim_debug (DBG_CORE, & cpu_dev,
               "core_read2 %08o %012"PRIo64" (%s)\n",
                addr, * odd, ctx);
    PNL (trackport (addr, * odd));
    return 0;
}
#endif
//
////! for working with CY-pairs
//int core_read72(word24 addr, word72 *dst) // needs testing
//{
//    word36 even, odd;
//    if (core_read2(addr, &even, &odd) == -1)
//        return -1;
//    *dst = ((word72)even << 36) | (word72)odd;
//    return 0;
//}
//
#ifndef SPEED
int core_write2(word24 addr, word36 even, word36 odd, const char * ctx) {
    PNL (cpu.portBusy = true;)
    if(addr & 1) {
        sim_debug(DBG_MSG, &cpu_dev, "warning: subtracting 1 from pair at %o in core_write2 (%s)\n", addr, ctx);
        addr &= (word24)~1; /* make it even a dress, or iron a skirt ;) */
    }
#ifdef ISOLTS
    if (cpu.switches.useMap)
      {
        uint pgnum = addr / SCBANK;
        int os = cpu.scbank_pg_os [pgnum];
        if (os < 0)
          { 
            doFault (FAULT_STR, (_fault_subtype) {.fault_str_subtype=flt_str_nea},  __func__);
          }
        addr = (word24)os + addr % SCBANK;
      }
    else
#endif
      nem_check (addr,  "core_write2 nem");
#ifdef ISOLTS
    if (cpu.MR.sdpap)
      {
        sim_warn ("failing to implement sdpap\n");
        cpu.MR.sdpap = 0;
      }
    if (cpu.MR.separ)
      {
        sim_warn ("failing to implement separ\n");
        cpu.MR.separ = 0;
      }
#endif

    if (watchBits [addr])
    //if (watchBits [addr] && even==0)
      {
        //sim_debug (0, & cpu_dev, "write2 %08o %012"PRIo64" (%s)\n",addr, even, ctx);
        sim_printf ("WATCH [%"PRId64"] write2 %08o %012"PRIo64" (%s)\n", sim_timell (), addr, even, ctx);
        traceInstruction (0);
      }
    M[addr++] = even;
    PNL (trackport (addr - 1, even));
    sim_debug (DBG_CORE, & cpu_dev,
               "core_write2 %08o %012"PRIo64" (%s)\n",
                addr - 1, even, ctx);

    // If the even address is OK, the odd will be
    //nem_check (addr,  "core_write2 nem");

    if (watchBits [addr])
    //if (watchBits [addr] && odd==0)
      {
        //sim_debug (0, & cpu_dev, "write2 %08o %012"PRIo64" (%s)\n",addr, odd, ctx);
        sim_printf ("WATCH [%"PRId64"] write2 %08o %012"PRIo64" (%s)\n", sim_timell (), addr, odd, ctx);
        traceInstruction (0);
      }
    M[addr] = odd;
    PNL (trackport (addr, odd));
    sim_debug (DBG_CORE, & cpu_dev,
               "core_write2 %08o %012"PRIo64" (%s)\n",
                addr, odd, ctx);
    return 0;
}
#endif

//#define MM
#if 1   //def MM


#ifndef QUIET_UNUSED
//=============================================================================

/*
 * encode_instr()
 *
 * Convert an instr_t struct into a  36-bit word.
 *
 */

void encode_instr(const instr_t *ip, word36 *wordp)
{
    *wordp = 0;
    putbits36_18 (wordp, 0, ip->addr);
#if 1
    putbits36_10 (wordp, 18, ip->opcode);
#else
    putbits36_9 (*wordp, 18, ip->opcode & 0777);
    putbits36_1 (*wordp, 27, ip->opcode >> 9);
#endif
    putbits36_1 (wordp, 28, ip->inhibit);
    if (! is_eis[ip->opcode&MASKBITS(10)]) {
        putbits36_1 (wordp, 29, ip->mods.single.pr_bit);
        putbits36_6 (wordp, 30, ip->mods.single.tag);
    } else {
        putbits36_1 (wordp, 29, ip->mods.mf1.ar);
        putbits36_1 (wordp, 30, ip->mods.mf1.rl);
        putbits36_1 (wordp, 31, ip->mods.mf1.id);
        putbits36_4 (wordp, 32, ip->mods.mf1.reg);
    }
}
#endif


#endif // MM
    

/*
 * instruction fetcher ...
 * fetch + decode instruction at 18-bit address 'addr'
 */

/*
 * instruction decoder .....
 *
 * if dst is not NULL place results into dst, if dst is NULL plae results into global currentInstruction
 */
void decodeInstruction (word36 inst, DCDstruct * p)
{
    CPT (cpt1L, 17); // instruction decoder
    p->opcode  = GET_OP(inst);  // get opcode
    p->opcodeX = GET_OPX(inst); // opcode extension
    p->address = GET_ADDR(inst);// address field from instruction
    p->b29     = GET_A(inst);   // "A" - the indirect via pointer register flag
    p->i       = GET_I(inst);   // "I" - inhibit interrupt flag
    p->tag     = GET_TAG(inst); // instruction tag
    
    p->info = getIWBInfo(p);     // get info for IWB instruction
    
    // HWR 18 June 2013 
    //p->info->opcode = p->opcode;
    //p->IWB = inst;
    
    // HWR 21 Dec 2013
    if (p->info->flags & IGN_B29)
        p->b29 = 0;   // make certain 'a' bit is valid always

    if (p->info->ndes > 0)
    {
        p->b29 = 0;
        p->tag = 0;
        if (p->info->ndes > 1)
        {
            memset (& cpu.currentEISinstruction, 0, sizeof (cpu.currentEISinstruction)); 
        }
    }

    // Save the RFI
    p -> restart = cpu.cu.rfi != 0;
    cpu.cu.rfi = 0;
    if (p -> restart)
      {
        sim_debug (DBG_TRACE, & cpu_dev, "restart\n");
      }
}

// MM stuff ...

//
// is_priv_mode()
//
// Report whether or or not the CPU is in privileged mode.
// True if in absolute mode or if priv bit is on in segment TPR.TSR
// The processor executes instructions in privileged mode when forming
// addresses in absolute mode or when forming addresses in append mode and the
// segment descriptor word (SDW) for the segment in execution specifies a
// privileged procedure and the execution ring is equal to zero.
//
// PPR.P A flag controlling execution of privileged instructions.
//
// Its value is 1 (permitting execution of privileged instructions) if PPR.PRR
// is 0 and the privileged bit in the segment descriptor word (SDW.P) for the
// procedure is 1; otherwise, its value is 0.
//
 
int is_priv_mode(void)
  {
sim_debug (DBG_TRACE, & cpu_dev, "is_priv_mode P %u get_addr_mode %d get_bar_mode %d IR %06o\n", cpu.PPR.P, get_addr_mode (), get_bar_mode (), cpu.cu.IR);
    if (TST_I_NBAR && cpu.PPR.P)
      return 1;
    
// Back when it was ABS/APP/BAR, this test was right; now that
// it is ABS/APP,BAR/NBAR, check bar mode.
// Fixes ISOLTS 890 05a.
    if (get_addr_mode () == ABSOLUTE_mode && ! get_bar_mode ())
      return 1;

    return 0;
  }

void set_went_appending (void)
  {
    CPT (cpt1L, 18); // set went appending
    cpu.went_appending = true;
  }

void clr_went_appending (void)
  {
    CPT (cpt1L, 19); // clear went appending
    cpu.went_appending = false;
  }

bool get_went_appending (void)
  {
    return cpu.went_appending;
  }

/*
 * addr_modes_t get_addr_mode()
 *
 * Report what mode the CPU is in.
 * This is determined by examining a couple of IR flags.
 *
 * TODO: get_addr_mode() probably belongs in the CPU source file.
 *
 */

static void set_TEMPORARY_ABSOLUTE_mode (void)
{
    CPT (cpt1L, 20); // set temp. abs. mode
    cpu.secret_addressing_mode = true;
    cpu.went_appending = false;
}

static bool clear_TEMPORARY_ABSOLUTE_mode (void)
{
    CPT (cpt1L, 21); // clear temp. abs. mode
    cpu.secret_addressing_mode = false;
    //sim_debug (DBG_TRACE, & cpu_dev, "clear_TEMPORARY_ABSOLUTE_mode returns %s\n", cpu.went_appending ? "true" : "false");
    return cpu.went_appending;
}

/* 
 * get_bar_mode: During fault processing, we do not want to fetch and execute the fault vector instructions
 *   in BAR mode. We leverage the secret_addressing_mode flag that is set in set_TEMPORARY_ABSOLUTE_MODE to
 *   direct us to ignore the I_NBAR indicator register.
 */
bool get_bar_mode(void) {
  return !(cpu.secret_addressing_mode || TST_I_NBAR);
}

addr_modes_t get_addr_mode(void)
{
    if (cpu.secret_addressing_mode)
        return ABSOLUTE_mode; // This is not the mode you are looking for

    if (cpu.went_appending)
        return APPEND_mode;

    if (TST_I_ABS)
      {
          return ABSOLUTE_mode;
      }
    else
      {
          return APPEND_mode;
      }
}


/*
 * set_addr_mode()
 *
 * Put the CPU into the specified addressing mode.   This involves
 * setting a couple of IR flags and the PPR priv flag.
 *
 */

void set_addr_mode(addr_modes_t mode)
{
    cpu.went_appending = false;
// Temporary hack to fix fault/intr pair address mode state tracking
//   1. secret_addressing_mode is only set in fault/intr pair processing.
//   2. Assume that the only set_addr_mode that will occur is the b29 special
//   case or ITx.
    //if (secret_addressing_mode && mode == APPEND_mode)
      //set_went_appending ();

    cpu.secret_addressing_mode = false;
    if (mode == ABSOLUTE_mode) {
        CPT (cpt1L, 22); // set abs mode
        sim_debug (DBG_DEBUG, & cpu_dev, "APU: Setting absolute mode.\n");

        SET_I_ABS;
        cpu.PPR.P = 1;
        
    } else if (mode == APPEND_mode) {
        CPT (cpt1L, 23); // set append mode
        if (! TST_I_ABS && TST_I_NBAR)
          sim_debug (DBG_DEBUG, & cpu_dev, "APU: Keeping append mode.\n");
        else
           sim_debug (DBG_DEBUG, & cpu_dev, "APU: Setting append mode.\n");

        CLR_I_ABS;
    } else {
        sim_debug (DBG_ERR, & cpu_dev, "APU: Unable to determine address mode.\n");
        sim_warn ("APU: Unable to determine address mode. Can't happen!\n");
    }
}


//=============================================================================

int query_scu_unit_num (int cpu_unit_num, int cpu_port_num)
  {
    if (cables -> cablesFromScuToCpu [cpu_unit_num].ports [cpu_port_num].inuse)
      return cables -> cablesFromScuToCpu [cpu_unit_num].ports [cpu_port_num].scu_unit_num;
    return -1;
  }

// XXX when multiple cpus are supported, merge this into cpu_reset

static void cpu_init_array (void)
  {
    for (int i = 0; i < N_CPU_UNITS_MAX; i ++)
      for (int p = 0; p < N_CPU_PORTS; p ++)
        cables -> cablesFromScuToCpu [i].ports [p].inuse = false;
  }

static t_stat cpu_show_config (UNUSED FILE * st, UNIT * uptr, 
                               UNUSED int val, UNUSED const void * desc)
{
    long unit_num = UNIT_NUM (uptr);
    if (unit_num < 0 || unit_num >= (int) cpu_dev.numunits)
      {
        //sim_debug (DBG_ERR, & cpu_dev, "cpu_show_config: Invalid unit number %d\n", unit_num);
        sim_printf ("error: invalid unit number %ld\n", unit_num);
        return SCPE_ARG;
      }

#ifdef ROUND_ROBIN
    uint save = setCPUnum ((uint) unit_num);
#endif

    sim_printf ("CPU unit number %ld\n", unit_num);

    sim_printf("Fault base:               %03o(8)\n", cpu.switches.FLT_BASE);
    sim_printf("CPU number:               %01o(8)\n", cpu.switches.cpu_num);
    sim_printf("Data switches:            %012"PRIo64"(8)\n", cpu.switches.data_switches);
    for (int i = 0; i < N_CPU_PORTS; i ++)
      {
        sim_printf("Port%c enable:             %01o(8)\n", 'A' + i, cpu.switches.enable [i]);
        sim_printf("Port%c init enable:        %01o(8)\n", 'A' + i, cpu.switches.init_enable [i]);
        sim_printf("Port%c assignment:         %01o(8)\n", 'A' + i, cpu.switches.assignment [i]);
        sim_printf("Port%c interlace:          %01o(8)\n", 'A' + i, cpu.switches.assignment [i]);
        sim_printf("Port%c store size:         %01o(8)\n", 'A' + i, cpu.switches.store_size [i]);
      }
    sim_printf("Processor mode:           %s [%o]\n", cpu.switches.proc_mode ? "Multics" : "GCOS", cpu.switches.proc_mode);
    sim_printf("Processor speed:          %02o(8)\n", cpu.switches.proc_speed);
    sim_printf("Invert Absolute:          %01o(8)\n", cpu.switches.invert_absolute);
    sim_printf("Bit 29 test code:         %01o(8)\n", cpu.switches.b29_test);
    sim_printf("DIS enable:               %01o(8)\n", cpu.switches.dis_enable);
    sim_printf("AutoAppend disable:       %01o(8)\n", cpu.switches.auto_append_disable);
    sim_printf("LPRPn set high bits only: %01o(8)\n", cpu.switches.lprp_highonly);
    //sim_printf("Steady clock:             %01o(8)\n", cpu.switches.steady_clock);
    sim_printf("Steady clock:             %01o(8)\n", scu [0].steady_clock);
    sim_printf("Degenerate mode:          %01o(8)\n", cpu.switches.degenerate_mode);
    sim_printf("Append after:             %01o(8)\n", cpu.switches.append_after);
    //sim_printf("Super user:               %01o(8)\n", cpu.switches.super_user);
    sim_printf("EPP hack:                 %01o(8)\n", cpu.switches.epp_hack);
    sim_printf("Halt on unimplemented:    %01o(8)\n", cpu.switches.halt_on_unimp);
    sim_printf("Disable SDWAM/PTWAM:      %01o(8)\n", cpu.switches.disable_wam);
    //sim_printf("Bullet time:              %01o(8)\n", cpu.switches.bullet_time);
    sim_printf("Bullet time:              %01o(8)\n", scu [0].bullet_time);
    sim_printf("Disable kbd bkpt:         %01o(8)\n", cpu.switches.disable_kbd_bkpt);
    sim_printf("Report faults:            %01o(8)\n", cpu.switches.report_faults);
    sim_printf("TRO faults enabled:       %01o(8)\n", cpu.switches.tro_enable);
    //sim_printf("Y2K enabled:              %01o(8)\n", cpu.switches.y2k);
    sim_printf("Y2K enabled:              %01o(8)\n", scu [0].y2k);
    sim_printf("drl fatal enabled:        %01o(8)\n", cpu.switches.drl_fatal);
    //sim_printf("trlsb:                  %3d\n",       cpu.switches.trlsb);
    sim_printf("useMap:                   %d\n",      cpu.switches.useMap);

#ifdef ROUND_ROBIN
    setCPUnum (save);
#endif

    return SCPE_OK;
}

//
// set cpu0 config=<blah> [;<blah>]
//
//    blah =
//           faultbase = n
//           num = n
//           data = n
//           portenable = n
//           portconfig = n
//           portinterlace = n
//           mode = n
//           speed = n
//    Hacks:
//           invertabsolute = n
//           b29test = n // deprecated
//           dis_enable = n
//           auto_append_disable = n // still need for 20184, not for t4d
//           lprp_highonly = n // deprecated
//           steadyclock = on|off
//           degenerate_mode = n // deprecated
//           append_after = n
//           epp_hack = n
//           halt_on_unimplmented = n
//           disable_wam = n
//           bullet_time = n
//           disable_kbd_bkpt = n
//           report_faults = n
//               n = 0 don't
//               n = 1 report
//               n = 2 report overflow
//           tro_enable = n
//           y2k
//           drl_fatal

static config_value_list_t cfg_multics_fault_base [] =
  {
    { "multics", 2 },
    { NULL, 0 }
  };

static config_value_list_t cfg_on_off [] =
  {
    { "off", 0 },
    { "on", 1 },
    { "disable", 0 },
    { "enable", 1 },
    { NULL, 0 }
  };

static config_value_list_t cfg_cpu_mode [] =
  {
    { "gcos", 0 },
    { "multics", 1 },
    { NULL, 0 }
  };

static config_value_list_t cfg_port_letter [] =
  {
    { "a", 0 },
    { "b", 1 },
    { "c", 2 },
    { "d", 3 },
#ifdef L68
    { "e", 4 },
    { "f", 5 },
    { "g", 6 },
    { "h", 7 },
#endif
    { NULL, 0 }
  };

static config_value_list_t cfg_interlace [] =
  {
    { "off", 0 },
    { "2", 2 },
    { "4", 4 },
    { NULL, 0 }
  };

static config_value_list_t cfg_size_list [] =
  {
#ifdef L68
// rsw.incl.pl1
//
//  dcl  dps_mem_size_table (0:7) fixed bin (24) static options (constant) init /* DPS and L68 memory sizes */
//      (32768, 65536, 4194304, 131072, 524288, 1048576, 2097152, 262144);
//  
//  /* Note that the third array element above, is changed incompatibly in MR10.0.
//     In previous releases, this array element was used to decode a port size of
//     98304 (96K). With MR10.0 it is now possible to address 4MW per CPU port, by
//     installing  FCO # PHAF183 and using a group 10 patch plug, on L68 and DPS CPUs.
//  */

    { "32", 0 },    //   32768
    { "64", 1 },    //   65536
    { "4096", 2 },  // 4194304
    { "128", 3 },   //  131072
    { "512", 4 },   //  524288
    { "1024", 5 },  // 1048576
    { "2048", 6 },  // 2097152
    { "256", 7 },   //  262144

    { "32K", 0 },
    { "64K", 1 },
    { "4096K", 2 },
    { "128K", 3 },
    { "512K", 4 },
    { "1024K", 5 },
    { "2048K", 6 },
    { "256K", 7 },

    { "1M", 5 },
    { "2M", 6 },
    { "4M", 2 },
#endif // L68

#ifdef DPS8M
// These values are taken from the dps8_mem_size_table loaded by the boot tape.

    {    "32", 0 },
    {    "64", 1 },
    {   "128", 2 },
    {   "256", 3 }, 
    {   "512", 4 }, 
    {  "1024", 5 },
    {  "2048", 6 },
    {  "4096", 7 },

    {   "32K", 0 },
    {   "64K", 1 },
    {  "128K", 2 },
    {  "256K", 3 },
    {  "512K", 4 },
    { "1024K", 5 },
    { "2048K", 6 },
    { "4096K", 7 },

    { "1M", 5 },
    { "2M", 6 },
    { "4M", 7 },
#endif // DPS8M
    { NULL, 0 }

  };

static config_list_t cpu_config_list [] =
  {
    /*  0 */ { "faultbase", 0, 0177, cfg_multics_fault_base },
    /*  1 */ { "num", 0, 07, NULL },
    /*  2 */ { "data", 0, 0777777777777, NULL },
    /*  3 */ { "mode", 0, 01, cfg_cpu_mode }, 
    /*  4 */ { "speed", 0, 017, NULL }, // XXX use keywords
    /*  5 */ { "port", 0, N_CPU_PORTS - 1, cfg_port_letter },
    /*  6 */ { "assignment", 0, 7, NULL },
    /*  7 */ { "interlace", 0, 1, cfg_interlace },
    /*  8 */ { "enable", 0, 1, cfg_on_off },
    /*  9 */ { "init_enable", 0, 1, cfg_on_off },
    /* 10 */ { "store_size", 0, 7, cfg_size_list },

    // Hacks

    /* 11 */ { "invertabsolute", 0, 1, cfg_on_off }, 
    /* 12 */ { "b29test", 0, 1, cfg_on_off }, 
    /* 13 */ { "dis_enable", 0, 1, cfg_on_off }, 
    /* 14 */ { "auto_append_disable", 0, 1, cfg_on_off }, 
    /* 15 */ { "lprp_highonly", 0, 1, cfg_on_off }, 
    // steady_clock was moved to SCU; keep here for script compatibility
    /* 16 */ { "steady_clock", 0, 1, cfg_on_off },
    /* 17 */ { "degenerate_mode", 0, 1, cfg_on_off },
    /* 18 */ { "append_after", 0, 1, cfg_on_off },
    /* 19 */ { "super_user", 0, 1, cfg_on_off },
    /* 20 */ { "epp_hack", 0, 1, cfg_on_off },
    /* 21 */ { "halt_on_unimplemented", 0, 1, cfg_on_off },
    /* 22 */ { "disable_wam", 0, 1, cfg_on_off },
    // bullet_time was moved to SCU; keep here for script compatibility
    /* 23 */ { "bullet_time", 0, 1, cfg_on_off },
    /* 24 */ { "disable_kbd_bkpt", 0, 1, cfg_on_off },
    /* 25 */ { "report_faults", 0, 2, NULL },
    /* 26 */ { "tro_enable", 0, 1, cfg_on_off },
    // y2k was moved to SCU; keep here for script compatibility
    /* 27 */ { "y2k", 0, 1, cfg_on_off },
    /* 28 */ { "drl_fatal", 0, 1, cfg_on_off },
    /* 29 */ { "trlsb", 0, 256, NULL },
    /* 30 */ { "useMap", 0, 1, cfg_on_off },
    { NULL, 0, 0, NULL }
  };

static t_stat cpu_set_initialize_and_clear (UNUSED UNIT * uptr, 
                                            UNUSED int32 value,
                                            UNUSED const char * cptr, 
                                            UNUSED void * desc)
  {
    // Crashes console?
    //cpun_reset2 ((uint) cpu_unit_num);
#ifdef ISOLTS
    long cpu_unit_num = UNIT_NUM (uptr);
    cpu_state_t * cpun = cpus + cpu_unit_num;
    if (cpun->switches.useMap)
      {
        for (uint pgnum = 0; pgnum < N_SCBANKS; pgnum ++)
          {
            int os = cpun->scbank_pg_os [pgnum];
            if (os < 0)
              continue;
//sim_printf ("clearing @%08o\n", (uint) os);
            for (uint addr = 0; addr < SCBANK; addr ++)
              M [addr + (uint) os] = MEM_UNINITIALIZED;
          }
      }
#endif
    return SCPE_OK;
  }

static t_stat cpu_set_config (UNIT * uptr, UNUSED int32 value, const char * cptr, 
                              UNUSED void * desc)
  {
// XXX Minor bug; this code doesn't check for trailing garbage

    long cpu_unit_num = UNIT_NUM (uptr);
    if (cpu_unit_num < 0 || cpu_unit_num >= (long) cpu_dev.numunits)
      {
        //sim_debug (DBG_ERR, & cpu_dev, "cpu_set_config: Invalid unit number %d\n", cpu_unit_num);
        sim_printf ("error: cpu_set_config: invalid unit number %ld\n", cpu_unit_num);
        return SCPE_ARG;
      }

#ifdef ROUND_ROBIN
    uint save = setCPUnum ((uint) cpu_unit_num);
#endif

    static int port_num = 0;

    config_state_t cfg_state = { NULL, NULL };

    for (;;)
      {
        int64_t v;
        int rc = cfgparse ("cpu_set_config", cptr, cpu_config_list, & cfg_state, & v);
        switch (rc)
          {
            case -2: // error
              cfgparse_done (& cfg_state);
              return SCPE_ARG; 

            case -1: // done
              break;

            case  0: // FAULTBASE
              cpu.switches.FLT_BASE = (uint) v;
              break;

            case  1: // NUM
              cpu.switches.cpu_num = (uint) v;
              break;

            case  2: // DATA
              cpu.switches.data_switches = (word36) v;
              break;

            case  3: // MODE
              cpu.switches.proc_mode = (uint) v;
              break;

            case  4: // SPEED
              cpu.switches.proc_speed = (uint) v;
              break;

            case  5: // PORT
              port_num = (int) v;
              break;

            case  6: // ASSIGNMENT
              cpu.switches.assignment [port_num] = (uint) v;
              break;

            case  7: // INTERLACE
              cpu.switches.interlace [port_num] = (uint) v;
              break;

            case  8: // ENABLE
              cpu.switches.enable [port_num] = (uint) v;
              break;

            case  9: // INIT_ENABLE
              cpu.switches.init_enable [port_num] = (uint) v;
              break;

            case 10: // STORE_SIZE
              cpu.switches.store_size [port_num] = (uint) v;
              break;

            case 11: // INVERTABSOLUTE
              cpu.switches.invert_absolute = (uint) v;
              break;

            case 12: // B29TEST
              cpu.switches.b29_test = (uint) v;
              break;

            case 13: // DIS_ENABLE
              cpu.switches.dis_enable = (uint) v;
              break;

            case 14: // AUTO_APPEND_DISABLE
              cpu.switches.auto_append_disable = (uint) v;
              break;

            case 15: // LPRP_HIGHONLY
              cpu.switches.lprp_highonly = (uint) v;
              break;

            case 16: // STEADY_CLOCK
              scu [0].steady_clock = (uint) v;
              break;

            case 17: // DEGENERATE_MODE
              cpu.switches.degenerate_mode = (uint) v;
              break;

            case 18: // APPEND_AFTER
              cpu.switches.append_after = (uint) v;
              break;

            case 19: // SUPER_USER
              sim_printf ("SUPER_USER deprecated\n");
              break;

            case 20: // EPP_HACK
              cpu.switches.epp_hack = (uint) v;
              break;

            case 21: // HALT_ON_UNIMPLEMENTED
              cpu.switches.halt_on_unimp = (uint) v;
              break;

            case 22: // DISABLE_WAM
              cpu.switches.disable_wam = (uint) v;
              if (v) {
                  cpu.cu.SD_ON = 0;
                  cpu.cu.PT_ON = 0;
              }
              break;

            case 23: // BULLET_TIME
              scu [0].bullet_time = (uint) v;
              break;

            case 24: // DISABLE_KBD_BKPT
              cpu.switches.disable_kbd_bkpt = (uint) v;
              break;

            case 25: // REPORT_FAULTS
              cpu.switches.report_faults = (uint) v;
              break;

            case 26: // TRO_ENABLE
              cpu.switches.tro_enable = (uint) v;
              break;

            case 27: // Y2K
              scu [0].y2k = (uint) v;
              break;

            case 28: // DRL_FATAL
              cpu.switches.drl_fatal = (uint) v;
              break;

            case 29: // TRLSB
              //cpu.switches.trlsb = (uint) v;
              sim_printf ("TRLSB deprecated\n");
              break;

            case 30: // USEMAP
              cpu.switches.useMap = v;
              break;

            default:
              //sim_debug (DBG_ERR, & cpu_dev, "cpu_set_config: Invalid cfgparse rc <%d>\n", rc);
              sim_printf ("error: cpu_set_config: invalid cfgparse rc <%d>\n", rc);
              cfgparse_done (& cfg_state);
              return SCPE_ARG; 
          } // switch
        if (rc < 0)
          break;
      } // process statements
    cfgparse_done (& cfg_state);

#ifdef ROUND_ROBIN
    setCPUnum (save);
#endif

    return SCPE_OK;
  }

#ifndef SPEED
static int words2its (word36 word1, word36 word2, struct _par * prp)
  {
    if ((word1 & MASKBITS(6)) != 043)
      {
        return 1;
      }
    prp->SNR = getbits36_15 (word1, 3);
    prp->WORDNO = getbits36_18 (word2, 0);
    prp->RNR = getbits36_3 (word2, 18);  // not strictly correct; normally merged with other ring regs
    //prp->BITNO = getbits36_6(word2, 57 - 36);
    prp->PR_BITNO = getbits36_6 (word2, 57 - 36);
    prp->AR_BITNO = getbits36_6 (word2, 57 - 36) % 9;
    prp->AR_CHAR = getbits36_6 (word2, 57 - 36) / 9;
    return 0;
  }   

static int stack_to_entry (unsigned abs_addr, struct _par * prp)
  {
    // Looks into the stack frame maintained by Multics and
    // returns the "current" entry point address that's
    // recorded in the stack frame.  Abs_addr should be a 24-bit
    // absolute memory location.
    return words2its (M [abs_addr + 026], M [abs_addr + 027], prp);
  }


static void print_frame (
    int seg,        // Segment portion of frame pointer address.
    int offset,     // Offset portion of frame pointer address.
    int addr)       // 24-bit address corresponding to above seg|offset
  {
    // Print a single stack frame for walk_stack()
    // Frame pointers can be found in PR[6] or by walking a process's stack segment

    struct _par  entry_pr;
    sim_printf ("stack trace: ");
    if (stack_to_entry ((uint) addr, & entry_pr) == 0)
      {
         sim_printf ("\t<TODO> entry %o|%o  ", entry_pr.SNR, entry_pr.WORDNO);

//---        const seginfo& seg = segments(entry_pr.PR.snr);
//---        map<int,linkage_info>::const_iterator li_it = seg.find_entry(entry_pr.wordno);
//---        if (li_it != seg.linkage.end()) {
//---            const linkage_info& li = (*li_it).second;
//---            sim_printf ("\t%s  ", li.name.c_str());
//---        } else
//---            sim_printf ("\tUnknown entry %o|%o  ", entry_pr.PR.snr, entry_pr.wordno);
      }
    else
      sim_printf ("\tUnknowable entry {%"PRIo64",%"PRIo64"}  ", M [addr + 026], M [addr + 027]);
    sim_printf ("\n");
    sim_printf ("(stack frame at %03o|%06o)\n", seg, offset);

#if 0
    char buf[80];
    out_msg("prev_sp: %s; ",    its2text(buf, addr+020));
    out_msg("next_sp: %s; ",    its2text(buf, addr+022));
    out_msg("return_ptr: %s; ", its2text(buf, addr+024));
    out_msg("entry_ptr: %s\n",  its2text(buf, addr+026));
#endif
}
#endif

#if 0
static int dsLookupAddress (word18 segno, word18 offset, word24 * finalAddress, char * ctx)
  {
    char * msg;
    int rc = dbgLookupAddress (segno, offset, finalAddress, & msg);
    if (rc)
      {
        sim_printf ("Cannot convert %s %5o|%6o to absolute memory address because %s.\n",
            ctx, segno, offset, msg);
        * finalAddress = 0;
        return 1;
      }
    return 0;
  }
#endif

#if 0
static int dumpStack (uint stkBase, uint stkNo)
  {
    word36 w0, w1;
    word24 addr;
    int rc;
    sim_printf ("Stack %o, segment %05o\n", stkNo, stkBase);
    word24 hdrPage;
    rc = dsLookupAddress (stkBase, 0, & hdrPage, "stack base");
    if (rc)
      return 1;

    sim_printf ("  segment header page at absolute location %08o\n", hdrPage);

    w0 = M [hdrPage + 022];
    w1 = M [hdrPage + 023];
    sim_printf ("  stack_begin_ptr  %012"PRIo64" %012"PRIo64" %05o:%08o\n",
                w0, w1,
                (word15) getbits36_15 (w0,  3),
                (word18) getbits36_18 (w1,  0));

    w0 = M [hdrPage + 024];
    w1 = M [hdrPage + 025];
    sim_printf ("  stack_end_ptr    %012"PRIo64" %012"PRIo64" %05o:%08o\n",
                w0, w1,
                (word15) getbits36_15 (w0,  3),
                (word18) getbits36_18 (w1,  0));

    if (getbits36_6 (M [hdrPage + 022], 30) != 043)
      {
        sim_printf("stack_begin_ptr is not an ITS\n");
        return 1;
      }
    word15 stkBeginSegno = getbits36_15 (M [hdrPage + 022], 3);

    if (stkBeginSegno != stkBase)
      {
        sim_printf("stack_begin_ptr segno (%o) is wrong\n", stkBeginSegno);
        return 1;
      }
    word18 stkBeginOffset = getbits36_18 (M [hdrPage + 023], 0);



    if (getbits36_6 (M [hdrPage + 024], 30) != 043)
      {
        sim_printf("stack_end_ptr is not an ITS\n");
        return 1;
      }

    word15 stkEndSegno = getbits36_15 (M [hdrPage + 024], 3);
    if (stkBeginSegno != stkBase)
      {
        sim_printf("stack_end_ptr segno (%o) is wrong\n", stkEndSegno);
        return 1;
      }
    word18 stkEndOffset = getbits36_18 (M [hdrPage + 025], 0);

    word18 currentFrame = stkBeginOffset;
    int currentFrameNumber = 1;

    for (;;)
      {
        if (currentFrame > stkEndOffset)
          break;
        sim_printf ("  Frame %d, offset %06o\n", currentFrameNumber, currentFrame);
        for (uint n = 0; n < 8; n ++)
          {
            rc = dsLookupAddress (stkBase, currentFrame + 2 * n, & addr, "PR address");
            if (rc)
              return 1;
            w0 = M [addr + 0];
            w1 = M [addr + 1];
            sim_printf ("    PR%o               %012"PRIo64" %012"PRIo64" %05o:%06o BITNO %02o RNG %o\n",
                        n,
                        w0, w1,
                        (word15) getbits36_15 (w0,  3),
                        (word18) getbits36_18 (w1,  0),
                                 getbits36_6 (w1, 21),
                                 getbits36_3 (w0, 18));
          }

        rc = dsLookupAddress (stkBase, currentFrame + 020, & addr, "prev_sp");
        if (rc)
          return 1;
        w0 = M [addr + 0];
        w1 = M [addr + 1];
        sim_printf ("    prev_sp         %012"PRIo64" %012"PRIo64" %05o:%08o\n",
                    w0, w1,
                    (word15) getbits36_15 (w0,  3),
                    (word18) getbits36_18 (w1,  0));

        rc = dsLookupAddress (stkBase, currentFrame + 020, & addr, "next_sp");
        if (rc)
          return 1;
        w0 = M [addr + 0];
        w1 = M [addr + 1];

        word15 nextSpSegno  = (word15) getbits36_15 (w0,  3);
        word18 nextSpOffset = (word18) getbits36_18 (w1,  3);
        sim_printf ("    next_sp         %012"PRIo64" %012"PRIo64" %05o:%08o\n",
                    w0, w1, nextSpSegno, nextSpOffset);

      
        if (nextSpSegno != stkBase)
          {
            sim_printf("    nextsp segno (%o) is wrong\n", nextSpSegno);
            return 1;
          }

        if (nextSpOffset > stkEndOffset)
          {
            sim_printf("    nextsp offset is past end of stack\n");
            break;
          }

        if (nextSpOffset < currentFrame)
          {
            sim_printf("    nextsp offset is less then the current frame\n");
            break;
          }

        currentFrame = nextSpOffset;
        currentFrameNumber ++;
        sim_printf ("\n");
      }
    return 0;
    
  }
#endif

#if 0
int dumpStacks (void)
  {
    sim_printf ("DSBR.STACK %04u\n", cpu.DSBR.STACK);
    uint stkBase = (uint) cpu.DSBR.STACK << 3;
    for (uint stkNo = 0; stkNo <= 5; stkNo ++)
      {
        dumpStack (stkBase + stkNo, stkNo);
      }
    return 0;
  }
#endif

#ifndef SPEED
static int walk_stack (int output, UNUSED void * frame_listp /* list<seg_addr_t>* frame_listp */)
    // Trace through the Multics stack frames
    // See stack_header.incl.pl1 and http://www.multicians.org/exec-env.html
{

    if (cpu.PAR [6].SNR == 077777 || (cpu.PAR [6].SNR == 0 && cpu.PAR [6].WORDNO == 0)) {
        sim_printf ("%s: Null PR[6]\n", __func__);
        return 1;
    }

    // PR6 should point to the current stack frame.  That stack frame
    // should be within the stack segment.
    int seg = cpu.PAR [6].SNR;

    uint curr_frame;
    char * msg;
    //t_stat rc = computeAbsAddrN (& curr_frame, seg, cpu.PAR [6].WORDNO);
    int rc = dbgLookupAddress ((word18) seg, cpu.PAR [6].WORDNO, & curr_frame, & msg);
    if (rc)
      {
        sim_printf ("%s: Cannot convert PR[6] == %#o|%#o to absolute memory address because %s.\n",
            __func__, cpu.PAR [6].SNR, cpu.PAR [6].WORDNO, msg);
        return 1;
      }

    // The stack header will be at offset 0 within the stack segment.
    int offset = 0;
    word24 hdr_addr;  // 24bit main memory address
    //if (computeAbsAddrN (& hdr_addr, seg, offset))
    if (dbgLookupAddress ((word18) seg, (word18) offset, & hdr_addr, & msg))
      {
        sim_printf ("%s: Cannot convert %03o|0 to absolute memory address becuase %s.\n", __func__, seg, msg);
        return 1;
      }

    struct _par stack_begin_pr;
    if (words2its (M [hdr_addr + 022], M [hdr_addr + 023], & stack_begin_pr))
      {
        sim_printf ("%s: Stack header seems invalid; no stack_begin_ptr at %03o|22\n", __func__, seg);
        if (output)
            sim_printf ("%s: Stack Trace: Stack header seems invalid; no stack_begin_ptr at %03o|22\n", __func__, seg);
        return 1;
      }

    struct _par stack_end_pr;
    if (words2its (M [hdr_addr + 024], M [hdr_addr + 025], & stack_end_pr))
      {
        //if (output)
          sim_printf ("%s: Stack Trace: Stack header seems invalid; no stack_end_ptr at %03o|24\n", __func__, seg);
        return 1;
      }

    if (stack_begin_pr.SNR != seg || stack_end_pr.SNR != seg)
      {
        //if (output)
            sim_printf ("%s Stack Trace: Stack header seems invalid; stack frames are in another segment.\n", __func__);
        return 1;
      }

    struct _par lot_pr;
    if (words2its (M [hdr_addr + 026], M [hdr_addr + 027], & lot_pr))
      {
        //if (output)
          sim_printf ("%s: Stack Trace: Stack header seems invalid; no LOT ptr at %03o|26\n", __func__, seg);
        return 1;
      }
    // TODO: sanity check LOT ptr

    if (output)
      sim_printf ("%s: Stack Trace via back-links in current stack frame:\n", __func__);
    uint framep = stack_begin_pr.WORDNO;
    uint prev = 0;
    int finished = 0;
#if 0
    int need_hist_msg = 0;
#endif
    // while(framep <= stack_end_pr.WORDNO)
    for (;;)
      {
        // Might find ourselves in a different page while moving from frame to frame...
        // BUG: We assume a stack frame doesn't cross page boundries
        uint addr;
        //if (computeAbsAddrN (& addr, seg, framep))
        if (dbgLookupAddress ((word18) seg, (word18) offset, & addr, & msg))
          {
            if (finished)
              break;
            //if (output)
              sim_printf ("%s: STACK Trace: Cannot convert address of frame %03o|%06o to absolute memory address because %s.\n", __func__, seg, framep, msg);
            return 1;
          }

        // Sanity check
        if (prev != 0)
          {
            struct _par prev_pr;
            if (words2its (M [addr + 020], M [addr + 021], & prev_pr) == 0)
              {
                if (prev_pr.WORDNO != prev)
                  {
                    if (output)
                      sim_printf ("%s: STACK Trace: Stack frame's prior ptr, %03o|%o is bad; expected %o.\n", __func__, seg, prev_pr.WORDNO, prev);
                    break;
                  }
              }
          }
        prev = framep;
        // Print the current frame
        if (finished && M [addr + 022] == 0 && M [addr + 024] == 0 && M [addr + 026] == 0)
          break;
#if 0
        if (need_hist_msg) {
            need_hist_msg = 0;
            out_msg("stack trace: ");
            out_msg("Recently popped frames (aka where we recently returned from):\n");
        }
#endif
        if (output)
          print_frame (seg, (int) framep, (int) addr);
//---        if (frame_listp)
//---            (*frame_listp).push_back(seg_addr_t(seg, framep));

        // Get the next one
        struct _par next;
        if (words2its (M [addr + 022], M [addr + 023], & next))
          {
            if (! finished)
              if (output)
                sim_printf ("STACK Trace: no next frame.\n");
            break;
          }
        if (next.SNR != seg)
          {
            if (output)
              sim_printf ("STACK Trace: next frame is in a different segment (next is in %03o not %03o.\n", next.SNR, seg);
            break;
          }
        if (next.WORDNO == stack_end_pr.WORDNO)
          {
            finished = 1;
            break;
#if 0
            need_hist_msg = 1;
            if (framep != cpu.PAR [6].WORDNO)
                out_msg("Stack Trace: Stack may be garbled...\n");
            // BUG: Infinite loop if enabled and garbled stack with "Unknowable entry {0,0}", "Unknown entry 15|0  (stack frame at 062|000000)", etc
#endif
          }
        if (next.WORDNO < stack_begin_pr.WORDNO || next.WORDNO > stack_end_pr.WORDNO)
          {
            if (! finished)
              //if (output)
                sim_printf ("STACK Trace: DEBUG: next frame at %#o is outside the expected range of %#o .. %#o for stack frames.\n", next.WORDNO, stack_begin_pr.WORDNO, stack_end_pr.WORDNO);
            if (! output)
              return 1;
          }

        // Use the return ptr in the current frame to print the source line.
        if (! finished && output)
          {
            struct _par return_pr;
            if (words2its (M [addr + 024], M [addr + 025], & return_pr) == 0)
              {
//---                 where_t where;
                uint offset = return_pr.WORDNO;
                if (offset > 0)
                    -- offset;      // call was from an instr prior to the return point
                char * compname;
                word18 compoffset;
                char * where = lookupAddress (return_pr.SNR, (word18) offset, & compname, & compoffset);
                if (where)
                  {
                    sim_printf ("%s\n", where);
                    listSource (compname, compoffset, 0);
                  }

//---                 if (seginfo_find_all(return_pr.SNR, offset, &where) == 0) {
//---                     out_msg("stack trace: ");
//---                     if (where.line_no >= 0) {
//---                         // Note that if we have a source line, we also expect to have a "proc" entry and file name
//---                         out_msg("\t\tNear %03o|%06o in %s\n",
//---                             return_pr.SNR, return_pr.WORDNO, where.entry);
//---                         // out_msg("\t\tSource:  %s, line %5d:\n", where.file_name, where.line_no);
//---                         out_msg("stack trace: ");
//---                         out_msg("\t\tLine %d of %s:\n", where.line_no, where.file_name);
//---                         out_msg("stack trace: ");
//---                         out_msg("\t\tSource:  %s\n", where.line);
//---                     } else
//---                         if (where.entry_offset < 0)
//---                             out_msg("\t\tNear %03o|%06o", return_pr.SNR, return_pr.WORDNO);
//---                         else {
//---                             int off = return_pr.WORDNO - where.entry_offset;
//---                             char sign = (off < 0) ? '-' : '+';
//---                             if (sign == '-')
//---                                 off = - off;
//---                             out_msg("\t\tNear %03o|%06o %s %c%#o\n", return_pr.SNR, return_pr.WORDNO, where.entry, sign, off);
//---                         }
//---                 }
              }
          }
        // Advance
        framep = next.WORDNO;
    }

//---    if (output)
//---      {
//---        out_msg("stack trace: ");
//---        out_msg("Current Location:\n");
//---        out_msg("stack trace: ");
//---        print_src_loc("\t", get_addr_mode(), cpu.PPR.PSR, cpu.PPR.IC, &cpu.cu.IR);
//---
//---        log_any_io(0);      // Output of source/location info doesn't count towards requiring re-display of source
//---    }

    return 0;
}

static int cmd_stack_trace (UNUSED int32 arg, UNUSED char * buf)
  {
    walk_stack (1, NULL);
    sim_printf ("\n");
//---     trace_all_stack_hist ();
//---     sim_printf ("\n");
//---     dump_autos ();
//---     sim_printf ("\n");

    //float secs = (float) sys_stats.total_msec / 1000;
    //out_msg("Stats: %.1f seconds: %"PRIo64" cycles at %.0f cycles/sec, %"PRIo64" instructions at %.0f instr/sec\n",
        //secs, sys_stats.total_cycles, sys_stats.total_cycles/secs, sys_stats.total_instr, sys_stats.total_instr/secs);

    return 0;
  }


static int cpu_show_stack (UNUSED FILE * st, UNUSED UNIT * uptr, 
                           UNUSED int val, UNUSED const void * desc)
  {
#ifdef ROUND_ROBIN
    long unit_num = UNIT_NUM (uptr);
    uint save = setCPUnum ((uint) unit_num);
#endif
    // FIXME: use FILE *st
    int ret = cmd_stack_trace(0, NULL);
#ifdef ROUND_ROBIN
    setCPUnum (save);
#endif
    return ret;
  }
#endif

static t_stat cpu_show_nunits (UNUSED FILE * st, UNUSED UNIT * uptr, UNUSED int val, UNUSED const void * desc)
  {
    sim_printf("Number of CPUs in system is %d\n", cpu_dev.numunits);
    return SCPE_OK;
  }

static t_stat cpu_set_nunits (UNUSED UNIT * uptr, UNUSED int32 value, const char * cptr, UNUSED void * desc)
  {
    if (! cptr)
      return SCPE_ARG;
    int n = atoi (cptr);
    if (n < 1 || n > N_CPU_UNITS_MAX)
      return SCPE_ARG;
    cpu_dev.numunits = (uint32) n;
    return SCPE_OK;
  }

void addHist (uint hset, word36 w0, word36 w1)
  {
    //if (cpu.MR.emr)
      {
        cpu.history [hset] [cpu.history_cyclic[hset]] [0] = w0;
        cpu.history [hset] [cpu.history_cyclic[hset]] [1] = w1;
        cpu.history_cyclic[hset] = (cpu.history_cyclic[hset] + 1) % N_HIST_SIZE;
      }
  }

void addHistForce (uint hset, word36 w0, word36 w1)
  {
    cpu.history [hset] [cpu.history_cyclic[hset]] [0] = w0;
    cpu.history [hset] [cpu.history_cyclic[hset]] [1] = w1;
    cpu.history_cyclic[hset] = (cpu.history_cyclic[hset] + 1) % N_HIST_SIZE;
  }

#ifdef DPS8M
//void addCUhist (word36 flags, word18 opcode, word24 address, word5 proccmd, word7 flags2)
void addCUhist (void)
  {
    if (cpu.skip_cu_hist)
      return;
    if (! cpu.MR_cache.emr)
      return;
    if (! cpu.MR_cache.ihr)
      return;
    if (cpu.MR_cache.hrxfr && ! cpu.wasXfer)
      return;

    word36 flags = 0; // XXX fill out
    word5 proccmd = 0; // XXX fill out
    word7 flags2 = 0; // XXX fill out
    word36 w0 = 0, w1 = 0;
    w0 |= flags & 0777777000000;
    w0 |= IWB_IRODD & MASK18;
    w1 |= (cpu.iefpFinalAddress & MASK24) << 12;
    w1 |= (proccmd & MASK5) << 7;
    w1 |= flags2 & 0176;
    addHist (CU_HIST_REG, w0, w1);
  }

void addDUOUhist (word36 flags, word18 ICT, word9 RS_REG, word9 flags2)
  {
    word36 w0 = flags, w1 = 0;
    w1 |= (ICT & MASK18) << 18;
    w1 |= (RS_REG & MASK9) << 9;
    w1 |= flags2 & MASK9;
    addHist (DU_OU_HIST_REG, w0, w1);
  }

void addAPUhist (word15 ESN, word21 flags, word24 RMA, word3 RTRR, word9 flags2)
  {
    word36 w0 = 0, w1 = 0;
    w0 |= (ESN & MASK15) << 21;
    w0 |= flags & MASK21;
    w1 |= (RMA & MASK24) << 12;
    w1 |= (RTRR & MASK3) << 9;
    w1 |= flags2 & MASK9;
    addHist (APU_HIST_REG, w0, w1);
  }

void addEAPUhist (word18 ZCA, word18 opcode)
  {
    word36 w0 = 0;
    w0 |= (ZCA & MASK18) << 18;
    w0 |= opcode & MASK18;
    addHist (EAPU_HIST_REG, w0, 0);
    //cpu.eapu_hist[cpu.eapu_cyclic].ZCA = ZCA;
    //cpu.eapu_hist[cpu.eapu_cyclic].opcode = opcode;
    //cpu.history_cyclic[EAPU_HIST_REG] = (cpu.history_cyclic[EAPU_HIST_REG] + 1) % N_HIST_SIZE;
  }
#endif // DPS8M

#ifdef L68

// According to ISOLTS
//
//   0 PIA
//   1 POA
//   2 RIW
//   3 SIW
//   4 POT
//   5 PON
//   6 RAW
//   7 SAW
//   8 TRGO
//   9 XDE
//  10 XDO
//  11 IC
//  12 RPTS
//  13 WI
//  14 AR F/E
//  15 XIP
//  16 FLT
//  17 COMPL. ADD BASE
//  18:23 OPCODE/TAG
//  24:29 ADDREG
//  30:34 COMMAND A/B/C/D/E
//  35:38 PORT A/B/C/D
//  39 FB XEC
//  40 INS FETCH
//  41 CU STORE
//  42 OU STORE
//  43 CU LOAD
//  44 OU LOAD
//  45 RB DIRECT
//  46 -PC BUSY
//  47 PORT BUSY

void addCUhist (void)
  {
    CPT (cpt1L, 24); // add cu hist
// XXX strobe on opcode match
    if (cpu.skip_cu_hist)
      return;
    if (! cpu.MR_cache.emr)
      return;
    if (! cpu.MR_cache.ihr)
      return;

//IF1 if (cpu.MR.hrhlt) sim_printf ("%u\n", cpu.history_cyclic[CU_HIST_REG]);
//IF1 sim_printf ("%u\n", cpu.history_cyclic[CU_HIST_REG]);
    word36 w0 = 0, w1 = 0;

    // 0 PIA
    // 1 POA
    // 2 RIW
    // 3 SIW
    // 4 POT
    // 5 PON
    // 6 RAW
    // 7 SAW
    PNL (putbits36_8 (& w0, 0, cpu.prepare_state);)
    // 8 TRG 
    putbits36_1 (& w0, 8, cpu.wasXfer);
    // 9 XDE
    putbits36_1 (& w0, 9, cpu.cu.xde);
    // 10 XDO
    putbits36_1 (& w0, 10, cpu.cu.xdo);
    // 11 IC
    putbits36_1 (& w0, 11, USE_IRODD?1:0);
    // 12 RPT
    putbits36_1 (& w0, 12, cpu.cu.rpt);
    // 13 WI Wait for instruction fetch XXX Not tracked
    // 14 ARF "AR F/E" Address register Full/Empty Address has valid data 
    PNL (putbits36_1 (& w0, 14, cpu.AR_F_E);)
    // 15 !XA/Z "-XIP NOT prepare interrupt address" 
    putbits36_1 (& w0, 15, cpu.cycle != INTERRUPT_cycle?1:0);
    // 16 !FA/Z Not tracked. (cu.-FL?)
    putbits36_1 (& w0, 16, cpu.cycle != FAULT_cycle?1:0);
    // 17 M/S  (master/slave, cu.-BASE?, NOT BAR MODE)
    putbits36_1 (& w0, 17, TSTF (cpu.cu.IR, I_NBAR)?1:0);
    // 18:35 IWR (lower half of IWB)
    putbits36_18 (& w0, 18, (word18) (IWB_IRODD & MASK18));

    // 36:53 CA
    putbits36_18 (& w1, 0, cpu.TPR.CA);
    // 54:58 CMD system controller command XXX
    // 59:62 SEL port select (XXX ignoring "only valid if port A-D is selected")
    PNL (putbits36_1 (& w1, 59-36, (cpu.portSelect == 0)?1:0);)
    PNL (putbits36_1 (& w1, 60-36, (cpu.portSelect == 1)?1:0);)
    PNL (putbits36_1 (& w1, 61-36, (cpu.portSelect == 2)?1:0);)
    PNL (putbits36_1 (& w1, 62-36, (cpu.portSelect == 3)?1:0);)
    // 63 XEC-INT An interrupt is present
    putbits36_1 (& w1, 63-36, cpu.interrupt_flag?1:0);
    // 64 INS-FETCH Perform an instruction fetch
    PNL (putbits36_1 (& w1, 64-36, cpu.INS_FETCH?1:0);)
    // 65 CU-STORE Control unit store cycle XXX
    // 66 OU-STORE Operations unit store cycle XXX
    // 67 CU-LOAD Control unit load cycle XXX
    // 68 OU-LOAD Operations unit load cycle XXX
    // 69 DIRECT Direct cycle XXX
    // 70 -PC-BUSY Port control logic not busy XXX
    // 71 BUSY Port interface busy XXX

    addHist (CU_HIST_REG, w0, w1);

    // Check for overflow
    CPTUR (cptUseMR);
    if (cpu.MR.hrhlt && cpu.history_cyclic[CU_HIST_REG] == 0)
      {
        //cpu.history_cyclic[CU_HIST_REG] = 15;
        if (cpu.MR.ihrrs)
          {
            cpu.MR.ihr = 0;
          }
//IF1 sim_printf ("trapping......\n");
        set_FFV_fault (4);
        return;
      }
  }

// du history register inputs(actual names)
// bit 00= fpol-cx;010       bit 36= fdud-dg;112
// bit 01= fpop-cx;010       bit 37= fgdlda-dc;010
// bit 02= need-desc-bd;000  bit 38= fgdldb-dc;010
// bit 03= sel-adr-bd;000    bit 39= fgdldc-dc;010
// bit 04= dlen=direct-bd;000bit 40= fnld1-dp;110
// bit 05= dfrst-bd;021      bit 41= fgldp1-dc;110
// bit 06= fexr-bd;010       bit 42= fnld2-dp;110
// bit 07= dlast-frst-bd;010 bit 43= fgldp2-dc;110
// bit 08= ddu-ldea-bd;000   bit 44= fanld1-dp;110
// bit 09= ddu-stea-bd;000   bit 45= fanld2-dp;110
// bit 10= dredo-bd;030      bit 46= fldwrt1-dp;110
// bit 11= dlvl<wd-sz-bg;000 bit 47= fldwrt2-dp;110
// bit 12= exh-bg;000        bit 48= data-avldu-cm;000
// bit 13= dend-seg-bd;111   bit 49= fwrt1-dp;110
// bit 14= dend-bd;000       bit 50= fgstr-dc;110
// bit 15= du=rd+wrt-bd;010  bit 51= fanstr-dp;110
// bit 16= ptra00-bd;000     bit 52= fstr-op-av-dg;010
// bit 17= ptra01-bd;000     bit 53= fend-seg-dg;010
// bit 18= fa/i1-bd;110      bit 54= flen<128-dg;010
// bit 19= fa/i2-bd;110      bit 55= fgch-dp;110
// bit 20= fa/i3-bd;110      bit 56= fanpk-dp;110
// bit 21= wrd-bd;000        bit 57= fexmop-dl;110
// bit 22= nine-bd;000       bit 58= fblnk-dp;100
// bit 23= six-bd;000        bit 59= unused
// bit 24= four-bd;000       bit 60= dgbd-dc;100
// bit 25= bit-bd;000        bit 61= dgdb-dc;100
// bit 26= unused            bit 62= dgsp-dc;100
// bit 27= unused            bit 63= ffltg-dc;110
// bit 28= unused            bit 64= frnd-dg;120
// bit 29= unused            bit 65= dadd-gate-dc;100
// bit 30= fsampl-bd;111     bit 66= dmp+dv-gate-db;100
// bit 31= dfrst-ct-bd;010   bit 67= dxpn-gate-dg;100
// bit 32= adj-lenint-cx;000 bit 68= unused
// bit 33= fintrptd-cx;010   bit 69= unused
// bit 34= finhib-stc1-cx;010bit 70= unused
// bit 35= unused            bit 71= unused

void addDUhist (void)
  {
    CPT (cpt1L, 25); // add du hist
    PNL (addHist (DU_HIST_REG, cpu.du.cycle1, cpu.du.cycle2);)
  }
     

void addOUhist (void)
  {
    CPT (cpt1L, 26); // add ou hist
    word36 w0 = 0, w1 = 0;
     
    // 0-16 RP
    //   0-8 OP CODE
    PNL (putbits36_9 (& w0, 0, cpu.ou.RS);)
    
    //   9 CHAR
    putbits36_1 (& w0, 9, cpu.ou.characterOperandSize ? 1 : 0);

    //   10-12 TAG 1/2/3
    putbits36_3 (& w0, 10, cpu.ou.characterOperandOffset);

    //   13 CRFLAG
    putbits36_1 (& w0, 13, cpu.ou.crflag);

    //   14 DRFLAG
    putbits36_1 (& w0, 14, cpu.ou.directOperandFlag ? 1 : 0);

    //   15-16 EAC
    putbits36_2 (& w0, 15, cpu.ou.eac);

    // 17 0
    // 18-26 RS REG
    PNL (putbits36_9 (& w0, 18, cpu.ou.RS);)

    // 27 RB1 FULL
    putbits36_1 (& w0, 27, cpu.ou.RB1_FULL);

    // 28 RP FULL
    putbits36_1 (& w0, 28, cpu.ou.RP_FULL);

    // 29 RS FULL
    putbits36_1 (& w0, 29, cpu.ou.RS_FULL);

    // 30-35 GIN/GOS/GD1/GD2/GOE/GOA
    putbits36_6 (& w0, 30, (word6) (cpu.ou.cycle >> 3));

    // 36-38 GOM/GON/GOF
    putbits36_3 (& w1, 36-36, (word3) cpu.ou.cycle);

    // 39 STR OP
    putbits36_1 (& w1, 39-36, cpu.ou.STR_OP);

    // 40 -DA-AV XXX

    // 41-50 stuvwyyzAB -A-REG -Q-REG -X0-REG .. -X7-REG
    PNL (putbits36_10 (& w1, 41-36, (word10) ~NonEISopcodes [cpu.ou.RS].reg_use);)

    // 51-53 0

    // 54-71 ICT TRACKER
    putbits36_18 (& w1, 54 - 36, cpu.PPR.IC);

    addHist (OU_HIST_REG, w0, w1);
  }

// According to ISOLTS
//  0:2 OPCODE RP
//  3 9 BIT CHAR
//  4:6 TAG 3/4/5
//  7 CR FLAG
//  8 DIR FLAG
//  9 RP15
// 10 RP16
// 11 SPARE
// 12:14 OPCODE RS
// 15 RB1 FULL
// 16 RP FULL
// 17 RS FULL
// 18 GIN
// 19 GOS
// 20 GD1
// 21 GD2
// 22 GOE
// 23 GOA
// 24 GOM
// 25 GON
// 26 GOF
// 27 STORE OP
// 28 DA NOT
// 29:38 COMPLEMENTED REGISTER IN USE FLAG A/Q/0/1/2/3/4/5/6/7
// 39 ?
// 40 ?
// 41 ? 
// 42:47 ICT TRACT

// XXX addAPUhist

//  0:5 SEGMENT NUMBER
//  6 SNR/ESN
//  7 TSR/ESN
//  8 FSDPTW
//  9 FPTW2
// 10 MPTW
// 11 FANP
// 12 FAP
// 13 AMSDW
// 14:15 AMSDW #
// 16 AMPTW
// 17:18 AMPW #
// 19 ACV/DF
// 20:27 ABSOLUTE MEMORY ADDRESS
// 28 TRR #
// 29 FLT HLD

void addAPUhist (enum APUH_e op)
  {
    CPT (cpt1L, 28); // add apu hist
    word36 w0 = 0, w1 = 0;
     
    w0 = op; // set 17-24 FDSPTW/.../FAP bits

    // 0-14 ESN 
    putbits36_15 (& w0, 0, cpu.TPR.TSR);
    // 15-16 BSY
    PNL (putbits36_1 (& w0, 15, (cpu.apu.state & apu_ESN_SNR) ? 1 : 0);)
    PNL (putbits36_1 (& w0, 16, (cpu.apu.state & apu_ESN_TSR) ? 1 : 0);)
    // 25 SDWAMM
    putbits36_1 (& w0, 25, cpu.cu.SDWAMM);
    // 26-29 SDWAMR
#ifdef WAM
    putbits36_4 (& w0, 26, cpu.SDWAMR);
#endif
    // 30 PTWAMM
    putbits36_1 (& w0, 30, cpu.cu.PTWAMM);
    // 31-34 PTWAMR
#ifdef WAM
    putbits36_4 (& w0, 31, cpu.PTWAMR);
#endif
    // 35 FLT
    PNL (putbits36_1 (& w0, 35, (cpu.apu.state & apu_FLT) ? 1 : 0);)

    // 36-59 ADD
    PNL (putbits36_24 (& w1,  0, cpu.APUMemAddr);)
    // 60-62 TRR
    putbits36_3 (& w1, 24, cpu.TPR.TRR);
    // 66 XXX Multiple match error in SDWAM
    // 70 Segment is encachable
    putbits36_1 (& w1, 34, cpu.SDW0.C);
    // 71 XXX Multiple match error in PTWAM

    addHist (APU_HIST_REG, w0, w1);
  }

#endif

