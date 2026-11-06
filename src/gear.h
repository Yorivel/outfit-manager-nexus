// Worn gear, the player and the characters, the bags and where pieces are.
#pragma once
#include "om.h"

namespace om {

U64 equipped_item(U64 list, U16 slot);
U64 equipment_list();
extern std::vector<U16> g_placeholder_keys;
int read_worn(U64 self, WornRaw* out, int max);
U64 back_shield_item();
int anchor_outfit();
bool left_anchor(const Outfit& o, const WornRaw* w, int n);
void update_plus_icon(U64 self);
void carry_looks(const Outfit& from, Outfit& to);
bool snapshot(U64 self, Outfit& o);
bool hands_crossed(const Outfit& o, const WornRaw* w, int n, U16 slot);
bool same_saved(const Outfit& a, const Outfit& b);
int find_worn_outfit(U64 self);
U64 acquire_player(U64* ctx);
void release_player(U64* ctx);
U16 character_of(U64 actor);
U16 current_character();
U64 equip_bags(bool companion);
bool find_in_bags(U64 mgr, U64 uid, ItemAt& at, U64 allowed = 0, U16 key = 0xFFFF);
U64 find_copy(U64 uid, U16 key);
void log_bags(U64 mgr);
U64 slot_claim(U16 slot);
const OutfitItem* outfit_piece(const Outfit& o, U16 slot);
int read_bags_raw(U64 mgr, Owned* out, int max);
bool read_owned(U64 self, std::vector<Owned>& out);
const Owned* find_owned(const std::vector<Owned>& owned, U64 uid, U16 key);
bool playing_main_character();
PieceCheck check_pieces(const Outfit& o, const std::vector<Owned>& owned);
extern U64 g_missing_checked;
bool adopt_returned(Outfit& o, const std::vector<Owned>& owned);
bool refresh_missing(U64 self);
bool outfit_missing(size_t o);
const char* slot_name(U16 slot);

} // namespace om
