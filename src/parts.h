// Visibility Extras: the model parts a worn piece brings (parts.cpp).
#pragma once
#include <string>
#include <vector>
#include "om.h"

namespace om {

struct ItemPart {
    int handle = 0;   // (the visibility calls' handle)
    std::string name; // (the model's part name, "Upperbody_Acc")
};
bool parts_install();
bool parts_on();
std::vector<ItemPart> item_parts(U64 inv, U64 model, U16 slot);
std::string part_group(const std::string& name);
// Working parts (the Tech Gloves crash): an item whose part the game builds with a component of its own is never taken
// off the presentation. Known per permanent id: 1 working, 0 plain, -1 not checked yet.
int working_part_known(U16 key);
int working_part_check(U64 inv, U64 model, U16 slot, U16 key); // (the game's own build of the slot: checks + records)
std::string working_parts_line(bool working);                   // (the outfits file: hex permanent ids)
void working_parts_read(const std::string& ids, bool working);
bool working_parts_dirty();                                      // (a result recorded since: the file is written)
U32 working_parts_in_use(U64 inv, U64 model);                     // (slots whose working part is in use now)

} // namespace om
