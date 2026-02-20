#pragma once

#include <string>
#include <vector>

struct CVarCatalogEntry {
    std::string name;  // CVar key (ex: "gSettings.Menu.Popout")
    std::string label; // Widget label (ex: "Popout Menu")
};

namespace CVarCatalog {
void Register(const char* cvarName, const char* label);
const std::vector<CVarCatalogEntry>& GetAll();
} // namespace CVarCatalog
