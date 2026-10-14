#pragma once

// New implementation. Scoped Studio hooks shared with gl_studio/exportfuncs;
// GPU setup and the public optional interface live in ModelPreview.cpp.
struct engine_studio_api_s;
struct alight_s;

namespace modelpreview {
bool isActive();
bool setupLight(alight_s* Light);
void install(engine_studio_api_s* Studio);
void uninstall();
}
