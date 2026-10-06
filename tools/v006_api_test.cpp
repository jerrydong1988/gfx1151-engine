// Compile with the API's other translation units; exercise real stream splitting.
#define main gdec_api_real_main
#include "../src/api/main.cpp"
#undef main
#include <iostream>

int main(int argc,char** argv) {
  auto require=[](bool ok,const char* name){if(!ok)throw std::runtime_error(name);};
  ThinkSplitter first(true);std::string r,c;first.feed("We",&r,&c);
  require(r=="We" && c.empty(),"first piece must be emitted without another token");
  size_t cases=1;
  for(bool enabled : {false,true}) {
    for(const std::string text : {std::string("We think\n\n</think>\n\nAnswer\nnext"),
            std::string("中文推理\n</think>\n中文答案"),std::string("reason < literal </thiX then\n"),
            std::string("\n\n</think>\n\n"), std::string("unfinished </thi"),
            std::string("line\n\ninside reasoning\n\n")}) {
      std::string er,ec;split_reasoning(text,enabled,&er,&ec);
      // Every two-fragment split and every constant byte-sized fragmentation.
      for(size_t cut=0;cut<=text.size();++cut) {
        ThinkSplitter s(enabled);std::string gotr,gotc;
        for(auto piece : {text.substr(0,cut),text.substr(cut)}) {
          r.clear();c.clear();s.feed(piece,&r,&c);gotr+=r;gotc+=c;
        }
        r.clear();c.clear();s.finish(&r,&c);gotr+=r;gotc+=c;
        require(gotr==er && gotc==ec,"fragmented stream disagrees with non-streaming");++cases;
      }
      for(size_t width=1;width<=9;++width) {
        ThinkSplitter s(enabled);std::string gotr,gotc;
        for(size_t off=0;off<text.size();off+=width){r.clear();c.clear();s.feed(text.substr(off,width),&r,&c);gotr+=r;gotc+=c;}
        r.clear();c.clear();s.finish(&r,&c);gotr+=r;gotc+=c;
        require(gotr==er && gotc==ec,"chunked stream disagrees with non-streaming");++cases;
      }
    }
  }
  require(argc==2,"tokenizer path required");
  require(g_tok.load(argv[1]),"tokenizer load");
  auto& tokenizer=g_tok;
  const std::unordered_set<int> pads={248056,248057};
  for(const auto& text : {"explain <|image_pad|> please","<|video_pad|> is a token", "中文 <|image_pad|> <|video_pad|> 文本"}) {
    auto plain=tokenizer.encode(text,false,&pads),original=tokenizer.encode(text);
    require(std::none_of(plain.begin(),plain.end(),[&](int id){return pads.count(id);}),"literal pad escaped");
    require(std::any_of(original.begin(),original.end(),[&](int id){return pads.count(id);}),"default behavior retained");++cases;
    // An older persisted transcript containing a special pad must not poison
    // a new text request, even when the byte prefix matches.
    g_tcache.set_capacity(10000);g_tcache.record(original);
    std::vector<int> ids;std::vector<size_t> starts;std::string error;
    require(encode_spliced(text,nullptr,&ids,&starts,&error),"encode cached text");
    require(ids==plain && ids.size()==starts.size(),"old pad cache safely bypassed");++cases;
  }
  vision::Frame frame;frame.grid={1,4,4};std::vector<vision::Frame> frames={frame};
  std::vector<int> ids;std::vector<size_t> starts;std::string error;
  const std::string mixed="Discuss <|image_pad|> and <|video_pad|>. <|vision_start|><|image_pad|><|vision_end|> describe.";
  require(vision::encode_prompt(mixed,tokenizer,frames,&ids,&error,&starts),"mixed text and real image");
  require(std::count(ids.begin(),ids.end(),248056)==frame.pad_tokens() &&
          std::count(ids.begin(),ids.end(),248057)==0 && starts.size()==ids.size(),"only real image expanded");++cases;
  frames.push_back(frame);
  require(!vision::encode_prompt(mixed,tokenizer,frames,&ids,&error,&starts),"frame mismatch still rejected");++cases;
  std::cout<<"RESULT PASS cases="<<cases<<"\n";
}
