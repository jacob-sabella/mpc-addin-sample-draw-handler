/* Internal to the addin: the core (sdraw.c) and the control side (ctl.c). */
#pragma once
#include "mpc_sample_draw.h"
#include <stddef.h>
#include <stdio.h>

#define LOG(...) fprintf(stderr, "mpc-sample-draw-handler: " __VA_ARGS__)
#define SDRAW_W 320
#define SDRAW_H 240
#define SDRAW_MAX_CLIENTS 16

const mpc_sdraw_api *mpc_sample_draw_api(int version);
void sdraw_enable(void);                        /* start watching MPC's display calls (the constructor calls it) */
int  sdraw_status_json(char *out, size_t n);    /* the clients, the owner and the display, for the control interface */
int  sdraw_grant(int id);                       /* control: give client id the screen (it gets a focus callback) */
void sdraw_take_back(void);                     /* control: give MPC its screen back (the owner gets a callback) */
