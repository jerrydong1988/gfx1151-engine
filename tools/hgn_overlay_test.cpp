// Host-only regression for the merged v1/v2 loader. No private weights required.
#include "../src/hgn.h"
#include <fstream>
#include <iostream>

static void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
static void write(const std::filesystem::path& path, bool v2, bool malformed=false) {
  hgn::Header h{};
  memcpy(h.magic,"HGN1",4);h.version=1;h.tensor_count=2;
  h.records_offset=sizeof(h);h.data_offset=sizeof(h)+2*sizeof(hgn::Record);
  h.file_size=h.data_offset+4;
  hgn::Record records[2]{};
  strcpy(records[0].name,"norm.weight");records[0].dtype=0;
  strcpy(records[1].name,"projection.weight");records[1].dtype=v2?16:0;
  for(int i=0;i<2;i++) {
    records[i].ndims=1;records[i].dims[0]=1;
    records[i].data_offset=h.data_offset+i*2;records[i].data_size=2;
  }
  records[1].extra=uint64_t(0x120a)<<32;
  if(malformed) records[1].data_offset=h.file_size+1;
  std::ofstream out(path,std::ios::binary);
  out.write((const char*)&h,sizeof(h));out.write((const char*)records,sizeof(records));
  const uint16_t data[2]={uint16_t(v2?0x3f80:0x4000),0x4040};
  out.write((const char*)data,sizeof(data));
  check(bool(out),"fixture write");
}
int main(int argc,char**argv) try {
  if(argc!=2) throw std::runtime_error("usage: hgn-overlay-test existing-temp-dir");
  const std::filesystem::path dir=argv[1];
  check(std::filesystem::is_directory(dir),"scratch directory must already exist");
  const auto base=dir/"base.hgn", overlay=dir/"overlay.hgn", bad=dir/"bad.hgn";
  write(base,true);write(overlay,false);write(bad,false,true);
  {
    hgn::Checkpoint ck(base.string().c_str());
    check(ck.at("projection.weight").qparam==0x120a,"qparam preserved");
    const auto* norm=ck.at("norm.weight").data;
    check(ck.add_overlay((dir/"."/"base.hgn").string().c_str())!=nullptr,"same file skipped");
    check(ck.mappings().size()==1,"duplicate mapping avoided");
    check(ck.add_overlay(overlay.string().c_str())!=nullptr,"v1 overlay rejected for v2");
    check(ck.mappings().size()==1,"rejected overlay unmapped");
    check(ck.at("norm.weight").data==norm,"overlay rejection is atomic");
    check(ck.at("projection.weight").dtype==16,"v2 dtype retained");
    check(ck.mapping_of(norm)==&ck.mappings()[0],"mapping ownership");
  }
  {
    hgn::Checkpoint ck(overlay.string().c_str());
    check(ck.add_overlay(base.string().c_str())==nullptr,"v1 still accepts valid overlays");
    check(ck.at("projection.weight").dtype==16,"v1 overlay applied");
  }
  bool rejected=false;
  try {hgn::Checkpoint ck(bad.string().c_str());}catch(const std::exception&){rejected=true;}
  check(rejected,"out of file tensor rejected");
  std::filesystem::remove(base);std::filesystem::remove(overlay);std::filesystem::remove(bad);
  std::cout<<"PASS v1/v2 overlay compatibility, mapping dedup, qparam and bounds\n";
} catch(const std::exception&e) {std::cerr<<e.what()<<"\n";return 1;}
