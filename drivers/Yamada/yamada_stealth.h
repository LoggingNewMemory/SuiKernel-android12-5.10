#ifndef _YAMADA_STEALTH_H
#define _YAMADA_STEALTH_H

#include <linux/types.h>

/* Global variables for SuiKernel Manager Stealth IOCTL */
extern bool anya_thermal_enabled;
extern bool yamada_boost_enabled;
extern int  yamada_boost_duration;
extern bool inaho_enabled;
extern bool tenebrion_enabled;
extern int  sparxie_swappiness_val;
extern bool airani_enabled;
extern bool sandevistan_enabled;

/* Magic IOCTL Commands for /dev/null */
#define CMD_ANYA_THERMAL 0x392C8989
#define CMD_YAMADA_BOOST 0x4E1F48AE
#define CMD_YAMADA_BOOST_DUR 0x2605423F
#define CMD_INAHO_AUDIO 0x1281817A
#define CMD_TENEBRION 0x11EAF5CB
#define CMD_SPARXIE_SWAP 0x1B3FB033
#define CMD_AIRANI_CPUSET 0x2FC6501B
#define CMD_SANDEVISTAN 0x27B55B18

#endif /* _YAMADA_STEALTH_H */
