#pragma once
#include <Arduino.h>

// WebUI + JSON API on port 80.
// Auth: POST /api/login {"pin":"123456"} -> {"token":"..."}.
// Every other /api/* call needs header "Authorization: Bearer <token>".
// Tokens expire after 5 minutes of inactivity. 5 wrong PINs -> 5 minute lockout.
// Bearer tokens are not sent automatically by browsers, so no CSRF token is needed.
namespace Web {

bool begin(bool filesystemReady);
void tick();                 // handles scheduled restarts after config save
void requestRestart(uint32_t delayMs = 1500);

}  // namespace Web
