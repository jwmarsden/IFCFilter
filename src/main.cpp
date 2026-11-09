#include "mapping.h"
#include "report.h"
#include "split.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int usage(int code) {
    (code ? std::cerr : std::cout)
        << "usage: ifcfilter report MODEL.ifc [--csv FAMILIES.csv] [--suspects ELEMENTS.csv] [--rules rules.txt] [--top N] [--heavy-faces N] [--transparent T]\n"
        << "       ifcfilter split MODEL.ifc|FOLDER... --out DIR [--rules rules.txt] [--transparent T] [--mapping mapping.csv] [--no-merge] [--merge-gap M] [--no-instance] [--no-compact] [--precision-mm MM] [--no-zip] [--jobs N] [--all]\n"
        << "       ifcfilter check MODEL.ifc|FOLDER... --out DIR\n"
        << "       ifcfilter inventory MODEL.ifc|FOLDER... --mapping mapping.csv [--jobs N]\n";
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage(2);
    const std::string command = argv[1];
    if (command == "-h" || command == "--help") return usage(0);
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
