// ifcfilter: prepares IFC models for federation. Each command's options are read here and handed to the
// module that does it; the README says what each does.

#include "cache.h"
#include "combine.h"
#include "mapping.h"
#include "report.h"
#include "split.h"
#include "site.h"
#include "site_inventory.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int usage(int code) {
    (code ? std::cerr : std::cout)
        << "usage: ifcfilter report MODEL.ifc [--csv FAMILIES.csv] [--suspects ELEMENTS.csv] [--rules rules.txt] [--top N] [--heavy-faces N] [--transparent T]\n"
        << "       ifcfilter split MODEL.ifc|FOLDER... --out DIR [--rules rules.txt] [--transparent T] [--mapping mapping.csv] [--site-lookup site-id-lookup.xlsx] [--location-reference location-referencing.xlsx] [--property-schema site-id-attributes.xlsx] [--combine combine.csv] [--no-merge] [--merge-gap M] [--no-instance] [--no-compact] [--precision-mm MM] [--simplify-mm MM] [--no-zip] [--jobs N] [--all]\n"
        << "       ifcfilter check MODEL.ifc|FOLDER... --out DIR\n"
        << "       ifcfilter cache MODEL.ifc [MODEL.ifc...] [--out CACHE.ifcc] [--detail | --detail-element GUID] [--if-changed] [--rules rules.txt]\n"
        << "       ifcfilter inventory MODEL.ifc|FOLDER... --mapping mapping.csv [--jobs N]\n"
        << "       ifcfilter combine FEDERATE.ifc|FOLDER... --out DIR [--register combine.csv] [--name-part N]\n"
        << "       ifcfilter site-uniclass-inventory MODEL.ifc|FOLDER... --rules rules.txt --out SITES.csv\n"
        << "       Any command: --data-dir DIR (or set IFC_DATA_DIR)\n";
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--data-dir") continue;
        if (i + 1 >= argc) { std::cerr << "--data-dir needs a directory\n"; return 2; }
#ifdef _WIN32
        if (_putenv_s("IFC_DATA_DIR", argv[i + 1]) != 0) { std::cerr << "Cannot set IFC_DATA_DIR\n"; return 2; }
#else
        if (setenv("IFC_DATA_DIR", argv[i + 1], 1) != 0) { std::cerr << "Cannot set IFC_DATA_DIR\n"; return 2; }
#endif
        for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
        argc -= 2;
        argv[argc] = nullptr;
        --i;
    }
    if (argc < 2) return usage(2);
    const std::string command = argv[1];
    if (command == "-h" || command == "--help") return usage(0);
    // The Uniclass codes the models give each site, for review (site_inventory.h)
    if (command == "site-uniclass-inventory") {
        std::vector<std::string> inputs;
        std::string rules, output;
        for (int i=2;i<argc;++i) {
            const std::string arg=argv[i];
            if (arg=="--rules" && i+1<argc) rules=argv[++i];
            else if (arg=="--out" && i+1<argc) output=argv[++i];
            else if (!arg.empty() && arg[0]=='-') return usage(2);
            else inputs.push_back(arg);
        }
        if (inputs.empty() || rules.empty() || output.empty()) return usage(2);
        try { return ifcfilter::inventory_site_uniclass(inputs,rules,output); }
        catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
    }
    // The main run: split each model into federation, hidden and envelope files, merge, tidy and write (split.h)
    if (command == "split") {
        ifcfilter::SplitOptions options;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto value = [&]() -> std::string {
                if (++i >= argc) {
                    std::cerr << arg << " needs a value\n";
                    std::exit(2);
                }
                return argv[i];
            };
            if (arg == "--out") options.out_dir = value();
            else if (arg == "--rules") options.rules_path = value();
            else if (arg == "--mapping") options.mapping_path = value();
            else if (arg == "--site-lookup") options.site_lookup_path = value();
            else if (arg == "--location-reference") options.location_reference_path = value();
            else if (arg == "--property-schema") options.property_schema_path = value();
            else if (arg == "--combine") options.combine_path = value();
            else if (arg == "--transparent") options.transparent = std::stod(value());
            else if (arg == "--jobs") options.jobs = std::stoi(value());
            else if (arg == "--worker-shared") options.worker_shared = value();
            else if (arg == "--no-merge") options.merge = false;
            else if (arg == "--all") options.all = true;
            else if (arg == "--merge-gap") options.merge_gap_m = std::stod(value());
            else if (arg == "--no-instance") options.instance = false;
            else if (arg == "--no-compact") options.compact = false;
            else if (arg == "--no-zip") options.zip = false;
            else if (arg == "--precision-mm") options.precision_mm = std::stod(value());
            else if (arg == "--simplify-mm") options.simplify_mm = std::stod(value());
            else if (!arg.empty() && arg[0] == '-') return usage(2);
            else options.inputs.push_back(arg);
        }
        if (options.inputs.empty() || options.out_dir.empty()) return usage(2);
        try {
            return ifcfilter::split(options);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }
    // A viewing cache of one model, or of several in one (cache.h)
    if (command == "cache") {
        std::vector<std::string> inputs;
        std::string output;
        ifcfilter::CacheOptions options;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--out" && i + 1 < argc) output = argv[++i];
            else if (arg == "--detail") options.detail = true;
            else if (arg == "--detail-element" && i + 1 < argc) options.detail_elements.push_back(argv[++i]);
            else if (arg == "--if-changed") options.if_changed = true;
            else if (arg == "--rules" && i + 1 < argc) ifcfilter::load_schema(argv[++i]);
            else if (!arg.empty() && arg[0] == '-') return usage(2);
            else inputs.push_back(arg);
        }
        if (inputs.empty() || (inputs.size() > 1 && output.empty()) || (options.detail && !options.detail_elements.empty())) return usage(2);
        if (output.empty()) output = std::filesystem::path(inputs[0]).replace_extension(".ifcc").string();
        return ifcfilter::cache(inputs, output, options);
    }
    // Compare the models with the register, without changing anything (register.h)
    if (command == "check") {
        std::vector<std::string> inputs;
        std::string out_dir;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--out" && i + 1 < argc) out_dir = argv[++i];
            else if (!arg.empty() && arg[0] == '-') return usage(2);
            else inputs.push_back(arg);
        }
        if (inputs.empty() || out_dir.empty()) return usage(2);
        try {
            return ifcfilter::check(inputs, out_dir);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 2;
        }
    }
    // Write federation files together as one, by combine.csv or by zone (combine.h)
    if (command == "combine") {
        ifcfilter::CombineOptions options;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--out" && i + 1 < argc) options.out_dir = argv[++i];
            else if (arg == "--name-part" && i + 1 < argc) options.name_part = std::stoi(argv[++i]);
            else if (arg == "--register" && i + 1 < argc) options.register_path = argv[++i];
            else if (!arg.empty() && arg[0] == '-') return usage(2);
            else options.inputs.push_back(arg);
        }
        if (options.inputs.empty() || options.out_dir.empty()) return usage(2);
        try {
            return ifcfilter::combine(options);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }
    // List every class, attribute, set and property into mapping.csv, keeping its actions (mapping.h)
    if (command == "inventory") {
        std::vector<std::string> inputs;
        std::string csv_path;
        unsigned jobs = 0;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--mapping" && i + 1 < argc) csv_path = argv[++i];
            else if (arg == "--jobs" && i + 1 < argc) jobs = static_cast<unsigned>(std::stoul(argv[++i]));
            else if (!arg.empty() && arg[0] == '-') return usage(2);
            else inputs.push_back(arg);
        }
        if (inputs.empty() || csv_path.empty()) return usage(2);
        try {
            return ifcfilter::inventory(inputs, csv_path, jobs);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }
    if (command != "report") return usage(2);

    // Where a model's bytes go, by family, with suspiciously large elements to look at (report.h)

    ifcfilter::ReportOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (++i >= argc) {
                std::cerr << arg << " needs a value\n";
                std::exit(2);
            }
            return argv[i];
        };
        if (arg == "--csv") options.csv_path = value();
        else if (arg == "--suspects") options.suspects_path = value();
        else if (arg == "--rules") options.rules_path = value();
        else if (arg == "--top") options.top_families = std::stoul(value());
        else if (arg == "--heavy-faces") options.heavy_faces = std::stoul(value());
        else if (arg == "--transparent") options.transparent = std::stod(value());
        else if (!arg.empty() && arg[0] == '-') return usage(2);
        else if (options.ifc_path.empty()) options.ifc_path = arg;
        else return usage(2);
    }
    if (options.ifc_path.empty()) return usage(2);
    return ifcfilter::report(options);
}




