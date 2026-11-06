// The outfits file (OutfitManager.outfits): reading, writing, one list per character.
#pragma once
#include "om.h"

namespace om {

CharacterOutfits& store_for(U16 character);
size_t outfit_total(const Store& s);
extern std::mutex g_store_mu;
extern Store g_published;
extern std::map<U32, BoundSet> g_bound;
extern SaveId g_loaded_id;
extern bool g_loaded_known;
extern std::vector<PendingWrite> g_pending_writes;
extern std::vector<PendingWrite> g_unfinished;
extern std::atomic<U32> g_generation;
extern U32 g_seen_generation;
extern bool g_store_unreadable;
extern const char* g_store_why;
extern std::vector<U32> g_placeholder_ids;
bool write_store_file();
bool pull_active();
void save_outfits();
void load_outfits();
bool switch_character(U16 character);

} // namespace om
