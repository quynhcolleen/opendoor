#ifndef OPENDOOR_TUI_H
#define OPENDOOR_TUI_H

#include "opendoor/app.h"

typedef void (*OdTuiBlockingStep)(void *context);

void od_tui_run_flushed_interstitial(OdTuiBlockingStep render,
                                     OdTuiBlockingStep operation,
                                     void *context);
int od_tui_run(const OpendoorOptions *options);

#endif
