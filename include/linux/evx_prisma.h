#ifndef _LINUX_EVX_PRISMA_H
#define _LINUX_EVX_PRISMA_H

#include <linux/types.h>

/* Private SECOND-NEES prctl operation: ASCII "EVXP". */
#define PR_EVX_PRISMA_CHALLENGE 0x45565850

#define EVX_PRISMA_PROTOCOL_VERSION 1UL
#define EVX_PRISMA_NONCE_SIZE 32UL
#define EVX_PRISMA_RESPONSE_SIZE 32UL

long evx_prisma_prctl(unsigned long nonce_address,
                      unsigned long response_address,
                      unsigned long nonce_size,
                      unsigned long protocol_version);

#endif
