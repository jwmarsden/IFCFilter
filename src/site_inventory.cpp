#include "site_inventory.h"
#include "site.h"
#include "spf.h"
#include "uniclass.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
namespace ifcfilter {
namespace fs=std::filesystem;
namespace {
// What's found for a property set, product or site: its site IDs, and per table (Co, En, SL, EF, Ss, Pr) the
// active codes and the unknown or inactive ones
struct Values {std::set<std::string> sites;std::array<std::set<std::string>,6> codes, unknown;};
std::string quoted(std::string_view s) {auto a=s.find('\'');auto b=s.rfind('\'');return a==s.npos||b<=a?"":std::string(s.substr(a+1,b-a-1));}
// A property name lower case and without punctuation, so Uniclass_Pr_Code and "Uniclass Pr Code" match
std::string norm(std::string_view s) {std::string r;for(unsigned char c:s)if(std::isalnum(c))r+=char(std::tolower(c));return r;}
std::string csv(const std::string& s) {std::string r="\"";for(char c:s){if(c=='"')r+='"';r+=c;}return r+'"';}
void add(Values& to,const Values& from) {to.sites.insert(from.sites.begin(),from.sites.end());for(size_t i=0;i<6;++i){to.codes[i].insert(from.codes[i].begin(),from.codes[i].end());to.unknown[i].insert(from.unknown[i].begin(),from.unknown[i].end());}}
}
// For each model: what each property set says, then what each product gets through its own sets and its
// type's, then, for a product with exactly one site ID, added to that site. A product with several is
// counted as ambiguous and left out.
int inventory_site_uniclass(const std::vector<std::string>& inputs,const std::string& rules,const std::string& output) {
    load_schema(rules);std::vector<fs::path> files;
    for(const auto& name:inputs) {fs::path path(name);if(fs::is_directory(path)) {for(const auto& e:fs::recursive_directory_iterator(path)) if(e.is_regular_file() && e.path().extension()==".ifc") files.push_back(e.path());} else files.push_back(path);}
    std::sort(files.begin(),files.end());std::map<std::string,Values> sites;std::set<std::string> unrecognised;size_t i=0,ambiguous=0;
    const std::array<std::string,6> names={"co","en","sl","ef","ss","pr"};
    for(const auto& path:files) {
        SpfFile f(path.string());std::unordered_map<uint32_t,Values> sets;
        for(uint32_t id=0;id<f.max_id();++id) if(f.type(id)=="IFCPROPERTYSET") {
            auto a=attributes_of(f.line(id));if(a.size()!=5)continue;const auto setname=quoted(a[2]);Values v;
            for(auto p:refs_in(a[4])) {
                if(f.type(p)!="IFCPROPERTYSINGLEVALUE")continue;auto b=attributes_of(f.line(p));if(b.size()<3)continue;
                auto pname=quoted(b[0]),value=quoted(b[2]);if(value.empty()||NOT_A_SITE.count(value))continue;
                if(schema().site_sets.count(setname) && pname==schema().site_property && is_one_site(value))v.sites.insert(value);
                auto n=norm(pname);
                for(size_t t=0;t<6;++t) if(n=="uniclass"+names[t]+"code" || n=="uniclass"+names[t]+"value") {
                    if(uniclass_title(value).empty()) {unrecognised.insert(value);v.unknown[t].insert(value);} else v.codes[t].insert(value);
                }
            }
            bool keep=!v.sites.empty();for(auto& x:v.codes)keep=keep||!x.empty();for(auto& x:v.unknown)keep=keep||!x.empty();if(keep)sets.emplace(id,std::move(v));
        }
        std::unordered_map<uint32_t,Values> products;
        for(uint32_t id=0;id<f.max_id();++id) {
            const auto type=f.type(id);if(type!="IFCRELDEFINESBYPROPERTIES" && type!="IFCRELDEFINESBYTYPE")continue;
            auto a=attributes_of(f.line(id));if(a.size()!=6)continue;auto targets=refs_in(a[4]);
            for(auto def:refs_in(a[5])) {
                std::vector<uint32_t> ids;
                if(type=="IFCRELDEFINESBYPROPERTIES")ids.push_back(def);
                else {auto b=attributes_of(f.line(def));if(b.size()>5)ids=refs_in(b[5]);}
                for(auto sid:ids) {auto at=sets.find(sid);if(at==sets.end())continue;for(auto product:targets)add(products[product],at->second);}
            }
        }
        for(auto& [id,v]:products) {
            if(v.sites.size()!=1){if(v.sites.size()>1)++ambiguous;continue;}
            add(sites[*v.sites.begin()],v);
        }
        std::cout<<"\rUniclass scan "<<++i<<"/"<<files.size()<<std::flush;
    }
    // A table's code is written only where the site has exactly one; more than one is a conflict, for the review
    std::ofstream out(output,std::ios::binary);if(!out)throw std::runtime_error(output+": cannot write");out<<"SiteID,Co,En,SL,EF,Ss,Pr\n";
    size_t populated=0,conflicts=0;
    for(auto& [site,v]:sites) {out<<csv(site);for(auto& codes:v.codes){out<<',';if(codes.size()==1){out<<csv(*codes.begin());++populated;}else if(codes.size()>1)++conflicts;}out<<'\n';}
    if(!out)throw std::runtime_error(output+": write failed");
    std::ofstream review(output+".review.csv",std::ios::binary);
    if(!review) throw std::runtime_error(output+".review.csv: cannot write");
    review<<"SiteID,Table,Issue,Source values\n";
    for(const auto& [site,v]:sites) for(size_t t=0;t<names.size();++t) {
        if(v.codes[t].size()>1) {
            std::string list;for(const auto& code:v.codes[t]) list+=(list.empty()?"":"; ")+code;
            review<<csv(site)<<','<<csv(names[t])<<",Conflicting codes,"<<csv(list)<<'\n';
        }
        if(!v.unknown[t].empty()) {
            std::string list;for(const auto& code:v.unknown[t]) list+=(list.empty()?"":"; ")+code;
            review<<csv(site)<<','<<csv(names[t])<<",Unknown or inactive code,"<<csv(list)<<'\n';
        }
    }
    if(!review) throw std::runtime_error(output+".review.csv: write failed");
    std::cout<<"\n"<<sites.size()<<" sites, "<<populated<<" unambiguous table codes, "<<conflicts<<" conflicting site/table combinations, "<<ambiguous<<" ambiguous source elements, "<<unrecognised.size()<<" unknown codes\n";
    return 0;
}
}


