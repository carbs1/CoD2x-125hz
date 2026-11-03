#ifndef COD2X_TICKRATE_HISTORY_H
#define COD2X_TICKRATE_HISTORY_H
void tickrate_history_init();
bool tickrate_history_can_reload();
int tickrate_history_capacity();
unsigned int tickrate_history_snapshot_count();
int tickrate_history_snapshot_time();
#endif
