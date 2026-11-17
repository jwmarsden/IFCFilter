#include "location_reference.h"
#include "xlsx.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>
namespace ifcfilter {
// Reads the CSV, then keeps each field of a location only where all its rows agree
LocationReference load_location_reference(const std::string& path) {
    std::string extension=std::filesystem::path(path).extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    std::vector<std::vector<std::string>> rows;
    if(extension==".xlsx") rows=read_xlsx(path);
    else {
        std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error(path+": cannot read location reference");
        rows.emplace_back(1);bool quoted=false;
        for(char c;in.get(c);) {
            if(c=='"') {if(quoted && in.peek()=='"'){rows.back().back()+='"';in.get();}else quoted=!quoted;}
            else if(!quoted && c==',')rows.back().emplace_back();
            else if(!quoted && c=='\n')rows.emplace_back(1);
            else if(c!='\r' || quoted)rows.back().back()+=c;
        }
        if(quoted)throw std::runtime_error(path+": unterminated CSV quote");
    }
    if(rows.empty() || rows[0].empty())throw std::runtime_error(path+": empty location reference");
    if(rows[0][0].compare(0,3,"\xef\xbb\xbf")==0)rows[0][0].erase(0,3);
    // The code column may carry an owner's prefix, as XYZ_LocationHierarchyCode
    const std::array<const char*,8> columns = {"LocationHierarchyCode", "LocationHierarchyDescription", "Uniclass_CoCode", "Uniclass_CoTitle", "Uniclass_EnCode", "Uniclass_EnTitle", "Uniclass_SLCode", "Uniclass_SLTitle"};
    const std::string code_suffix = std::string("_")+columns[0];
    std::array<size_t,8> indices; indices.fill(rows[0].size());
    for(size_t i=0;i<rows[0].size();++i) for(size_t j=0;j<columns.size();++j) if(rows[0][i]==columns[j])indices[j]=i;
    if(indices[0]==rows[0].size())
        for(size_t i=0;i<rows[0].size();++i)
            if(rows[0][i].size()>code_suffix.size() && rows[0][i].compare(rows[0][i].size()-code_suffix.size(),code_suffix.size(),code_suffix)==0) {indices[0]=i;break;}
    if(indices[0]==rows[0].size() || indices[1]==rows[0].size())throw std::runtime_error(path+": missing location reference columns");
    // Every value seen for each location, field by field: description, then the six classification fields
    std::map<std::string,std::array<std::set<std::string>,7>> values;
    for(size_t i=1;i<rows.size();++i) {
        auto row=rows[i];if(row.empty() || (row.size()==1 && row[0].empty()))continue;
        if(row.size()>rows[0].size() || (extension!=".xlsx" && row.size()!=rows[0].size()))throw std::runtime_error(path+": invalid location reference row "+std::to_string(i+1));
        row.resize(rows[0].size());
        if(row[indices[0]].empty())continue;
        for(size_t j=1;j<indices.size();++j) if(indices[j]<row.size() && !row[indices[j]].empty()) values[row[indices[0]]][j-1].insert(row[indices[j]]);
    }
    LocationReference result;
    for(const auto& [key,fields]:values) {
        auto& entry=result[key];
        if(fields[0].size()==1)entry.description=*fields[0].begin();
        for(size_t i=0;i<6;++i)if(fields[i+1].size()==1)entry.classification[i]=*fields[i+1].begin();
    }
    return result;
}
}
