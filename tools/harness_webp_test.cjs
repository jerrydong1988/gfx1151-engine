// Installed Harness integration; isolated attachments and service-registry facade.
// Usage: Electron (ELECTRON_RUN_AS_NODE=1) this.cjs <app.asar/dsh> <base/v1> <output>
const fs=require('node:fs');
const path=require('node:path');
const assert=require('node:assert/strict');
const {pathToFileURL}=require('node:url');
const root=process.argv[2];
const baseURL=process.argv[3];
const modelId='qwen3.8-flash-next';
const providerId='gfx1151-engine';
const output=path.resolve(process.argv[4]);fs.mkdirSync(output,{recursive:true});
const storeRoot=path.join(output,'isolated-dsh','attachments','v1');
const cacheRoot=path.join(output,'isolated-dsh','cache','attachments');
async function main(){
  const attachment=await import(pathToFileURL(path.join(root,'node_modules/@deepseek-ai/dsh-attachment-local/lib/index.js')).href);
  const sharp=require(path.join(root,'node_modules/sharp'));
  const requests=[];
  const fetchOriginal=globalThis.fetch;
  globalThis.fetch=async function(input,init){
    const url=typeof input==='string'?input:input.url;
    if(url?.startsWith(baseURL+'/chat/completions')){
      const raw=init?.body??(input.clone?await input.clone().text():null);
      if(raw){const body=JSON.parse(raw);requests.push({stream:body.stream,roles:body.messages.map(m=>m.role),
        images:body.messages.flatMap(m=>Array.isArray(m.content)?m.content.filter(x=>x.type==='image_url').map(x=>({prefix:x.image_url.url.split(',')[0],bytes:x.image_url.url.length})):[])});}
    }
    return fetchOriginal(input,init);
  };
  const pkg=await import(pathToFileURL(path.join(root,'node_modules/@deepseek-ai/dsh-llm-pi-ai/lib/index.js')).href);
  const original=require(path.join(root,'node_modules/yaml')).parse(fs.readFileSync(path.join(process.env.USERPROFILE,'.dsh/profiles/desktop/cordis.patch.yml'),'utf8'));
  const saved=original.find(x=>x.id==='llm-pi-ai').config;
  assert(saved.providers[providerId]);saved.providers[providerId].baseURL=baseURL;
  const config=pkg.Config(saved);
  let adapter;
  // Use the installed attachment implementation end to end, with isolated disk
  // storage and cache. Only the application's service registry is a test facade.
  const attachments={readImageRequest:async(ref,target,signal)=>attachment.readRequestImageFile(cacheRoot,await attachment.readImageFile(storeRoot,ref,signal),target,signal)};
  const ctx={fiber:{entry:{options:{id:'llm-pi-ai'}}},inject(){},on(){},get(name){return name==='attachments'?attachments:undefined},logger:console,
    llm:{registerConfigurableProviders(){return {replace(){}}},registerModelDiscovery(){},registerAdapter(routes,value){adapter=value;return {replace(){}}}}};
  pkg.apply(ctx,config);
  adapter.config.resolveImageAccess=()=>undefined;
  const png=await sharp({create:{width:2936,height:1651,channels:4,background:{r:0,g:0,b:255,alpha:0.5}}}).png().toBuffer();
  const prepared=await attachment.prepareImageFile({data:png,mediaType:'image/png',name:'synthetic-blue.png'},
    {maxImageBytes:20971520,maxImagePixels:64000000,maxImageDimension:8192},
    {maxPixels:4194304,maxDimension:8192,maxBytes:4194304});
  const ref=await attachment.commitPreparedImageFile(storeRoot,prepared);
  const fixturePath=path.join(output,'synthetic-blue.png');fs.writeFileSync(fixturePath,png);
  attachments.imageLimits={mediaTypes:['image/png','image/jpeg','image/webp'],maxImageBytes:20971520,maxMessageImageBytes:20971520,maxImagePixels:64000000,maxImageDimension:8192};
  let saveCalls=0;
  attachments.saveImage=async input=>{saveCalls++;return attachment.commitPreparedImageFile(storeRoot,await attachment.prepareImageFile(input,attachments.imageLimits,{maxPixels:4194304,maxDimension:8192,maxBytes:4194304}));};
  const fsPkg=await import(pathToFileURL(path.join(root,'node_modules/@deepseek-ai/dsh-tool-fs/lib/index.js')).href);
  let readTool;
  const fileContext={systemPrompt:{section(){},getSectionOrder(){return 0;}},
    tools:{register(t){if(t.name==='read_image')readTool=t;},get(){}},
    inject(names,fn){fn(fileContext);},get(n){return n==='attachments'?attachments:n==='llm'?{resolveModelInfo:async()=>({inputModalities:['text','image']})}:undefined;},
    emit(){},workingDirectory:{ensure:async()=>output},
    fs:{resolve:async p=>{const resolved=path.resolve(output,p);assert.equal(resolved,fixturePath);return {displayPath:resolved};},
      stat:async t=>({type:'file',size:fs.statSync(t.displayPath).size,version:'fixture'}),
      readBytes:async t=>fs.readFileSync(t.displayPath),processPath:t=>t.displayPath}};
  fsPkg.apply(fileContext,fsPkg.Config({}));assert(readTool);

  assert.equal(ref.mediaType,'image/webp');
  assert.equal(ref.width,2730);assert.equal(ref.height,1535);
  if(process.argv.includes('--prepare-only')){console.log(JSON.stringify({fixture:ref,prepared:true}));return;}
  const report={fixture:ref,checks:[],requests};
  const user=text=>({role:'user',content:[{type:'text',text}]});
  const tools=[{name:'read_image',description:'Read the named image file.',parameters:{type:'object',properties:{file_path:{type:'string'}},required:['file_path'],additionalProperties:false}}];
  async function call(name,messages,extra={}){
    const begin=Date.now();const events=[];
    for await(const event of adapter.stream({provider:providerId,model:modelId,messages,reasoningEffort:'off',temperature:0,maxTokens:128,signal:AbortSignal.timeout(180000),...extra}))events.push(event);
    const blocks=events.filter(e=>e.type==='block-end').map(e=>e.block);
    const text=blocks.filter(b=>b.type==='text').map(b=>b.text).join('');
    const finish=events.findLast(e=>e.type==='finish');
    const row={name,text,blocks,finish,elapsedMs:Date.now()-begin};report.checks.push(row);
    fs.writeFileSync(path.join(output,'harness-report.json'),JSON.stringify(report,null,2));
    assert(finish&&!['error','aborted','max-tokens'].includes(finish.reason.kind),JSON.stringify(finish));
    console.log(name,JSON.stringify({text,finish:finish.reason,elapsedMs:row.elapsedMs}));return row;
  }
  const direct=await call('direct_webp',[{role:'user',content:[{type:'text',text:'Name the dominant color. Reply with one English color word only.'},{type:'image',attachment:ref}]}]);
  assert(/blue/i.test(direct.text));
  const start=[user('Call read_image once with file_path synthetic-blue.png. After reading, name the dominant color in one English word.')];
  const tool=await call('read_image_call',start,{tools});
  const block=tool.blocks.find(b=>b.type==='tool-call');assert.equal(block?.name,'read_image');
  const toolArgs=JSON.parse(block.arguments);
  assert.equal(toolArgs.file_path,'synthetic-blue.png');
  const toolValue=await readTool.execute(toolArgs,{signal:AbortSignal.timeout(30000),agent:{session:{requestHeader:()=>({config:{provider:providerId,model:modelId}})},options:{provider:providerId,model:modelId}}});
  const toolContent=readTool.output.render(toolArgs,toolValue);
  assert.equal(toolValue.image.mediaType,'image/webp');assert.equal(saveCalls,1);
  report.readImageTool={installedImplementation:true,value:toolValue};
  const history=[...start,{role:'assistant',content:tool.blocks,source:{provider:providerId,model:modelId,replayState:tool.finish.replayState}},
    {role:'tool',toolCallId:block.id,content:toolContent}];
  const follow=await call('webp_tool_result',history,{tools});assert(/blue/i.test(follow.text));
  const replay=await call('webp_history_replay',[...history,{role:'assistant',content:follow.blocks,source:{provider:providerId,model:modelId,replayState:follow.finish.replayState}},user('What was the color of the image just read? Reply with one English color word only.')],{tools});
  assert(/blue/i.test(replay.text));
  assert.equal(requests.length,4);assert(requests.every(r=>r.stream));
  for(const i of [0,2,3])assert(requests[i].images.some(x=>x.prefix==='data:image/webp;base64'));
  report.installedPackages=Object.fromEntries(['dsh-tool-fs','dsh-attachment-local','dsh-llm-pi-ai'].map(n=>[n,JSON.parse(fs.readFileSync(path.join(root,'node_modules/@deepseek-ai',n,'package.json'),'utf8')).version]));
  report.passed=true;fs.writeFileSync(path.join(output,'harness-report.json'),JSON.stringify(report,null,2));
  console.log('ALL_REAL_ATTACHMENT_AND_ADAPTER_CHECKS_PASSED');
}
main().catch(e=>{console.error(e.message);process.exitCode=1;});
