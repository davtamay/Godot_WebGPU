// Test-only code injected into the exported Emscripten closure by gpu-depth-integration.mjs.
if (typeof window !== 'undefined') {
 let device;
 const identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];
 const stereo=[{eye:'left',projectionMatrix:identity,transform:{matrix:identity}},{eye:'right',projectionMatrix:identity,transform:{matrix:identity}}];
 let sources=[];
 const session=()=>({depthUsage:'gpu-optimized',depthActive:true,end:()=>Promise.resolve()});
 const make=(values,format='r32float',layers=1,width=4,height=2)=>{
  const texture=device.createTexture({size:[width,height,layers],format,usage:GPUTextureUsage.TEXTURE_BINDING|GPUTextureUsage.COPY_DST});sources.push(texture);
  device.queue.writeTexture({texture},format==='r16uint'?new Uint16Array(values):new Float32Array(values),{bytesPerRow:width*(format==='r16uint'?2:4),rowsPerImage:height},[width,height,layers]);return texture;
 };
 const depth=(texture,layer=0,matrix=identity,scale=1)=>({texture,width:texture.width,height:texture.height,rawValueToMeters:scale,normDepthBufferFromNormView:{matrix},getViewDescriptor:()=>({dimension:'2d',baseArrayLayer:layer,arrayLayerCount:1,baseMipLevel:0,mipLevelCount:1})});
 const fill=n=>Array(8).fill(n);
 window.__depthCaseExpire=()=>{for(const texture of sources)texture.destroy();sources=[];};
 window.__depthCaseSetup=index=>{
  if(index>=14)return null;
  device=Module['preinitializedWebGPUDevice'];
  window.__depthCaseExpire();
  if(!GodotWebXR.session)GodotWebXR.session=session();
  GodotWebXR.session.depthUsage='gpu-optimized';GodotWebXR.session.depthActive=true;
  GodotWebXR.frame={};GodotWebXR.pose={views:stereo};
  let items=[],values=[],status='ok',mask=3,sample_layer=0,name='';
  if(index===0||index===1){name='float stereo eye '+index;items=[depth(make(fill(2))),depth(make(fill(3)),0,identity,2)];values=[fill(2),fill(6)];sample_layer=index;}
  if(index===2){name='integer explicit reversed layers';const t=make([...fill(1000),...fill(2000)],'r16uint',2);items=[depth(t,1,identity,.001),depth(t,0,identity,.001)];values=[fill(2),fill(1)];}
  if(index===3){name='UV flip';const m=identity.slice();m[0]=-1;m[12]=1;items=[depth(make([1,2,3,4,5,6,7,8]),0,m),null];values=[[4,3,2,1,8,7,6,5],fill(0)];mask=1;}
  if(index===4){name='UV uncovered is unknown';const m=identity.slice();m[12]=.5;items=[depth(make([1,2,3,4,5,6,7,8]),0,m),null];values=[[3,4,0,0,7,8,0,0],fill(0)];mask=1;}
  if(index===5){name='missing left keeps right';items=[null,depth(make(fill(6)))];values=[fill(0),fill(6)];mask=2;sample_layer=1;}
  if(index===6){name='both missing';status='no_depth_data';mask=0;}
  if(index===7){name='paused';GodotWebXR.session.depthActive=false;status='depth_paused';mask=0;}
  if(index===8){name='CPU fallback';GodotWebXR.session.depthUsage='cpu-optimized';status='cpu_depth';mask=0;}
  if(index===9){name='outside frame';GodotWebXR.frame=null;status='no_frame';mask=0;}
  if(index===10){name='ambiguous array rejected';const d=depth(make([...fill(1),...fill(2)],'r32float',2));delete d.getViewDescriptor;items=[d,null];status='no_depth_data';mask=0;}
  if(index===11){name='resize to mono array';GodotWebXR.pose={views:[stereo[0]]};items=[depth(make([8,10],'r32float',1,2,1))];values=[[8,10]];mask=1;}
  if(index===12){name='session restart';GodotWebXR.session=session();items=[depth(make(fill(4))),depth(make(fill(7)))];values=[fill(4),fill(7)];sample_layer=1;}
  if(index===13){name='session ended';GodotWebXR.session=null;status='no_session';mask=0;}
  GodotWebXR.gpu_binding={getDepthInformation:view=>items[GodotWebXR.pose.views.indexOf(view)]||null};
  return {name,status,mask,values,sample_layer};
 };
}
