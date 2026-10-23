#ifndef _IOCTL_LIBRARY_H
#define _IOCTL_LIBRARY_H

#include <linux/types.h>

/* Global variables for SuiKernel Manager IOCTL */
extern bool anya_thermal_enabled;
extern void yamada_touch_boost_off(void);
extern void yamada_touch_boost_set_mode(int mode);
extern bool yamada_touch_boost_enabled;
extern int  yamada_touch_boost_duration;
extern bool inaho_enabled;
extern void inaho_set_enabled(bool enabled);
extern bool tenebrion_enabled;
extern void tenebrion_set_enabled(bool enabled);
extern int  sparxie_swappiness_val;
extern void sparxie_set_swappiness(int val);
extern bool airani_enabled;
extern bool sandevistan_enabled;
extern bool moona_enabled;
extern void moona_set_enabled(bool enabled);

/* Magic IOCTL Commands for /dev/null */
#define CMD_ANYA_THERMAL 0x392C8989
#define CMD_YAMADA_TOUCH_BOOST_DISABLE  0x7A1F2C3B
#define CMD_YAMADA_TOUCH_BOOST_BALANCED 0x5C3E8A1D
#define CMD_YAMADA_TOUCH_BOOST_GAMING   0x3B9C4E2F
#define CMD_INAHO_AUDIO 0x1281817A
#define CMD_TENEBRION 0x11EAF5CB
#define CMD_SPARXIE_SWAP 0x1B3FB033
#define CMD_AIRANI_CPUSET 0x2FC6501B
#define CMD_SANDEVISTAN 0x27B55B18
#define CMD_MOONA_ZRAM 0x3E8B1A4C

#define CMD_SUI_ELEVATE_ROOT 0x53554901
#define SUI_ELEVATE_MAGIC    0x59414D4144415355ULL /* "YAMADASU" */

#endif /* _IOCTL_LIBRARY_H */
