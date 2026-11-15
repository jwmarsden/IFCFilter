#pragma once

// mapping.csv: every class, text attribute, property set and property in the models, with what to do with
// each in the federation file. `ifcfilter inventory` writes it from the inputs, keeping any actions already in
// it, and `ifcfilter split --mapping` carries them out.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ifcfilter {

struct Mapping {
    std::map<std::string, std::string> retype;                         // class, in capitals, and the class it becomes
    std::set<std::pair<std::string, std::string>> blank;               // class, in capitals, and an attribute to leave empty
    std::set<std::string> delete_set;                                  // property and quantity sets, by name
    std::map<std::string, std::string> rename_set;
    std::set<std::pair<std::string, std::string>> delete_property;     // set, and property in it
    std::map<std::pair<std::string, std::string>, std::string> rename_property;
    bool empty() const;
};

// Reads mapping.csv; throws if an action isn't one it knows
Mapping load_mapping(const std::string& path);

struct MappingStats {
    uint64_t retyped = 0, blanked = 0;
    uint64_t sets_deleted = 0, sets_renamed = 0, sets_emptied = 0;
    uint64_t properties_deleted = 0, properties_renamed = 0;
    std::vector<std::string> warnings;  // what couldn't be done, once each
};

// Rewrites an IFC file in place, as the mapping says
MappingStats apply_mapping(const std::string& path, const Mapping& mapping);

// Writes csv_path from the models in inputs (files and folders of them), keeping the actions and new names of
// rows already in it; rows the models no longer have stay, with no count
int inventory(const std::vector<std::string>& inputs, const std::string& csv_path, unsigned jobs);

}  // namespace ifcfilter
