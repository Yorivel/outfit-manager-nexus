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

} // namespace om
