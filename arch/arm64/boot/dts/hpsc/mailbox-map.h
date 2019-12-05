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
// REGION_BLOCK_PROP__SRC_SW__DEST_SW
// REGION: [LSIO, HPPS] - the Chiplet region
// BLOCK: [MBOX0, MBOX1] - the IP block instance in REGION
// PROP: [CHAN, INT] - channel or interrupt
// SRC, DST: subsystem (enum sw_subsys in subsys.h)
// SW: the software component on SRC/DEST subsystem (enum sw_comp in susbys.h)

//
// LSIO Mailbox IP Block
//

#define LSIO_MBOX0_INT_EVT0__TRCH_SSW 0
#define LSIO_MBOX0_INT_EVT1__TRCH_SSW 1

/* Allocations can overlap for subsystems that cannot run concurrently. */
/* Note: SMP OS uses one mailbox -- synchronization up to the SW */
#define LSIO_MBOX0_INT_EVT0__RTPS_R52_LOCKSTEP_SSW 2
#define LSIO_MBOX0_INT_EVT1__RTPS_R52_LOCKSTEP_SSW 3
#define LSIO_MBOX0_INT_EVT0__RTPS_R52_SPLIT_0_SSW 2
#define LSIO_MBOX0_INT_EVT1__RTPS_R52_SPLIT_0_SSW 3
#define LSIO_MBOX0_INT_EVT0__RTPS_R52_SPLIT_1_SSW 4
#define LSIO_MBOX0_INT_EVT1__RTPS_R52_SPLIT_1_SSW 5
#define LSIO_MBOX0_INT_EVT0__RTPS_R52_SMP_SSW 2
#define LSIO_MBOX0_INT_EVT1__RTPS_R52_SMP_SSW 3

#define LSIO_MBOX0_INT_EVT0__RTPS_A53_ATF 6
#define LSIO_MBOX0_INT_EVT1__RTPS_A53_ATF 7

// RTPS R52 SSW <-> TRCH SSW
#define LSIO_MBOX0_CHAN__RTPS_R52_LOCKSTEP_SSW__TRCH_SSW 0
#define LSIO_MBOX0_CHAN__TRCH_SSW__RTPS_R52_LOCKSTEP_SSW 1
#define LSIO_MBOX0_CHAN__RTPS_R52_SPLIT_0_SSW__TRCH_SSW 0
#define LSIO_MBOX0_CHAN__TRCH_SSW__RTPS_R52_SPLIT_0_SSW 1
#define LSIO_MBOX0_CHAN__RTPS_R52_SPLIT_1_SSW__TRCH_SSW 2
#define LSIO_MBOX0_CHAN__TRCH_SSW__RTPS_R52_SPLIT_1_SSW 3
#define LSIO_MBOX0_CHAN__RTPS_R52_SMP_SSW__TRCH_SSW 0
#define LSIO_MBOX0_CHAN__TRCH_SSW__RTPS_R52_SMP_SSW 1

// RTPS A53 ATF <-> TRCH SSW
#define LSIO_MBOX0_CHAN__RTPS_A53_ATF__TRCH_SSW__RQST 4
#define LSIO_MBOX0_CHAN__RTPS_A53_ATF__TRCH_SSW__RPLY 5

// RTPS R52 SSW loopback
#define LSIO_MBOX0_CHAN__RTPS_R52_LOCKSTEP_SSW__LOOPBACK 31
#define LSIO_MBOX0_CHAN__RTPS_R52_SPLIT_0_SSW__LOOPBACK 31
#define LSIO_MBOX0_CHAN__RTPS_R52_SPLIT_1_SSW__LOOPBACK 31
#define LSIO_MBOX0_CHAN__RTPS_R52_SMP_SSW__LOOPBACK 31

//
// HPPS Mailbox IP Block 0
//

#define HPPS_MBOX0_INT_EVT0__TRCH_SSW 0
#define HPPS_MBOX0_INT_EVT1__TRCH_SSW 1

#define HPPS_MBOX0_INT_EVT0__HPPS_SMP_SSW 2
#define HPPS_MBOX0_INT_EVT1__HPPS_SMP_SSW 3


// HPPS userspace <-> TRCH SSW
#define HPPS_MBOX0_CHAN__HPPS_SMP_APP__TRCH_SSW 0
#define HPPS_MBOX0_CHAN__TRCH_SSW__HPPS_SMP_APP 1

// Mailboxes owned by Linux (just a test)
#define HPPS_MBOX0_CHAN__TRCH_SSW__HPPS_SMP_APP_OWN 2
#define HPPS_MBOX0_CHAN__HPPS_SMP_APP_OWN__TRCH_SSW 3

// HPPS ATF <-> TRCH SSW
#define HPPS_MBOX0_CHAN__HPPS_SMP_ATF__TRCH_SSW 28
#define HPPS_MBOX0_CHAN__TRCH_SSW__HPPS_SMP_ATF 29

// HPPS SSW <-> TRCH SSW
#define HPPS_MBOX0_CHAN__HPPS_SMP_SSW__TRCH_SSW 30
#define HPPS_MBOX0_CHAN__TRCH_SSW__HPPS_SMP_SSW 31

//
// HPPS Mailbox IP Block 1
//

#define HPPS_MBOX1_INT_EVT0__RTPS_R52_LOCKSTEP_SSW 0
#define HPPS_MBOX1_INT_EVT1__RTPS_R52_LOCKSTEP_SSW 1
#define HPPS_MBOX1_INT_EVT0__RTPS_R52_SPLIT_0_SSW 0
#define HPPS_MBOX1_INT_EVT1__RTPS_R52_SPLIT_0_SSW 1
#define HPPS_MBOX1_INT_EVT0__RTPS_R52_SPLIT_1_SSW 2
#define HPPS_MBOX1_INT_EVT1__RTPS_R52_SPLIT_1_SSW 3
#define HPPS_MBOX1_INT_EVT0__RTPS_R52_SMP_SSW 0
#define HPPS_MBOX1_INT_EVT1__RTPS_R52_SMP_SSW 1

#define HPPS_MBOX1_INT_EVT0__HPPS_SMP_SSW 4
#define HPPS_MBOX1_INT_EVT1__HPPS_SMP_SSW 5

// HPPS SSW <-> RTPS_R52_{LOCKSTEP,SPLIT_{0,1},SMP}
#define HPPS_MBOX1_CHAN__HPPS_SMP_APP__RTPS_R52_LOCKSTEP_SSW 0
#define HPPS_MBOX1_CHAN__RTPS_R52_LOCKSTEP_SSW__HPPS_SMP_APP 1
#define HPPS_MBOX1_CHAN__HPPS_SMP_APP__RTPS_R52_SPLIT_0_SSW 0
#define HPPS_MBOX1_CHAN__RTPS_R52_SPLIT_0_SSW__HPPS_SMP_APP 1
#define HPPS_MBOX1_CHAN__HPPS_SMP_APP__RTPS_R52_SPLIT_1_SSW 2
#define HPPS_MBOX1_CHAN__RTPS_R52_SPLIT_1_SSW__HPPS_SMP_APP 3
#define HPPS_MBOX1_CHAN__HPPS_SMP_APP__RTPS_R52_SMP_SSW 0
#define HPPS_MBOX1_CHAN__RTPS_R52_SMP_SSW__HPPS_SMP_APP 1

#endif // MAILBOX_MAP_H
