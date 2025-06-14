#ifndef DYNOS_C_H
#define DYNOS_C_H
#ifndef __cplusplus

#include "dynos.h"
#include "types.h"
#include "engine/behavior_script.h"
#include "game/moving_texture.h"
#include "lua.h"

void *dynos_swap_cmd(void *cmd);

// -- built in -- //
void *dynos_update_cmd(void *cmd);
void  dynos_update_gfx();
s32   dynos_tex_import(void **output, void *ptr, s32 tile, void *grapi, void **hashmap, void *pool, s32 *poolpos, s32 poolsize);
void  dynos_gfx_swap_animations(void *ptr);

// -- warps -- //
LevelScript* dynos_get_level_script(const char* scriptEntryName);
bool dynos_warp_to_warpnode(s32 aLevel, s32 aArea, s32 aAct, s32 aWarpId);
bool dynos_warp_to_level(s32 aLevel, s32 aArea, s32 aAct);
bool dynos_warp_restart_level(void);
bool dynos_warp_to_start_level(void);
bool dynos_warp_exit_level(s32 aDelay);
bool dynos_warp_to_castle(s32 aLevel);

// -- dynos packs -- //
void dynos_gfx_init(void);
int dynos_pack_get_count(void);
const char* dynos_pack_get_name(s32 index);
bool dynos_pack_get_enabled(s32 index);
void dynos_pack_set_enabled(s32 index, bool value);
bool dynos_pack_get_exists(s32 index);
void dynos_generate_packs(const char* directory);

// -- geos -- //
void dynos_actor_override(struct Object* obj, void** aSharedChild);
void dynos_add_actor_custom(s32 modIndex, s32 modFileIndex, const char *filePath, const char* geoName);
const void* dynos_geolayout_get(const char *name);
bool dynos_actor_get_mod_index_and_token(struct GraphNode *graphNode, u32 tokenIndex, s32 *modIndex, s32 *modFileIndex, const char **token);
void dynos_actor_register_modified_graph_node(struct GraphNode *node);

// -- collisions -- //
void dynos_add_collision(const char *filePath, const char* collisionName);
Collision* dynos_collision_get(const char* collisionName);

// -- textures -- //
void dynos_add_texture(const char *filePath, const char* textureName);
bool dynos_texture_get(const char* textureName, struct TextureInfo* outTextureInfo);
bool dynos_texture_get_from_data(const Texture *tex, struct TextureInfo* outTextureInfo);
void dynos_texture_override_set(const char* textureName, struct TextureInfo* overrideTextureInfo);
void dynos_texture_override_reset(const char* textureName);

// -- movtexqcs -- //
void dynos_movtexqc_register(const char* name, s16 level, s16 area, s16 type);
struct MovtexQuadCollection* dynos_movtexqc_get_from_id(u32 id);
struct MovtexQuadCollection* dynos_movtexqc_get_from_index(s32 index);

// -- levels -- //
void dynos_add_level(s32 modIndex, const char *filePath, const char* levelName);
const char* dynos_level_get_token(u32 index);
Trajectory* dynos_level_get_trajectory(const char* name);
void dynos_level_load_background(void *ptr);
u64 dynos_level_cmd_get(void *cmd, u64 offset);
void dynos_level_cmd_next(void *cmd);
void dynos_level_parse_script(const void *script, s32 (*aPreprocessFunction)(u8, void *));
void* dynos_level_get_script(s32 level);
s32 dynos_level_get_mod_index(s32 level);
bool dynos_level_is_vanilla_level(s32 level);
Collision *dynos_level_get_collision(u32 level, u16 area);

// -- behaviors -- //
void dynos_add_behavior(s32 modIndex, const char *filePath, const char *behaviorName);
bool dynos_behavior_get_active_mod_index(BehaviorScript *bhvScript, s32 *modIndex, s32 *modFileIndex);
const char *dynos_behavior_get_token(BehaviorScript *bhvScript, u32 index);
void dynos_behavior_hook_all_custom_behaviors(void);

// -- models -- //
struct GraphNode* dynos_model_load_geo(u32* aId, enum ModelPool aModelPool, void* aAsset, bool aDeDuplicate);
struct GraphNode* dynos_model_load_dl(u32* aId, enum ModelPool aModelPool, u8 aLayer, void* aAsset);
struct GraphNode* dynos_model_store_geo(u32* aId, enum ModelPool aModelPool, void* aAsset, struct GraphNode* aGraphNode);
struct GraphNode* dynos_model_get_geo(u32 aId);
void dynos_model_overwrite_slot(u32 srcSlot, u32 dstSlot);
u32 dynos_model_get_id_from_asset(void* aAsset);
u32 dynos_model_get_id_from_graph_node(struct GraphNode* aGraphNode);
void dynos_model_clear_pool(enum ModelPool aModelPool);

// -- gfx -- //
Gfx *dynos_gfx_get_writable_display_list(Gfx* gfx);
Gfx *dynos_gfx_get(const char *name, u32 *outLength);
Gfx *dynos_gfx_create(const char *name, u32 length);
bool dynos_gfx_resize(Gfx *gfx, u32 newLength);
bool dynos_gfx_delete(Gfx *gfx);
void dynos_gfx_delete_all();
Vtx *dynos_vtx_get(const char *name, u32 *outCount);
Vtx *dynos_vtx_create(const char *name, u32 count);
bool dynos_vtx_resize(Vtx *vtx, u32 newCount);
bool dynos_vtx_delete(Vtx *vtx);
void dynos_vtx_delete_all();

// -- other -- //
void dynos_mod_shutdown(void);
void dynos_add_scroll_target(u32 index, const char *name, u32 offset, u32 size);
u32 dynos_mod_data_get_last_error();

// -- smlua -- //
bool dynos_smlua_parse_gfx_command(lua_State *L, Gfx *gfx, const char *command, bool hasSpecifiers, char *errorMsg, u32 errorSize);
void dynos_smlua_clear_gfx_command_cache();

#endif
#endif
