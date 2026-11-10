#pragma once

// Federation files of one zone, written as one: one project, one set of units and contexts, one site and one
// building, with levels of the same name as one level. Everything else is copied as it is, renumbered so the
// files don't clash; GUIDs are already unique across a run. What a dropped site, building or level placed is
// placed from what took its place instead, where it was, so nothing moves.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ifcfilter {

struct CombineOptions {
    std::vector<std::string> inputs;  // federation files, and folders of them
    std::string out_dir;
    int name_part = 3;                 // which part of a file's name, split at '-', from 1, says what goes together: T2DPAA-T2D-N5T-... is N5T
    // A CSV that says, model by model, what each goes into, instead of going by name_part. If it isn't there, it's
    // written, listing every model with what it holds and nothing filled in, and nothing is combined. If it is,
    // models not in it yet are added, and each name in its combined column is written as one file.
    std::string register_path;
};

// Returns a process exit code
int combine(const CombineOptions& options);

// A register's groups: each model, by its name, and the name of what it goes into. Models left blank aren't in it.
std::map<std::string, std::string> combine_groups(const std::string& register_path);

// Writes federation files as one, at out_path. Returns false, having said why, if they can't be; summary says
// what it came to.
bool combine_files(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& out_path, std::string& summary);

}  // namespace ifcfilter
