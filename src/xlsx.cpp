#include "xlsx.h"
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
namespace ifcfilter {
namespace {
using boost::property_tree::ptree;
uint16_t u16(const std::string& s,size_t p) { if(p+2>s.size()) throw std::runtime_error("truncated XLSX"); return (unsigned char)s[p]|((unsigned char)s[p+1]<<8); }
uint32_t u32(const std::string& s,size_t p) { return uint32_t(u16(s,p))|(uint32_t(u16(s,p+2))<<16); }
struct Entry { uint16_t method,flags; uint32_t size,packed,offset,crc; };
class Zip {
    std::string data; std::unordered_map<std::string,Entry> entries;
  public:
    explicit Zip(const std::string& path) {
        std::ifstream in(path,std::ios::binary); if(!in) throw std::runtime_error(path+": cannot read XLSX");
        data.assign(std::istreambuf_iterator<char>(in),{});
        if(data.size()<22) throw std::runtime_error("invalid XLSX ZIP");
        size_t e=data.size()-22, min=data.size()>65557?data.size()-65557:0;
        while(e>min && u32(data,e)!=0x06054b50) --e;
        if(u32(data,e)!=0x06054b50) throw std::runtime_error("XLSX ZIP directory missing");
        const auto count=u16(data,e+10); size_t at=u32(data,e+16);
        for(unsigned i=0;i<count;++i) {
            if(u32(data,at)!=0x02014b50 || at+46>data.size()) throw std::runtime_error("invalid XLSX ZIP directory");
            const auto n=u16(data,at+28), extra=u16(data,at+30), comment=u16(data,at+32);
            if(at+46+n+extra+comment>data.size()) throw std::runtime_error("truncated XLSX ZIP directory");
            entries.emplace(data.substr(at+46,n),Entry{u16(data,at+10),u16(data,at+8),u32(data,at+24),u32(data,at+20),u32(data,at+42),u32(data,at+16)});
            at+=46+n+extra+comment;
        }
    }
    std::string get(const std::string& name,bool required=true) const {
        const auto it=entries.find(name); if(it==entries.end()) {if(required) throw std::runtime_error("XLSX is missing "+name);return {};}
        auto e=it->second; if((e.flags&1) || e.size>512u*1024u*1024u) throw std::runtime_error("encrypted or oversized XLSX entry");
        if(u32(data,e.offset)!=0x04034b50) throw std::runtime_error("invalid XLSX ZIP entry");
        size_t pos=size_t(e.offset)+30+u16(data,e.offset+26)+u16(data,e.offset+28);
        if(pos+e.packed>data.size()) throw std::runtime_error("truncated XLSX entry");
        std::string out(e.size,'\0');
        if(e.method==0) {if(e.size!=e.packed) throw std::runtime_error("invalid stored XLSX entry");out.assign(data.data()+pos,e.size);}
        else if(e.method==8) {
            z_stream z{}; z.next_in=(Bytef*)(data.data()+pos); z.avail_in=e.packed; z.next_out=(Bytef*)out.data(); z.avail_out=e.size;
            if(inflateInit2(&z,-15)!=Z_OK) throw std::runtime_error("cannot inflate XLSX");
            int result=inflate(&z,Z_FINISH); inflateEnd(&z);
            if(result!=Z_STREAM_END || z.total_out!=e.size) throw std::runtime_error("corrupt XLSX entry");
        } else throw std::runtime_error("unsupported XLSX compression");
        if(crc32(0,(const Bytef*)out.data(),static_cast<uInt>(out.size()))!=e.crc) throw std::runtime_error("XLSX entry CRC mismatch");
        return out;
    }
};
// XML producers may prefix spreadsheet element names (for example x:sheet).
// Keep attribute prefixes such as r:id, which are used to resolve relationships.
std::string strip_tag_prefix(std::string_view text) {
    std::string out;out.reserve(text.size());
    for(size_t i=0;i<text.size();) {
        if(text[i]!='<') {out+=text[i++];continue;}
        out+=text[i++];
        if(i<text.size() && text[i]=='/') out+=text[i++];
        const size_t start=i;
        while(i<text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i]=='_' || text[i]=='-' || text[i]=='.')) ++i;
        if(i<text.size() && text[i]==':' && i>start) ++i;
        else out.append(text.substr(start,i-start));
    }
    return out;
}
ptree xml(const std::string& text) { std::istringstream in(text);ptree tree;boost::property_tree::read_xml(in,tree);return tree; }
void texts(const ptree& node,std::string& out) {
    for(const auto& [name,child]:node) {if(name=="t") out+=child.data(); else if(name!="<xmlattr>") texts(child,out);}
}
std::vector<std::string> shared_strings(const std::string& input) {
    std::vector<std::string> out; size_t at=0;
    while((at=input.find("<si",at))!=std::string::npos) {
        if(at+3<input.size() && input[at+3]!='>' && input[at+3]!=' ') {at+=3;continue;}
        size_t end=input.find("</si>",at); if(end==std::string::npos) throw std::runtime_error("invalid XLSX shared strings");
        auto tree=xml(input.substr(at,end+5-at));std::string value;texts(tree,value);out.push_back(value);at=end+5;
    } return out;
}
}
std::vector<std::vector<std::string>> read_xlsx(const std::string& path) {
    Zip zip(path);
    const auto workbook=xml(strip_tag_prefix(zip.get("xl/workbook.xml")));
    std::string id;
    for(const auto& [key,sheet]:workbook.get_child("workbook.sheets")) if(key=="sheet") {id=sheet.get<std::string>("<xmlattr>.r:id");break;}
    if(id.empty()) throw std::runtime_error("XLSX has no worksheet");
    const auto relationships=xml(strip_tag_prefix(zip.get("xl/_rels/workbook.xml.rels")));std::string target;
    for(const auto& [key,rel]:relationships.get_child("Relationships")) if(key=="Relationship" && rel.get<std::string>("<xmlattr>.Id","")==id) target=rel.get<std::string>("<xmlattr>.Target");
    if(target.empty()) throw std::runtime_error("XLSX worksheet relationship missing");
    if(target[0]=='/') target.erase(0,1); else target="xl/"+target;
    auto strings=shared_strings(strip_tag_prefix(zip.get("xl/sharedStrings.xml",false)));
    auto sheet=strip_tag_prefix(zip.get(target));std::vector<std::vector<std::string>> rows;
    size_t at=0;
    while((at=sheet.find("<row",at))!=std::string::npos) {
        if(at+4<sheet.size() && sheet[at+4]!='>' && sheet[at+4]!=' ') {at+=4;continue;}
        size_t end=sheet.find("</row>",at);if(end==std::string::npos) throw std::runtime_error("invalid XLSX row");
        auto tree=xml(sheet.substr(at,end+6-at));std::vector<std::string> row;
        for(const auto& [key,cell]:tree.get_child("row")) if(key=="c") {
            const auto address=cell.get<std::string>("<xmlattr>.r",""); size_t col=0,j=0;
            while(j<address.size() && address[j]>='A' && address[j]<='Z') {col=col*26+size_t(address[j]-'A'+1);++j;}
            if(!col || col>16384) throw std::runtime_error("invalid XLSX cell address"); --col;
            if(cell.get_child_optional("f")) throw std::runtime_error("formulas are not supported in the site lookup");
            const auto type=cell.get<std::string>("<xmlattr>.t","");std::string value;
            if(type=="inlineStr") {auto v=cell.get_child_optional("is");if(v) texts(*v,value);}
            else {value=cell.get<std::string>("v","");if(type=="s") {size_t index=std::stoull(value);if(index>=strings.size()) throw std::runtime_error("invalid XLSX shared string");value=strings[index];}}
            if(row.size()<=col) row.resize(col+1);row[col]=value;
        }
        rows.push_back(std::move(row));at=end+6;
    }
    return rows;
}
}
