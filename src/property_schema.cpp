#include "property_schema.h"
#include "spf.h"
#include "system.h"
#include "upgrade.h"
#include "xlsx.h"
#include <ifcparse/schema.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifcfilter {
namespace fs = std::filesystem;
namespace {
using Schema = PropertySchema;
std::string quoted(std::string_view s) {
    auto a=s.find('\''); auto b=s.rfind('\'');
    return a==s.npos || b<=a ? "" : std::string(s.substr(a+1,b-a-1));
}
std::string ref(uint32_t id) { return "#" + std::to_string(id); }
std::string props(const SpfFile& f, uint32_t id, Schema& schema) {
    if (f.type(id)!="IFCPROPERTYSET") return {};
    auto a=attributes_of(f.line(id)); if (a.size()!=5) return {};
    auto name=quoted(a[2]); if (name.empty()) return {};
    auto& names=schema[name];
    for (auto p:refs_in(a[4])) {
        auto v=attributes_of(f.line(p));
        if (!v.empty()) { auto n=quoted(v[0]); if (!n.empty()) names.insert(n); }
    }
    return name;
}
std::unordered_set<uint32_t> referenced_sets(const SpfFile& f) {
    std::unordered_set<uint32_t> sets;
    for(uint32_t id=0;id<f.max_id();++id) {
        auto type=f.type(id);
        if(type!="IFCRELDEFINESBYPROPERTIES" && type!="IFCRELDEFINESBYTYPE") continue;
        auto a=attributes_of(f.line(id)); if(a.size()!=6) continue;
        for(auto d:refs_in(a[5])) {
            if(type=="IFCRELDEFINESBYPROPERTIES") sets.insert(d);
            else { auto ta=attributes_of(f.line(d)); if(ta.size()>5) for(auto set:refs_in(ta[5])) sets.insert(set); }
        }
    }
    return sets;
}
std::vector<fs::path> files_in(const fs::path& dir) {
    std::vector<fs::path> files;
    if (fs::exists(dir)) for (const auto& e:fs::recursive_directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension()==".ifc") files.push_back(e.path());
    std::sort(files.begin(),files.end()); return files;
}
void normalize(const fs::path& path, const Schema& schema) {
    const std::string temp=path.string()+".properties";
    bool changed=false;
    {
        SpfFile f(path.string());
        auto schema_name=schema_in_header(path);
        auto* definition=ifcopenshell::schema_by_name(schema_name.rfind("IFC2X3",0)==0 ? "IFC2X3" : schema_name.rfind("IFC4X3",0)==0 ? "IFC4X3_ADD2" : "IFC4");
        auto* product=definition->declaration_by_name("IfcProduct")->as_entity();
        std::unordered_map<std::string,bool> kinds;
        std::vector<uint32_t> products;
        std::string owner="$";
        for (uint32_t id=0;id<f.max_id();++id) {
            auto type=f.type(id); if(type.empty()) continue;
            if(type=="IFCOWNERHISTORY" && owner=="$") owner=ref(id);
            std::string key(type); auto it=kinds.find(key);
            if(it==kinds.end()) {
                bool yes=false; try { auto* ent=definition->declaration_by_name(key)->as_entity(); yes=ent && ent->is(*product); } catch(const std::exception&) {}
                it=kinds.emplace(key,yes).first;
            }
            if(it->second) products.push_back(id);
        }
        std::unordered_map<uint32_t,std::string> set_names;
        std::unordered_map<uint32_t,std::unordered_set<std::string>> present;
        std::unordered_map<uint32_t,std::vector<uint32_t>> type_sets;
        for(auto id:referenced_sets(f)) { Schema unused; auto n=props(f,id,unused); if(!n.empty()) set_names[id]=n; }
        for(uint32_t id=0;id<f.max_id();++id) {
            auto type=f.type(id); if(type!="IFCRELDEFINESBYPROPERTIES" && type!="IFCRELDEFINESBYTYPE") continue;
            auto a=attributes_of(f.line(id)); if(a.size()!=6) continue;
            auto targets=refs_in(a[4]); auto defs=refs_in(a[5]);
            if(type=="IFCRELDEFINESBYPROPERTIES") {
                for(auto d:defs) if(set_names.count(d)) for(auto p:targets) present[p].insert(set_names[d]);
            } else for(auto d:defs) {
                auto ta=attributes_of(f.line(d)); if(ta.size()<=5) continue;
                for(auto set:refs_in(ta[5])) if(set_names.count(set)) for(auto p:targets) present[p].insert(set_names[set]);
            }
        }
        uint32_t next=f.max_id();
        std::vector<std::string> added;
        std::map<std::string,uint32_t> blanks;
        auto blank=[&](const std::string& name) {
            auto at=blanks.find(name); if(at!=blanks.end()) return at->second;
            uint32_t id=next++; blanks[name]=id;
            added.push_back(ref(id)+"=IFCPROPERTYSINGLEVALUE('"+name+"',$,$,$);"); return id;
        };
        std::unordered_map<uint32_t,std::string> rewritten;
        for(const auto& [id,name]:set_names) {
            auto a=attributes_of(f.line(id)); std::set<std::string> have;
            for(auto p:refs_in(a[4])) {auto v=attributes_of(f.line(p)); if(!v.empty()) have.insert(quoted(v[0]));}
            std::string extra;
            for(const auto& n:schema.at(name)) if(!have.count(n)) extra+=","+ref(blank(n));
            if(!extra.empty()) {
                auto line=f.line(id); auto pos=static_cast<size_t>(a[4].data()-line.data())+a[4].size()-1;
                rewritten[id]=std::string(line.substr(0,pos))+extra+std::string(line.substr(pos));
            }
        }
        const std::string key=path.filename().string();
        for(const auto& [name,names]:schema) {
            std::vector<uint32_t> missing;
            for(auto p:products) if(!present[p].count(name)) missing.push_back(p);
            if(missing.empty() || names.empty()) continue;
            std::string list; for(const auto& n:names) list+=(list.empty()?"":",")+ref(blank(n));
            uint32_t set=next++;
            added.push_back(ref(set)+"=IFCPROPERTYSET('"+derived_guid(key+"/blank/"+name)+"',"+owner+",'"+name+"',$,("+list+"));");
            for(size_t i=0;i<missing.size();i+=4096) {
                std::string ids; for(size_t j=i;j<std::min(i+4096,missing.size());++j) ids+=(ids.empty()?"":",")+ref(missing[j]);
                uint32_t rel=next++;
                added.push_back(ref(rel)+"=IFCRELDEFINESBYPROPERTIES('"+derived_guid(key+"/blank/"+name+"/"+std::to_string(i))+"',"+owner+",$,$,("+ids+"),"+ref(set)+");");
            }
        }
        changed=!rewritten.empty() || !added.empty();
        if(changed) {
            std::ofstream out(temp,std::ios::binary); if(!out) throw std::runtime_error(temp+": cannot write");
            out<<f.header();
            for(uint32_t id=0;id<f.max_id();++id) {auto line=f.line(id); if(line.empty()) continue; auto at=rewritten.find(id); out<<(at==rewritten.end()?std::string(line):at->second)<<'\n';}
            for(const auto& line:added) out<<line<<'\n';
            out<<"ENDSEC;\nEND-ISO-10303-21;\n"; if(!out) throw std::runtime_error(temp+": write failed");
        }
    }
    if(changed) replace_file(temp,path.string());
}
std::string key_of(std::string_view value) {
    std::string out;
    for(unsigned char c:value) if(std::isalnum(c)) out+=static_cast<char>(std::tolower(c));
    return out;
}
std::string step_name(const std::string& value) {
    std::string out="'";
    for(char c:value) {out+=c;if(c=='\'')out+='\'';}
    return out+"'";
}
std::string set_name(const std::string& source,const Schema& schema) {
    auto key=key_of(source);
    for(const auto& [name,_]:schema) if(key_of(name)==key) return name;
    return {};
}
std::string property_name(const std::string& set,const std::string& source,const Schema& schema) {
    auto key=key_of(source);
    if(key_of(set)=="ifcfilter" && key=="designtag") key="designid";
    if(key_of(set)=="classification" && key.size()>5 && key.substr(key.size()-5)=="value") key.replace(key.size()-5,5,"code");
    for(const auto& name:schema.at(set)) if(key_of(name)==key) return name;
    return {};
}
std::string schema_header(std::string header) {
    if(header.find("IFCFILTER_PROPERTY_SCHEMA")!=std::string::npos) return header;
    auto start=header.find("FILE_DESCRIPTION");
    auto list=start==std::string::npos?start:header.find("((",start);
    if(list==std::string::npos) throw std::runtime_error("IFC header has no FILE_DESCRIPTION list");
    size_t close=std::string::npos; bool quoted=false;
    for(size_t i=list+2;i<header.size();++i) {
        if(header[i]=='\'') {
            if(quoted && i+1<header.size() && header[i+1]=='\'') {++i;continue;}
            quoted=!quoted;
        } else if(header[i]==')' && !quoted) {close=i;break;}
    }
    if(close==std::string::npos) throw std::runtime_error("invalid IFC FILE_DESCRIPTION");
    header.insert(close,(close==list+2 ? "" : ",")+std::string("'IFCFILTER_PROPERTY_SCHEMA'"));
    return header;
}
void normalize_explicit(const fs::path& path,const Schema& schema) {
    const std::string temp=path.string()+".properties";
    bool changed=false;
    {
        SpfFile f(path.string());
        auto schema_name=schema_in_header(path);
        auto* definition=ifcopenshell::schema_by_name(schema_name.rfind("IFC2X3",0)==0 ? "IFC2X3" : schema_name.rfind("IFC4X3",0)==0 ? "IFC4X3_ADD2" : "IFC4");
        auto* product=definition->declaration_by_name("IfcProduct")->as_entity();
        std::unordered_map<std::string,bool> kinds;std::vector<uint32_t> products;std::unordered_set<uint32_t> product_ids;
        std::string owner="$";
        for(uint32_t id=0;id<f.max_id();++id) {
            auto type=f.type(id);if(type.empty())continue;
            if(type=="IFCOWNERHISTORY" && owner=="$") owner=ref(id);
            std::string name(type);auto at=kinds.find(name);
            if(at==kinds.end()) {bool yes=false;try {auto* entity=definition->declaration_by_name(name)->as_entity();yes=entity && entity->is(*product);}catch(const std::exception&){}at=kinds.emplace(name,yes).first;}
            if(at->second){products.push_back(id);product_ids.insert(id);}
        }
        auto referenced=referenced_sets(f);
        std::unordered_map<uint32_t,std::string> allowed;
        for(auto id:referenced) {
            if(f.type(id)!="IFCPROPERTYSET")continue;
            auto a=attributes_of(f.line(id));if(a.size()!=5)continue;
            auto name=set_name(quoted(a[2]),schema);if(!name.empty())allowed[id]=name;
        }
        std::unordered_map<uint32_t,std::unordered_set<std::string>> present;
        std::unordered_map<uint32_t,std::string> rewrites;
        std::unordered_set<uint32_t> dropped;
        for(uint32_t id=0;id<f.max_id();++id) {
            auto type=f.type(id);if(type!="IFCRELDEFINESBYPROPERTIES" && type!="IFCRELDEFINESBYTYPE")continue;
            auto a=attributes_of(f.line(id));if(a.size()!=6)continue;
            auto targets=refs_in(a[4]);auto definitions=refs_in(a[5]);
            if(type=="IFCRELDEFINESBYPROPERTIES") {
                if(definitions.size()!=1)continue;
                auto set=definitions[0];
                if(allowed.count(set)) {for(auto p:targets)if(product_ids.count(p))present[p].insert(allowed[set]);}
                else if(f.type(set)=="IFCPROPERTYSET" || f.type(set)=="IFCELEMENTQUANTITY") {
                    std::string changed;
                    if(rewrite(f.line(id),[&](uint32_t refid){return product_ids.count(refid)!=0;},WhenEmpty::Drop,changed)) rewrites[id]=changed;
                    else dropped.insert(id);
                }
            } else for(auto def:definitions) {
                auto line=f.line(def);auto ta=attributes_of(line);if(ta.size()<=5)continue;
                auto ids=refs_in(ta[5]);std::string kept;
                for(auto set:ids) if(allowed.count(set)) {kept+=(kept.empty()?"":",")+ref(set);for(auto p:targets)if(product_ids.count(p))present[p].insert(allowed[set]);}
                if(ids.size()!=refs_in(kept).size()) {
                    auto pos=static_cast<size_t>(ta[5].data()-line.data());
                    rewrites[def]=std::string(line.substr(0,pos))+(kept.empty()?"$":"("+kept+")")+std::string(line.substr(pos+ta[5].size()));
                }
            }
        }
        uint32_t next=f.max_id();std::vector<std::string> added;std::map<std::string,uint32_t> blanks;
        auto blank=[&](const std::string& name) {auto it=blanks.find(name);if(it!=blanks.end())return it->second;auto id=next++;blanks[name]=id;added.push_back(ref(id)+"=IFCPROPERTYSINGLEVALUE("+step_name(name)+",$,$,$);");return id;};
        std::map<std::pair<uint32_t,std::string>,uint32_t> renamed;
        for(const auto& [id,name]:allowed) {
            auto line=f.line(id);auto a=attributes_of(line);std::map<std::string,uint32_t> kept;
            for(auto property:refs_in(a[4])) {
                auto pline=f.line(property);auto pa=attributes_of(pline);if(pa.empty())continue;
                auto pname=property_name(name,quoted(pa[0]),schema);if(pname.empty() || kept.count(pname))continue;
                uint32_t target=property;
                if(quoted(pa[0])!=pname) {
                    auto key=std::make_pair(property,pname);auto it=renamed.find(key);
                    if(it==renamed.end()) {
                        auto equals=pline.find('=');auto pos=static_cast<size_t>(pa[0].data()-pline.data());
                        if(equals==std::string_view::npos || pos<=equals) throw std::runtime_error("invalid IFC property");
                        target=next++;added.push_back(ref(target)+"="+std::string(pline.substr(equals+1,pos-equals-1))+step_name(pname)+std::string(pline.substr(pos+pa[0].size())));
                        renamed.emplace(key,target);
                    } else target=it->second;
                }
                kept[pname]=target;
            }
            std::string list;for(const auto& pname:schema.at(name)) {auto it=kept.find(pname);list+=(list.empty()?"":",")+ref(it==kept.end()?blank(pname):it->second);}
            auto namepos=static_cast<size_t>(a[2].data()-line.data()), listpos=static_cast<size_t>(a[4].data()-line.data());
            rewrites[id]=std::string(line.substr(0,namepos))+step_name(name)+std::string(line.substr(namepos+a[2].size(),listpos-namepos-a[2].size()))+"("+list+")"+std::string(line.substr(listpos+a[4].size()));
        }
        const std::string filekey=path.filename().string();
        for(const auto& [name,names]:schema) {
            std::vector<uint32_t> missing;for(auto p:products)if(!present[p].count(name))missing.push_back(p);
            if(missing.empty())continue;
            std::string list;for(const auto& n:names)list+=(list.empty()?"":",")+ref(blank(n));
            auto set=next++;added.push_back(ref(set)+"=IFCPROPERTYSET('"+derived_guid(filekey+"/schema/"+name+"/"+std::to_string(set))+"',"+owner+","+step_name(name)+",$,("+list+"));");
            for(size_t i=0;i<missing.size();i+=4096) {
                std::string ids;for(size_t j=i;j<std::min(i+4096,missing.size());++j)ids+=(ids.empty()?"":",")+ref(missing[j]);
                auto rel=next++;added.push_back(ref(rel)+"=IFCRELDEFINESBYPROPERTIES('"+derived_guid(filekey+"/schema/"+name+"/"+std::to_string(rel))+"',"+owner+",$,$,("+ids+"),"+ref(set)+");");
            }
        }
        // Remove disconnected property definitions as well as their relationships. Preserve every
        // definition referenced by another IFC object, including mandatory and shared definitions.
        std::vector<size_t> incoming(next, 0);
        auto count_refs = [&](std::string_view line) {
            for_each_ref(line, [&](uint32_t id) { if(id<incoming.size()) ++incoming[id]; });
        };
        for(uint32_t id=0;id<f.max_id();++id) {
            if(dropped.count(id)) continue;
            const auto at=rewrites.find(id);
            count_refs(at==rewrites.end()?f.line(id):std::string_view(at->second));
        }
        for(const auto& line:added) count_refs(line);
        const auto property_definition = [&](uint32_t id) {
            const auto type=f.type(id);
            return type=="IFCPROPERTYSET" || type=="IFCELEMENTQUANTITY" || type.rfind("IFCPROPERTY",0)==0 || type.rfind("IFCQUANTITY",0)==0;
        };
        std::vector<uint32_t> pending;
        for(uint32_t id=0;id<f.max_id();++id) if(!dropped.count(id) && property_definition(id) && !incoming[id]) pending.push_back(id);
        while(!pending.empty()) {
            const auto id=pending.back();pending.pop_back();
            if(dropped.count(id) || incoming[id]) continue;
            dropped.insert(id);
            const auto at=rewrites.find(id);
            const auto line=at==rewrites.end()?f.line(id):std::string_view(at->second);
            for_each_ref(line,[&](uint32_t child) {
                if(child>=incoming.size() || incoming[child]==0) return;
                if(--incoming[child]==0 && child<f.max_id() && property_definition(child)) pending.push_back(child);
            });
        }
        const std::string header=schema_header(std::string(f.header()));
        changed=!added.empty() || !dropped.empty() || header!=f.header();
        if(!changed) for(const auto& [id,line]:rewrites) if(line!=f.line(id)) {changed=true;break;}
        if(changed) {
            std::ofstream out(temp,std::ios::binary);if(!out)throw std::runtime_error(temp+": cannot write");out<<header;
            for(uint32_t id=0;id<f.max_id();++id) {auto line=f.line(id);if(line.empty()||dropped.count(id))continue;auto it=rewrites.find(id);out<<(it==rewrites.end()?std::string(line):it->second)<<'\n';}
            for(const auto& line:added)out<<line<<'\n';out<<"ENDSEC;\nEND-ISO-10303-21;\n";if(!out)throw std::runtime_error(temp+": write failed");
        }
    }
    if(changed) replace_file(temp,path.string());
}

}
PropertySchema load_property_schema(const std::string& path) {
    PropertySchema result;
    const auto rows=read_xlsx(path);
    for(size_t i=0;i<rows.size();++i) {
        const auto& row=rows[i];
        if(row.empty() || (row.size()==1 && row[0].empty())) continue;
        if(row.size()!=2 || row[0].empty() || row[1].empty()) throw std::runtime_error(path+": expected a property-set and property name on row "+std::to_string(i+1));
        if(row[0]=="Element") continue; // already represented by standard IFC element attributes
        if(!result[row[0]].insert(row[1]).second) throw std::runtime_error(path+": duplicate property on row "+std::to_string(i+1));
    }
    if(result.empty()) throw std::runtime_error(path+": no property sets are defined");
    return result;
}

void normalize_properties(const fs::path& directory, const std::string& schema_path) {
    auto files=files_in(directory); Schema schema;
    if(!schema_path.empty()) {
        schema=load_property_schema(schema_path);
        std::cout<<"Workbook property sets: "<<schema.size()<<" sets across "<<files.size()<<" federation files\n"<<std::flush;
        for(const auto& path:files) normalize_explicit(path,schema);
        return;
    }
    for(const auto& path:files) { SpfFile f(path.string()); for(auto id:referenced_sets(f)) props(f,id,schema); }
    auto& classification=schema["Classification"];
    for(const auto* table:{"Co","En","SL","EF","Ss","Pr"}) {classification.insert(std::string(table)+" Value");classification.insert(std::string(table)+" Description");}
    std::cout<<"Uniform property sets: "<<schema.size()<<" sets across "<<files.size()<<" federation files\n"<<std::flush;
    for(const auto& path:files) normalize(path,schema);
}
}


