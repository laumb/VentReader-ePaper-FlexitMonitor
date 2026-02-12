#pragma once
#include <Arduino.h>
#include "flexit_types.h"

// Init ePaper + fonts
void ui_init();
void ui_set_language(const String& lang);

// Render main dashboard
void ui_render(const FlexitData& d, const String& mbStatus);

// Render onboarding/provisioning screen shown at boot when device runs fallback AP
void ui_render_onboarding(const String& apSsid, const String& apPass, const String& ip, const String& url);

// Force a hard reset + full clear of the e-paper panel.
// Use on first boot / after flashing.
void ui_epaper_hard_clear();
