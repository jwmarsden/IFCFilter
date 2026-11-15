#include "site_lookup.h"
#include "site.h"
#include "xlsx.h"
#include "uniclass.h"
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <vector>
namespace ifcfilter {
namespace {
// RFC 4180 CSV: quoted fields may hold commas, newlines and doubled quotes
std::vector<std::vector<std::string>> read_csv(std::istream& in) {
    std::vector<std::vector<std::string>> rows(1, std::vector<std::string>(1));
    bool quoted=false;
    for(char c; in.get(c);) {
        if(c=='"') {
            if(quoted && in.peek()=='"') { rows.back().back()+='"'; in.get(); }
            else quoted=!quoted;
        } else if(!quoted && c==',') rows.back().emplace_back();
        else if(!quoted && c=='\n') rows.emplace_back(1);
        else if(c!='\r' || quoted) rows.back().back()+=c;
    }
    if(quoted) throw std::runtime_error("unterminated quoted field in site lookup");
    if(rows.back().size()==1 && rows.back()[0].empty()) rows.pop_back();
    return rows;
}
}
// The first row is the column names; rows without a site ID are skipped
SiteLookup load_site_lookup(const std::string& path) {
    SiteLookup result; if(path.empty()) return result;
    std::ifstream in(path,std::ios::binary); if(!in) throw std::runtime_error(path+": cannot read site lookup");
    auto rows=path.size()>=5 && path.substr(path.size()-5)==".xlsx" ? read_xlsx(path) : read_csv(in); if(rows.empty()) throw std::runtime_error(path+": site lookup is empty");
    if(rows[0][0].size()>=3 && rows[0][0].compare(0,3,"\xef\xbb\xbf")==0) rows[0][0].erase(0,3);
    std::map<std::string,size_t> columns;
    for(size_t i=0;i<rows[0].size();++i) if(!columns.emplace(rows[0][i],i).second) throw std::runtime_error(path+": duplicate column "+rows[0][i]);
    for(const char* name:{"SiteID","DesignTag","Asset Type","Location","Zone","Co","En","SL","EF","Ss","Pr"}) if(!columns.count(name)) throw std::runtime_error(path+": missing column "+name);
    for(size_t i=1;i<rows.size();++i) {
        auto& row=rows[i]; if(row.size()<rows[0].size()) row.resize(rows[0].size()); if(row.size()!=rows[0].size()) throw std::runtime_error(path+": wrong column count on row "+std::to_string(i+1));
        const auto get=[&](const char* key)->const std::string& {return row[columns.at(key)];};
        if(get("SiteID").empty()) continue;
        if(!is_one_site(get("SiteID"))) throw std::runtime_error(path+": invalid SiteID on row "+std::to_string(i+1)+": "+get("SiteID"));
        SiteCorrection record; record.design=get("DesignTag");record.type=get("Asset Type");record.location=get("Location");record.zone=get("Zone");
        if(columns.count("Identifier")) record.identifier=get("Identifier");
        if(columns.count("Description")) record.description=get("Description");
        // Additional columns can supply same-named IFC_Filter fields in the schema workbook.
        for(const auto& [name, column] : columns) if(!row[column].empty()) record.extra[name]=row[column];
        // Each Uniclass code must start with its table's prefix, as Pr_ for products, and be an active code
        const char* codes[]={"Co","En","SL","EF","Ss","Pr"};
                for(size_t j=0;j<6;++j) {
            record.codes[j]=get(codes[j]);
            if(record.codes[j].empty()) continue;
            std::string prefix=record.codes[j].substr(0,record.codes[j].find('_'));
            std::string expected=codes[j];
            for(char& c:prefix)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            for(char& c:expected)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if(prefix!=expected || uniclass_title(record.codes[j]).empty())
                throw std::runtime_error(path+": invalid Uniclass "+codes[j]+" code on row "+std::to_string(i+1)+": "+record.codes[j]);
        }
        if(!result.emplace(get("SiteID"),std::move(record)).second) throw std::runtime_error(path+": duplicate SiteID "+get("SiteID"));
    }
    return result;
}
}



