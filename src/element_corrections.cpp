#include "element_corrections.h"
#include "site.h"
#include "spf.h"
#include "xlsx.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace ifcfilter {
namespace fs = std::filesystem;
namespace {
std::string trim(std::string s) {
    const auto first=s.find_first_not_of(" \t\r\n"), last=s.find_last_not_of(" \t\r\n");
    return first==std::string::npos ? "" : s.substr(first,last-first+1);
}
std::string upper(std::string s) { for(char& c:s)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));return s; }
}
fs::path element_correction_path(const fs::path& input, const std::string& schema_path) {
    fs::path data;
    if(const char* configured=std::getenv("IFC_DATA_DIR");configured && *configured) data=configured;
    else data=fs::path(schema_path).parent_path();
    return data / "site-id-corrections" / fs::absolute(input).parent_path().filename() / (input.stem().string()+".xlsx");
}
ElementCorrections load_element_corrections(const fs::path& path, const fs::path& input, const SpfFile& spf) {
    ElementCorrections result;
    if(!fs::is_regular_file(path)) return result;
    const auto rows=read_xlsx(path.string());
    if(rows.empty()) throw std::runtime_error(path.string()+": empty correction workbook");
    std::map<std::string,size_t> columns;
    for(size_t i=0;i<rows[0].size();++i) {
        const auto name=upper(trim(rows[0][i]));
        if(!name.empty() && !columns.emplace(name,i).second) throw std::runtime_error(path.string()+": duplicate column "+name);
    }
    for(const auto* name:{"ASSET TYPE","SITE ID","REVIEW STATUS","SOURCE GLOBALID","SOURCE MODEL"})
        if(!columns.count(name)) throw std::runtime_error(path.string()+": missing column "+name);
    std::map<std::string,std::vector<uint32_t>> ids;
    for(uint32_t id=0;id<spf.max_id();++id) {
        const auto line=spf.line(id); const auto open=line.find('(');
        if(open==std::string_view::npos || open+25>=line.size() || line[open+1]!='\'' || line[open+24]!='\'') continue;
        ids[std::string(line.substr(open+2,22))].push_back(id);
    }
    std::set<std::string> seen;
    size_t unmatched=0;
    for(size_t i=1;i<rows.size();++i) {
        const auto get=[&](const char* key) { const auto n=columns.at(key);return n<rows[i].size()?trim(rows[i][n]):std::string(); };
        if(upper(get("REVIEW STATUS"))!="APPROVED") continue;
        const auto site=get("SITE ID"), type=get("ASSET TYPE"), guid=get("SOURCE GLOBALID"), model=get("SOURCE MODEL");
        const auto transparency_text=columns.count("TRANSPARENCY") ? get("TRANSPARENCY") : std::string();
        if(site.empty() && type.empty()) continue;
        const std::string context=path.string()+": row "+std::to_string(i+1)+": ";
        if(upper(model)!=upper(input.filename().string())) throw std::runtime_error(context+"source model does not match "+input.filename().string());
        if(guid.empty() || !seen.insert(guid).second) throw std::runtime_error(context+"missing or duplicate source GlobalId");
        if(!site.empty() && !is_one_site(site)) throw std::runtime_error(context+"invalid Site ID "+site);
        if(!type.empty() && (upper(type)!=type || !(std::all_of(type.begin(),type.end(),[](unsigned char c){return std::isalpha(c);}) || type=="RC1" || type=="RC2" || type=="RC3"))) throw std::runtime_error(context+"invalid Asset Type "+type);
        if(!site.empty() && !type.empty() && asset_type_of(site)!=type) throw std::runtime_error(context+"Asset Type does not match Site ID");
        double transparency=-1;
        if(!transparency_text.empty()) {
            if(site.empty()) throw std::runtime_error(context+"Transparency needs a Site ID");
            try { size_t used=0; transparency=std::stod(transparency_text,&used); if(used!=transparency_text.size()) throw std::invalid_argument(""); }
            catch(const std::exception&) { throw std::runtime_error(context+"invalid Transparency "+transparency_text); }
            if(transparency<0 || transparency>1) throw std::runtime_error(context+"Transparency is from 0 to 1");
        }
        const auto found=ids.find(guid);
        if(found==ids.end()) { ++unmatched; std::cerr<<"  warning: correction GlobalId "<<guid<<" is not in "<<input.filename().string()<<"; review row "<<i+1<<"\n"; continue; }
        if(found->second.size()!=1) throw std::runtime_error(context+"source GlobalId is not unique in the model");
        result.emplace(found->second.front(),ElementCorrection{site,type,transparency});
    }
    std::cout<<"  "<<result.size()<<" approved element corrections applied from "<<path.string();
    if(unmatched) std::cout<<"; "<<unmatched<<" missing GlobalIds need review";
    std::cout<<"\n";
    return result;
}
}
