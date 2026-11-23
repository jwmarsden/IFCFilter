#include "element_corrections.h"
#include "site.h"
#include "spf.h"
#include "xlsx.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
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
namespace {
fs::path data_dir(const std::string& schema_path) {
    if(const char* configured=std::getenv("IFC_DATA_DIR");configured && *configured) return configured;
    return fs::path(schema_path).parent_path();
}
// Each GlobalId in the model, and the lines that have it
std::map<std::string,std::vector<uint32_t>> ids_by_guid(const SpfFile& spf) {
    std::map<std::string,std::vector<uint32_t>> ids;
    for(uint32_t id=0;id<spf.max_id();++id) {
        const auto line=spf.line(id); const auto open=line.find('(');
        if(open==std::string_view::npos || open+25>=line.size() || line[open+1]!='\'' || line[open+24]!='\'') continue;
        ids[std::string(line.substr(open+2,22))].push_back(id);
    }
    return ids;
}
// RFC 4180 CSV: quoted fields may hold commas, newlines and doubled quotes
std::vector<std::vector<std::string>> read_csv(std::istream& in) {
    std::vector<std::vector<std::string>> rows(1,std::vector<std::string>(1));
    bool quoted=false;
    for(char c;in.get(c);) {
        if(c=='"') { if(quoted && in.peek()=='"') { rows.back().back()+='"'; in.get(); } else quoted=!quoted; }
        else if(!quoted && c==',') rows.back().emplace_back();
        else if(!quoted && c=='\n') rows.emplace_back(1);
        else if(c!='\r' || quoted) rows.back().back()+=c;
    }
    if(rows.back().size()==1 && rows.back()[0].empty()) rows.pop_back();
    return rows;
}
}
fs::path element_correction_path(const fs::path& input, const std::string& schema_path) {
    return data_dir(schema_path) / "site-id-corrections" / fs::absolute(input).parent_path().filename() / (input.stem().string()+".xlsx");
}
fs::path location_correction_path(const fs::path& input, const std::string& schema_path) {
    return data_dir(schema_path) / "location-corrections" / fs::absolute(input).parent_path().filename() / (input.stem().string()+".csv");
}
fs::path site_correction_csv_path(const fs::path& input, const std::string& schema_path) {
    return data_dir(schema_path) / "site-id-corrections" / fs::absolute(input).parent_path().filename() / (input.stem().string()+".csv");
}
namespace {
// An edit log of GlobalId and value, as IFCViewer writes them: the last Approved row for each element in this
// model, by step ID, with its value (empty: taken away). what names the kind of correction in messages.
std::map<uint32_t,std::string> read_edit_log(const fs::path& path, const fs::path& input, const SpfFile& spf, const char* column, const char* what, size_t& other_model) {
    std::map<uint32_t,std::string> result;
    std::ifstream in(path,std::ios::binary);
    if(!in) throw std::runtime_error(path.string()+": can't be read");
    auto rows=read_csv(in);
    if(rows.empty()) return result;
    if(rows[0][0].compare(0,3,"\xef\xbb\xbf")==0) rows[0][0].erase(0,3);
    std::map<std::string,size_t> columns;
    for(size_t i=0;i<rows[0].size();++i) columns.emplace(upper(trim(rows[0][i])),i);
    for(const auto* name:{"GLOBALID",column})
        if(!columns.count(name)) throw std::runtime_error(path.string()+": missing column "+name);
    // Rows are added as edits are made, so the last approved one for each element is what it is now
    std::map<std::string,std::pair<std::string,size_t>> latest;  // GlobalId, its value and row
    for(size_t i=1;i<rows.size();++i) {
        const auto get=[&](const char* key) { const auto at=columns.find(key); return at!=columns.end() && at->second<rows[i].size() ? trim(rows[i][at->second]) : std::string(); };
        const auto guid=get("GLOBALID");
        if(guid.empty()) continue;
        const auto status=upper(get("STATUS"));
        if(!status.empty() && status!="APPROVED") continue;
        const auto model=get("MODEL");
        if(!model.empty() && upper(model)!=upper(input.filename().string())) { ++other_model; continue; }
        latest[guid]={get(column),i+1};
    }
    const auto ids=ids_by_guid(spf);
    for(const auto& [guid,value] : latest) {
        const auto found=ids.find(guid);
        if(found==ids.end() || found->second.size()!=1) {
            std::cerr<<"  warning: "<<what<<" correction GlobalId "<<guid<<" is not "<<(found==ids.end()?"in ":"unique in ")<<input.filename().string()<<"; review row "<<value.second<<"\n";
            continue;
        }
        result[found->second.front()]=value.first;
    }
    if(latest.size()>result.size()) std::cerr<<"  "<<latest.size()-result.size()<<" "<<what<<" corrections in "<<path.string()<<" need review\n";
    return result;
}
}
void load_location_corrections(const fs::path& path, const fs::path& input, const SpfFile& spf, ElementCorrections& corrections) {
    if(!fs::is_regular_file(path)) return;
    size_t other_model=0, applied=0;
    for(const auto& [id,location] : read_edit_log(path,input,spf,"LOCATION","location",other_model)) {
        if(location.empty()) continue;  // taken away
        corrections[id].location=location;
        ++applied;
    }
    std::cout<<"  "<<applied<<" location corrections applied from "<<path.string();
    if(other_model) std::cout<<"; "<<other_model<<" rows for other models left out";
    std::cout<<"\n";
}
std::set<std::string> corrected_sites(const fs::path& input, const std::string& schema_path) {
    std::set<std::string> sites;
    const auto workbook = element_correction_path(input, schema_path);
    if (fs::is_regular_file(workbook)) {
        const auto rows = read_xlsx(workbook.string());
        std::map<std::string,size_t> columns;
        if (!rows.empty()) for (size_t i = 0; i < rows[0].size(); ++i) columns.emplace(upper(trim(rows[0][i])), i);
        if (columns.count("SITE ID") && columns.count("REVIEW STATUS")) {
            for (size_t i = 1; i < rows.size(); ++i) {
                const auto get = [&](const char* key) { const auto n = columns.at(key); return n < rows[i].size() ? trim(rows[i][n]) : std::string(); };
                if (upper(get("REVIEW STATUS")) == "APPROVED" && is_one_site(get("SITE ID"))) sites.insert(get("SITE ID"));
            }
        }
    }
    const auto csv = site_correction_csv_path(input, schema_path);
    std::ifstream in(csv, std::ios::binary);
    if (!in) return sites;
    auto rows = read_csv(in);
    if (rows.empty()) return sites;
    if (rows[0][0].compare(0, 3, "\xef\xbb\xbf") == 0) rows[0][0].erase(0, 3);
    std::map<std::string,size_t> columns;
    for (size_t i = 0; i < rows[0].size(); ++i) columns.emplace(upper(trim(rows[0][i])), i);
    if (!columns.count("GLOBALID") || !columns.count("SITE ID")) return sites;
    std::map<std::string,std::string> latest;
    for (size_t i = 1; i < rows.size(); ++i) {
        const auto get = [&](const char* key) { const auto at = columns.find(key); return at != columns.end() && at->second < rows[i].size() ? trim(rows[i][at->second]) : std::string(); };
        const auto status = upper(get("STATUS")), model = get("MODEL");
        if ((!status.empty() && status != "APPROVED") || (!model.empty() && upper(model) != upper(input.filename().string()))) continue;
        if (!get("GLOBALID").empty()) latest[get("GLOBALID")] = get("SITE ID");
    }
    for (const auto& [guid, site] : latest) if (is_one_site(site)) sites.insert(site);
    return sites;
}
void load_site_corrections_csv(const fs::path& path, const fs::path& input, const SpfFile& spf, ElementCorrections& corrections) {
    if(!fs::is_regular_file(path)) return;
    size_t other_model=0, applied=0, removed=0;
    for(const auto& [id,site] : read_edit_log(path,input,spf,"SITE ID","site ID",other_model)) {
        if(site.empty()) {
            // Taken away: the workbook's site for it too, leaving any location
            if(const auto found=corrections.find(id);found!=corrections.end()) {
                found->second.site.clear(); found->second.type.clear(); found->second.transparency=-1;
                ++removed;
            }
            continue;
        }
        if(!is_one_site(site)) { std::cerr<<"  warning: "<<path.string()<<": invalid Site ID "<<site<<" left out\n"; continue; }
        auto& correction=corrections[id];
        if(correction.site!=site) correction.transparency=-1;  // the workbook's transparency was for its site
        correction.site=site;
        correction.type=asset_type_of(site);
        ++applied;
    }
    std::cout<<"  "<<applied<<" site ID corrections applied from "<<path.string();
    if(removed) std::cout<<"; "<<removed<<" taken away";
    if(other_model) std::cout<<"; "<<other_model<<" rows for other models left out";
    std::cout<<"\n";
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
    const auto ids=ids_by_guid(spf);
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
