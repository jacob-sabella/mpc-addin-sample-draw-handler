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
int  sdraw_snapshot(uint32_t *out);             /* control: the screen as shown, SDRAW_W x SDRAW_H 0x00RRGGBB; -1 unmapped */
int  sdraw_grant(int id);                       /* control: give client id the screen (it gets a focus callback) */
void sdraw_take_back(void);                     /* control: give MPC its screen back (the owner gets a callback) */
void sdraw_cycle(void);                         /* the page button: MPC <-> the home page (else MPC -> each client -> MPC) */
void sdraw_set_home(const char *name);          /* page_home: the client the page button opens ("" = cycle) */
void input_config(int page_note, int hold_ms, int log_buttons);
void input_controls(const int keys[4], const int knobs[3], int dial_cc);   /* notes / CCs of the controls a client can take */
int  sdraw_input_wanted(void);                  /* the client with the screen takes the controls */
int  sdraw_input_owner(unsigned *gen);         /* that client's id, or 0 (no owner, or it takes no input); *gen: hand-overs */
void sdraw_deliver(const mpc_sdraw_event *e);   /* hand an event to that client (addin thread, no input lock held) */
void input_enable(void);                        /* start watching MPC's button reads */
