#ifndef MAILBOX_MAP_H
#define MAILBOX_MAP_H

// Chiplet-wide allocations of the mailbox resources. This file is exists in
// code bases for all subsystems: TRCH, RTPS, HPPS (included from Linux DT).
// For practical reasons, there are multiple copies of the file, but they
// should all be in sync (i.e. identical).

// In the context of TRCH, RTPS, these allocations are completely outside the
// scope of the mailbox driver and of the mailbox link.  They are referenced
// only from the top-level app code -- namely, main and sever.
//
// This file included from both RTPS and TRCH code, to minimize possibility
// of conflicting assignments.

// For interrupt assignments: each interrupt is dedicated to one subsystem.

// The interrupt index is the index within the IP block (not global IRQ#).
// There are HPSC_MBOX_INTS interrupts per IP block, either event from any
// mailbox instance can be mapped to any interrupt.

// Naming convention:
// REGION_BLOCK_PROP__SRC_SW__DEST_SW__DIR
// REGION: [LSIO, HPPS] - the Chiplet region
// BLOCK: [MBOX0, MBOX1] - the IP block instance in REGION
// PROP: [CHAN, INT] - channel or interrupt
// SRC, DST: [TRCH, RTPS_R52_LOCKSTEP, RTPS_R52_SMP,
//            RTPS_R52_SPLIT_0, RTPS_R52_SPLIT_1, RTPS_A53,
//            HPPS_SMP, HPPS_SMP_CL0, HPPS_SMP_CL1] - subsystem
// SW: [SSW, ATF, APP] - the SRC/DST software component

//
// LSIO Mailbox IP Block
//

#define LSIO_MBOX0_INT_EVT0__TRCH_SSW 0
#define LSIO_MBOX0_INT_EVT1__TRCH_SSW 1

#define LSIO_MBOX0_INT_EVT0__RTPS_R52_LOCKSTEP_SSW 2
#define LSIO_MBOX0_INT_EVT1__RTPS_R52_LOCKSTEP_SSW 3

#define LSIO_MBOX0_INT_EVT0__RTPS_A53_ATF 4
#define LSIO_MBOX0_INT_EVT1__RTPS_A53_ATF 5

#define LSIO_MBOX0_CHAN__RTPS_R52_LOCKSTEP_SSW__TRCH_SSW__RQST 0
#define LSIO_MBOX0_CHAN__RTPS_R52_LOCKSTEP_SSW__TRCH_SSW__RPLY 1

#define LSIO_MBOX0_CHAN__RTPS_A53_ATF__TRCH_SSW__RQST 2
#define LSIO_MBOX0_CHAN__RTPS_A53_ATF__TRCH_SSW__RPLY 3

#define LSIO_MBOX0_CHAN__RTPS_R52_LOCKSTEP_LOOPBACK_SSW 31

//
// HPPS Mailbox IP Block 0
//

#define HPPS_MBOX0_INT_EVT0__TRCH_SSW 0
#define HPPS_MBOX0_INT_EVT1__TRCH_SSW 1

#define HPPS_MBOX0_INT_EVT0__HPPS_SMP_SSW 2
#define HPPS_MBOX0_INT_EVT1__HPPS_SMP_SSW 3

#define HPPS_MBOX0_CHAN__HPPS_SMP_APP__TRCH_SSW__RQST 0
#define HPPS_MBOX0_CHAN__HPPS_SMP_APP__TRCH_SSW__RPLY 1

// Mailboxes owned by Linux (just a test)
#define HPPS_MBOX0_CHAN__HPPS_SMP_APP_OWN__TRCH_SSW__RQST 2
#define HPPS_MBOX0_CHAN__HPPS_SMP_APP_OWN__TRCH_SSW__RPLY 3

// Mailboxes owned by ATF (just a test)
#define HPPS_MBOX0_CHAN__HPPS_SMP_ATF__TRCH_SSW__RQST 28
#define HPPS_MBOX0_CHAN__HPPS_SMP_ATF__TRCH_SSW__RPLY 29

#define HPPS_MBOX0_CHAN__HPPS_SMP_SSW__TRCH_SSW__RQST 30
#define HPPS_MBOX0_CHAN__HPPS_SMP_SSW__TRCH_SSW__RPLY 31

//
// HPPS Mailbox IP Block 1
//

#define HPPS_MBOX1_INT_EVT0__RTPS_R52_LOCKSTEP_SSW 0
#define HPPS_MBOX1_INT_EVT1__RTPS_R52_LOCKSTEP_SSW 1

#define HPPS_MBOX1_INT_EVT0__HPPS_SMP_SSW 2
#define HPPS_MBOX1_INT_EVT1__HPPS_SMP_SSW 3

#define HPPS_MBOX1_CHAN__HPPS_SMP_APP__RTPS_R52_LOCKSTEP_SSW__RQST 0
#define HPPS_MBOX1_CHAN__HPPS_SMP_APP__RTPS_R52_LOCKSTEP_SSW__RPLY 1

#endif // MAILBOX_MAP_H
