/*
 * DirectDraw HAL XVidMode interface
 *
 * Copyright 2001 TransGaming Technologies, Inc.
 */
#ifndef __WINE_XVIDMODE_H
#define __WINE_XVIDMODE_H
#include "config.h"
#ifdef HAVE_LIBXXF86VM
#include "ddrawi.h"

extern LPDDHALMODEINFO xf86vm_modes;
extern unsigned xf86vm_mode_count;

void X11DRV_XF86VM_Init(void);
void X11DRV_XF86VM_Cleanup(void);
int X11DRV_XF86VM_GetCurrentMode(void);
void X11DRV_XF86VM_SetCurrentMode(int mode);
void X11DRV_XF86VM_SetExclusiveMode(int lock);

#endif /* HAVE_LIBXXF86VM */
#endif /* __WINE_XVIDMODE_H */
