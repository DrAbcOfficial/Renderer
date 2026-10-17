#pragma once

//Internal seam of the entity view scope (entity_view.cpp). The scene renderers
//only ask whether an isolated view is active and, in one place, how to draw
//the entity a view was opened for; the plugin entry points install and release
//the Studio hooks.
struct engine_studio_api_s;
struct alight_s;
struct cl_entity_s;

bool EntityView_IsActive();

//Copies the light of the active view into *Light; false without a view.
bool EntityView_ApplyLight(alight_s* Light);

//Camera parameters of the active view for the scene pipeline; zero without a
//view.
float EntityView_GetVerticalFov();
float EntityView_GetNearPlane();
float EntityView_GetFarPlane();

//Draws Entity through the scene pipeline while a view is active; no-op without
//a view.
void EntityView_DrawCurrentEntity(cl_entity_s* Entity);

void EntityView_InstallHooks(engine_studio_api_s* Studio);
void EntityView_UninstallHooks();
