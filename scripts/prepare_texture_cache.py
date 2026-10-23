"""Retain cached textures and grow storage for the live submission."""


def prepare(text):
    def once(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned texture-cache anchor changed: ' + old[:90])
        text = text.replace(old, new)

    once('    u32 csum;           /* sparse checksum of the source bytes last uploaded */',
         '    u64 last_used;      /* host cache access order; never guest time */\n'
         '    u32 csum;           /* sparse checksum of the source bytes last uploaded */')

    once('VPTexSlot             vp_tex[VP_TEX_SLOTS];', 'VPTexSlot*            vp_tex;\n    unsigned             vp_tex_capacity;')
    begin = text.index('/* Distinct textures uploadable per frame.')
    end = text.index('#define VP_TEX_SLOTS 96', begin) + len('#define VP_TEX_SLOTS 96')
    text = text[:begin] + '''/* Initial CPU slot capacity; live submissions grow it as needed. */
#define PORT_TEXTURE_INITIAL_CAPACITY 96''' + text[end:]
    # VP draws bind their own descriptor windows. The redundant legacy write
    # at 1+slot must go before a growing pool could reach the RT descriptor range.
    begin = text.index('    /* SRV at heap slot 1+slot */')
    end = text.index('\n    t->off = off;', begin)
    if 'CreateShaderResourceView' not in text[begin:end]:
        raise ValueError('Pinned legacy texture SRV write changed')
    text = text[:begin] + text[end:]
    for old, new in (
        ('for(unsigned i=0;i<VP_TEX_SLOTS;i++)', 'for(unsigned i=0;i<s_d3d.vp_tex_capacity;i++)'),
        ('for (int i = 0; i < VP_TEX_SLOTS; i++)', 'for (unsigned i = 0; i < s_d3d.vp_tex_capacity; i++)'),
        ('for (int _i = 0; _i < VP_TEX_SLOTS; _i++)', 'for (unsigned _i = 0; _i < s_d3d.vp_tex_capacity; _i++)'),
    ):
        once(old, new)
    once('static int vp_upload_tex_slot(', r'''/* Pick storage only after the complete key lookup has missed. Resources
 * referenced by this submission remain pinned until it has completed. */
static u64 port_texture_use_clock;
static void port_texture_touch(VPTexSlot* slot)
{
    slot->last_used=++port_texture_use_clock;
}

static int port_texture_free_slot(const VPTexSlot* slots, unsigned count)
{
    int reusable=-1;
    for(unsigned i=0;i<count;i++) {
        if(slots[i].used) continue;
        if(!slots[i].res) return (int)i;
        if(reusable<0 || slots[i].last_used<slots[reusable].last_used) reusable=(int)i;
    }
    return reusable;
}

/* Only CPU metadata moves; recorded GPU resource identities stay valid. */
static int port_texture_grow(VPTexSlot** slots,unsigned* capacity)
{
    unsigned old=*capacity;
    if(old>0x3FFFFFFFu) return -1;
    unsigned count=old?old*2:PORT_TEXTURE_INITIAL_CAPACITY;
    if((size_t)count>SIZE_MAX/sizeof(**slots)) return -1;
    VPTexSlot* grown=(VPTexSlot*)realloc(*slots,(size_t)count*sizeof(*grown));
    if(!grown) return -1;
    memset(grown+old,0,(size_t)(count-old)*sizeof(*grown));
    *slots=grown;*capacity=count;
    return (int)old;
}

int port_check_texture_cache(void)
{
    VPTexSlot slots[4]={0};
    /* A new batch references B while A from the previous batch is retained.
     * Uploading B must fill empty storage rather than evicting A. No GPU or
     * guest assets are needed; resource identities are never dereferenced. */
    slots[0].res=(ID3D12Resource*)&slots[0];
    int empty=port_texture_free_slot(slots,4)==1;
    slots[1].res=(ID3D12Resource*)&slots[1];slots[1].used=1;
    int pinned=port_texture_free_slot(slots,4)==2;
    slots[2].res=(ID3D12Resource*)&slots[2];
    slots[3].res=(ID3D12Resource*)&slots[3];
    int reuse=port_texture_free_slot(slots,4)==0;
    VPTexSlot hot[4]={0};
    for(unsigned i=0;i<4;i++) {
        hot[i].res=(ID3D12Resource*)&hot[i];port_texture_touch(&hot[i]);
    }
    port_texture_touch(&hot[0]); /* Most recent, although first in storage. */
    hot[1].used=1;             /* Oldest is pinned to the current submission. */
    int oldest=port_texture_free_slot(hot,4)==2;
    hot[2].used=1;
    int oldest_unpinned=port_texture_free_slot(hot,4)==3;
    hot[3].res=NULL;port_texture_touch(&hot[3]);
    int empty_before_old=port_texture_free_slot(hot,4)==3;
    for(unsigned i=0;i<4;i++) slots[i].used=1;
    int full=port_texture_free_slot(slots,4)==-1;
    slots[3].res=NULL;
    int pinned_empty=port_texture_free_slot(slots,4)==-1;
    VPTexSlot* pool=NULL;unsigned capacity=0;int growth=1;
    for(unsigned key=1;key<=200 && growth;key++) {
        int slot=port_texture_free_slot(pool,capacity);
        if(slot<0) slot=port_texture_grow(&pool,&capacity);
        if(slot<0) { growth=0;break; }
        if(pool[slot].res || pool[slot].used || pool[slot].key) growth=0;
        pool[slot].res=(ID3D12Resource*)&slots[0];
        pool[slot].used=1;pool[slot].key=key;
    }
    if(capacity<200) growth=0;
    for(unsigned i=0;i<200 && growth;i++)
        if(pool[i].key!=i+1 || !pool[i].used || pool[i].res!=(ID3D12Resource*)&slots[0]) growth=0;
    free(pool);
    int ok=empty&&pinned&&reuse&&full&&pinned_empty&&growth&&oldest&&oldest_unpinned&&empty_before_old;
    fprintf(stderr,"[compat-check] texture cache empty=%d pinned=%d reuse=%d full=%d pinned-empty=%d growth200=%d oldest=%d oldest-unpinned=%d empty-before-old=%d %s\n",
        empty,pinned,reuse,full,pinned_empty,growth,oldest,oldest_unpinned,empty_before_old,ok?"PASS":"FAIL");
    return ok?0:1;
}

static int vp_upload_tex_slot(''')
    once('        if (!c->used && freeslot < 0) freeslot = i;', '')
    once('            && c->cube == cube) {',
         '            && c->cube == cube) {\n            port_texture_touch(c);')
    once('    t->cube = cube; t->used = 1;',
         '    t->cube = cube; t->used = 1;port_texture_touch(t);')
    once('    /* TEX_OFF_BIAS=1:', '''    if(slot<0) freeslot=port_texture_free_slot(s_d3d.vp_tex,s_d3d.vp_tex_capacity);
    /* TEX_OFF_BIAS=1:''')
    once('    if (slot < 0 && freeslot < 0) return -1;', '''    if (slot < 0 && freeslot < 0) {
        freeslot=port_texture_grow(&s_d3d.vp_tex,&s_d3d.vp_tex_capacity);
        if(freeslot<0) {
            fprintf(stderr,"[port-renderer] cannot grow texture upload storage\\n");
            abort();
        }
    }''')
    # The public backend shutdown already waits for its GPU fence here.
    once('    /* Release D3D12 resources */', '''    /* Release retained texture resources after the shutdown fence. */
    if(s_d3d.vp_tex) {
        for(unsigned i=0;i<s_d3d.vp_tex_capacity;i++) {
            VPTexSlot* t=&s_d3d.vp_tex[i];
            if(t->res) t->res->lpVtbl->Release(t->res);
            if(t->up) t->up->lpVtbl->Release(t->up);
        }
        free(s_d3d.vp_tex);s_d3d.vp_tex=NULL;s_d3d.vp_tex_capacity=0;
    }
    /* Release D3D12 resources */''')
    return text
