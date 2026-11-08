/* Actual uploader, not a second implementation. Golden source layouts below
 * include rectangular Morton data, fixed-pitch linear levels, BC tails and
 * packed/linear cube face strides. Read back every uploaded subresource. */
int port_check_mip_uploads_gpu(void)
{
    if(s_d3d.device || s_d3d.vp_tex) return 2;
    ID3D12Device* device=NULL;IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;
    ID3D12CommandQueue* queue=NULL;ID3D12CommandAllocator* allocator=NULL;
    ID3D12GraphicsCommandList* list=NULL;ID3D12Fence* fence=NULL;HANDLE event=NULL;
    ID3D12Resource* readback=NULL;HRESULT hr=S_OK;int ok=0,software=0;
    unsigned checked=0;int cache_hit=0,pinned=0,level_key=0,pitch_key=0;
    struct Test {u32 fmt,w,h,pitch,faces,stride,chain,offsets[3];};
    static const struct Test cases[]={
        {0x85,4,2,0,1,44,44,{0,32,40}},
        {0xA5,4,2,24,1,96,96,{0,48,72}},
        {0xA5,4,4,24,6,168,168,{0,96,144}},
        {0x86,4,4,0,6,128,24,{0,8,16}},
        {0x87,4,4,0,6,128,48,{0,16,32}},
        {0xA8,4,4,32,1,96,96,{0,32,64}},
        {0xA1,7,3,12,1,60,60,{0,36,48}},
        {0x88,4,4,0,6,128,48,{0,16,32}},
        {0xA4,4,2,12,1,48,48,{0,24,36}},
        {0x85,4,4,0,6,128,84,{0,64,80}}
    };
    struct Record {int slot;unsigned test,mutation,levels;u32 first;};
    struct Record records[13];unsigned record_count=0,total_subs=0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[216];UINT64 total=0;
#define MIP_TRY(call) do {hr=(call);if(FAILED(hr)) goto cleanup;} while(0)
    hr=D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
    if(FAILED(hr)) {
        software=1;
        MIP_TRY(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
        MIP_TRY(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
        MIP_TRY(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device));
    }
    s_d3d.device=device;
    D3D12_COMMAND_QUEUE_DESC qd={0};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    MIP_TRY(device->lpVtbl->CreateCommandQueue(device,&qd,&IID_ID3D12CommandQueue,&queue));
    MIP_TRY(device->lpVtbl->CreateCommandAllocator(device,qd.Type,&IID_ID3D12CommandAllocator,&allocator));
    MIP_TRY(device->lpVtbl->CreateCommandList(device,0,qd.Type,allocator,NULL,&IID_ID3D12GraphicsCommandList,&list));
    MIP_TRY(device->lpVtbl->CreateFence(device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,&fence));
    event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!event) goto cleanup;
    s_d3d.cmd_list=list;
    for(unsigned test=0;test<sizeof(cases)/sizeof(cases[0]);test++) {
        const struct Test* c=&cases[test];u32 ea=0x600A0000u+test*0x10000u;
        u8* src=vm_base+ea;size_t span=(c->faces-1)*c->stride+c->chain;memset(src,0xCD,span);
        u32 base=c->fmt&0x9F;int bc=base>=0x86 && base<=0x88;
        for(u32 face=0;face<c->faces;face++) for(u32 mip=0;mip<3;mip++) {
            u32 w=c->w>>mip,h=c->h>>mip;if(!w) w=1;if(!h) h=1;
            u8* p=src+face*c->stride+c->offsets[mip];
            if(bc) {
                unsigned bytes=base==0x86?8:16;
                for(unsigned b=0;b<bytes;b++) p[b]=(u8)(test*13+face*19+mip*31+b);
            } else for(u32 y=0;y<h;y++) for(u32 x=0;x<w;x++) {
                u32 bpp=base==0x81?1:base==0x84?2:4;
                /* Independent small Morton index: pair x/y low bits then
                 * x's second bit. All swizzled goldens are 4x2 or 4x4. */
                u32 index=(x&1)|((y&1)<<1)|((x&2)<<1)|((y&2)<<2);
                if(w==2 && h==1) index=x;
                if(w==1 && h==1) index=0;
                u8* pixel=(c->fmt&0x20)?p+y*c->pitch+x*bpp:p+index*bpp;
                if(base==0x81) pixel[0]=(u8)(mip*60+x+y);
                else if(base==0x84) {u16 color=mip==0?0xF800:mip==1?0x07E0:0x001F;pixel[0]=(u8)(color>>8);pixel[1]=(u8)color;}
                else {pixel[0]=255;pixel[1]=(u8)(mip*60+x);pixel[2]=(u8)(face*30+y);pixel[3]=17;}
            }
        }
        int slot=vp_upload_tex_slot(ea,c->w,c->h,c->fmt,c->faces==6,3,c->pitch);
        if(slot<0) goto cleanup;
        D3D12_RESOURCE_DESC desc;s_d3d.vp_tex[slot].res->lpVtbl->GetDesc(s_d3d.vp_tex[slot].res,&desc);
        if(desc.MipLevels!=3 || desc.DepthOrArraySize!=c->faces) goto cleanup;
        if(s_d3d.vp_tex[slot].source_size!=span) goto cleanup;
        records[record_count++]=(struct Record){slot,test,0,3,0};
        if(test==0) {
            cache_hit=vp_upload_tex_slot(ea,4,2,0x85,0,3,0)==slot;
            src[41]^=0x3F; /* lower 1x1 mip only; mip0 is unchanged */
            int other=vp_upload_tex_slot(ea,4,2,0x85,0,3,0);
            pinned=other>=0 && other!=slot && s_d3d.vp_tex[other].res!=s_d3d.vp_tex[slot].res;
            if(!pinned) goto cleanup;
            records[record_count++]=(struct Record){other,test,1,3,0};
            int one=vp_upload_tex_slot(ea,4,2,0x85,0,1,0);
            level_key=one>=0 && one!=slot && one!=other &&
                vp_upload_tex_slot(ea,4,2,0x85,0,0,0)==one;
            if(!level_key) goto cleanup;
            records[record_count++]=(struct Record){one,test,0,1,0};
        }
        if(test==1) {
            memset(src,0xCD,128);
            for(unsigned mip=0;mip<3;mip++) {
                u32 w=4u>>mip,h=2u>>mip;if(!h)h=1;
                u32 offset=mip==0?0:mip==1?64:96;
                for(u32 y=0;y<h;y++)for(u32 x=0;x<w;x++) {
                    u8* p=src+offset+y*32+x*4;
                    p[0]=255;p[1]=(u8)(mip*60+x);p[2]=(u8)y;p[3]=17;
                }
            }
            int other=vp_upload_tex_slot(ea,4,2,0xA5,0,3,32);
            pitch_key=other>=0 && other!=slot && s_d3d.vp_tex[other].source_pitch==32;
            if(!pitch_key) goto cleanup;
            records[record_count++]=(struct Record){other,test,0,3,0};
        }
    }
    for(unsigned r=0;r<record_count;r++) {
        struct Record* rec=&records[r];VPTexSlot* t=&s_d3d.vp_tex[rec->slot];
        D3D12_RESOURCE_DESC td;t->res->lpVtbl->GetDesc(t->res,&td);
        unsigned count=td.MipLevels*td.DepthOrArraySize;rec->first=total_subs;
        for(unsigned sub=0;sub<count;sub++) {
            total=(total+511)&~511ull;UINT64 bytes=0;
            device->lpVtbl->GetCopyableFootprints(device,&td,sub,1,total,&fp[total_subs],NULL,NULL,&bytes);
            total=fp[total_subs].Offset+(u64)fp[total_subs].Footprint.RowPitch*fp[total_subs].Footprint.Height;
            total_subs++;
        }
    }
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb={0};rb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rb.Width=total;
    rb.Height=1;rb.DepthOrArraySize=1;rb.MipLevels=1;rb.SampleDesc.Count=1;rb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    MIP_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&rb,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,&readback));
    for(unsigned r=0;r<record_count;r++) {
        struct Record* rec=&records[r];VPTexSlot* t=&s_d3d.vp_tex[rec->slot];
        D3D12_RESOURCE_BARRIER b={0};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource=t->res;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->lpVtbl->ResourceBarrier(list,1,&b);
        unsigned count=rec->levels*cases[rec->test].faces;
        for(unsigned sub=0;sub<count;sub++) {
            D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=t->res;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=sub;
            dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp[rec->first+sub];
            list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
        }
    }
    MIP_TRY(list->lpVtbl->Close(list));
    ID3D12CommandList* lists[]={(ID3D12CommandList*)list};queue->lpVtbl->ExecuteCommandLists(queue,1,lists);
    MIP_TRY(queue->lpVtbl->Signal(queue,fence,1));MIP_TRY(fence->lpVtbl->SetEventOnCompletion(fence,1,event));
    if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0 || fence->lpVtbl->GetCompletedValue(fence)<1) abort();
    u8* data=NULL;D3D12_RANGE range={0,(SIZE_T)total};MIP_TRY(readback->lpVtbl->Map(readback,0,&range,(void**)&data));
    ok=cache_hit && pinned && level_key && pitch_key;
    for(unsigned r=0;r<record_count;r++) {
        struct Record* rec=&records[r];const struct Test* c=&cases[rec->test];u32 base=c->fmt&0x9F;
        for(unsigned face=0;face<c->faces;face++) for(unsigned mip=0;mip<rec->levels;mip++) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT* f=&fp[rec->first+face*rec->levels+mip];
            u8* bytes=data+f->Offset;int valid=1;u32 w=c->w>>mip,h=c->h>>mip;if(!w)w=1;if(!h)h=1;
            if(base>=0x86 && base<=0x88) {
                unsigned size=base==0x86?8:16;
                for(unsigned b=0;b<size;b++) if(bytes[b]!=(u8)(rec->test*13+face*19+mip*31+b)) valid=0;
            } else for(u32 y=0;y<h;y++) for(u32 x=0;x<w;x++) {
                u8* p=bytes+y*f->Footprint.RowPitch+x*(base==0x81?1:4);
                if(base==0x81) {if(p[0]!=(u8)(mip*60+x+y))valid=0;}
                else if(base==0x84) {if(p[0]!=(mip==0?255:0) || p[1]!=(mip==1?255:0) || p[2]!=(mip==2?255:0) || p[3]!=255)valid=0;}
                else {
                    u8 red=(u8)(mip*60+x);if(rec->mutation && mip==2)red^=0x3F;
                    if(p[0]!=red || p[1]!=(u8)(face*30+y) || p[2]!=17 || p[3]!=255)valid=0;
                }
            }
            if(valid)checked++;else {ok=0;fprintf(stderr,"[mip-test] case=%u face=%u mip=%u failed\n",rec->test,face,mip);}
        }
    }
    D3D12_RANGE written={0,0};readback->lpVtbl->Unmap(readback,0,&written);
cleanup:
    fprintf(stderr,"[compat-check] mip GPU device=%s subresources=%u/%u cache=%d pinned=%d level-key=%d pitch-key=%d %s hr=%08lX\n",
        software?"WARP":"hardware",checked,total_subs,cache_hit,pinned,level_key,pitch_key,ok?"PASS":"FAIL",(unsigned long)hr);
    port_mip_release_cache();s_d3d.device=NULL;s_d3d.cmd_list=NULL;
    if(event)CloseHandle(event);
#define MIP_RELEASE(p) if(p)p->lpVtbl->Release(p)
    MIP_RELEASE(readback);MIP_RELEASE(list);MIP_RELEASE(allocator);MIP_RELEASE(queue);MIP_RELEASE(fence);
    MIP_RELEASE(device);MIP_RELEASE(warp);MIP_RELEASE(factory);
#undef MIP_RELEASE
#undef MIP_TRY
    return ok?0:1;
}
