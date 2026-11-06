#include "report.h"
#include "split.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int usage(int code) {
    (code ? std::cerr : std::cout)
        << "usage: bimpass report MODEL.ifc [--csv FAMILIES.csv] [--suspects ELEMENTS.csv] [--top N] [--heavy-faces N] [--transparent T]\n"
        << "       bimpass split MODEL.ifc --out DIR [--rules rules.txt] [--transparent T] [--no-compact] [--precision-mm MM] [--no-zip]\n";
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage(2);
    const std::string command = argv[1];
    if (command == "-h" || command == "--help") return usage(0);
    if (command == "split") {
        bimpass::SplitOptions options;
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
            else if (arg == "--transparent") options.transparent = std::stod(value());
            else if (arg == "--no-compact") options.compact = false;
            else if (arg == "--no-zip") options.zip = false;
            else if (arg == "--precision-mm") options.precision_mm = std::stod(value());
            else if (!arg.empty() && arg[0] == '-') return usage(2);
            else if (options.ifc_path.empty()) options.ifc_path = arg;
            else return usage(2);
        }
        if (options.ifc_path.empty() || options.out_dir.empty()) return usage(2);
        try {
            return bimpass::split(options);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }
    if (command != "report") return usage(2);

    bimpass::ReportOptions options;
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
        else if (arg == "--top") options.top_families = std::stoul(value());
        else if (arg == "--heavy-faces") options.heavy_faces = std::stoul(value());
        else if (arg == "--transparent") options.transparent = std::stod(value());
        else if (!arg.empty() && arg[0] == '-') return usage(2);
        else if (options.ifc_path.empty()) options.ifc_path = arg;
        else return usage(2);
    }
    if (options.ifc_path.empty()) return usage(2);
    return bimpass::report(options);
}
