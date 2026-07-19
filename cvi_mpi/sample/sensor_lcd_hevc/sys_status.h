#ifndef SYS_STATUS_H
#define SYS_STATUS_H

#include <stdbool.h>

typedef struct SYS_STATUS_S {
	bool battery_valid;
	int battery_pct;
	bool temp_valid;
	int temp_c;
} SYS_STATUS_S;

/* Poll battery/temp on a background thread (non-blocking for preview). */
int sys_status_start(void);
void sys_status_stop(void);
void sys_status_get(SYS_STATUS_S *st);

#endif /* SYS_STATUS_H */
