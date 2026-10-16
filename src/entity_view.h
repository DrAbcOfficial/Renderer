#pragma once

//Internal seam of the entity view scope (entity_view.cpp). The scene renderers
//only ask whether an isolated view is active; the plugin entry points install
//and release the Studio hooks.
struct engine_studio_api_s;
struct alight_s;

bool EntityView_IsActive();

//Copies the light of the active view into *Light; false without a view.
bool EntityView_ApplyLight(alight_s* Light);

void EntityView_InstallHooks(engine_studio_api_s* Studio);
void EntityView_UninstallHooks();

//Releases the private UBOs whenever the GL context goes down.
void EntityView_FreeBuffers();
