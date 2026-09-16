#pragma once

// The advanced screen: every editable motion parameter on one page, so a tuning
// session does not need a rebuild (see docs/joycon-input.md). Drawing only, like
// the other screens, so tests/canvas renders it on a PC.

#include <dglab/nro/motion_settings.h>
#include <dglab/ui/canvas.h>
#include <dglab/ui/text.h>

typedef struct {
    const DglabMotionFeedConfig* config;
    unsigned selected;   ///< DglabMotionSetting
    bool saved;          ///< the last change reached the config file
} DglabAdvancedState;

void dglabAdvancedDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabAdvancedState* state);
