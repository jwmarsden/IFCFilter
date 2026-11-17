#include "uniclass.h"
#include "system.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <vector>
namespace ifcfilter {
namespace {
std::string upper(std::string s) { for (char& c:s) c=static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; }
// One line of CSV, split at commas outside quotes
std::vector<std::string> csv_line(const std::string& s) {
    std::vector<std::string> fields(1); bool quoted=false;
    for(size_t i=0;i<s.size();++i) {
        char c=s[i];
        if(c=='"') { if(quoted && i+1<s.size() && s[i+1]=='"') {fields.back()+='"';++i;} else quoted=!quoted; }
        else if(c==',' && !quoted) fields.emplace_back();
        else if(c!='\r') fields.back()+=c;
    }
    return fields;
}
// Every active code's title, from the NBS Uniclass tables copied into a uniclass folder beside the program,
// read once. Columns used: 1 the code, 6 its title, 7 whether it's active. Codes are compared in upper case.
const std::map<std::string,std::string>& titles() {
    static const auto data=[] {
        std::map<std::string,std::string> result;
        const std::array<const char*,6> files={"V0006_Uniclass_Complex_122.csv","V0006_Uniclass_Entity_134.csv","V0006_Uniclass_Space_Location_135.csv","V0006_Uniclass_Element_Function_116.csv","V0006_Uniclass_System_142.csv","V0006_Uniclass_Product_142.csv"};
        auto dir=std::filesystem::path(program_path()).parent_path()/"uniclass";
        if (const char* shared=std::getenv("IFC_DATA_DIR"); shared && *shared) dir=std::filesystem::path(shared)/"uniclass";
        else if (const char* profile=std::getenv("USERPROFILE"); profile && *profile) {
            auto shared=std::filesystem::path(profile)/"OneDrive"/"Work"/"IFCData"/"uniclass";
            if (std::filesystem::is_directory(shared)) dir=shared;
        }
        for(const auto* file:files) {
            std::ifstream in(dir/file,std::ios::binary); if(!in) throw std::runtime_error("missing Uniclass table: "+(dir/file).string());
            std::string line; std::getline(in,line);
            while(std::getline(in,line)) { auto fields=csv_line(line); if(fields.size()>=8 && upper(fields[7])=="TRUE") result[upper(fields[1])]=fields[6]; }
        }
        return result;
    }(); return data;
}
}
std::string uniclass_title(const std::string& code) { auto& data=titles(); auto at=data.find(upper(code)); return at==data.end()?"":at->second; }
}
