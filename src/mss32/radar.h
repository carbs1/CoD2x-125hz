#ifndef RADAR_H
#define RADAR_H

void radar_draw();
bool radar_compassActive();
void radar_drawCompass(float rectX, float rectY, float rectW, float rectH, int horzAlign, int vertAlign);
void radar_frame();
void radar_unload();
void radar_init();
void radar_patch();

#endif