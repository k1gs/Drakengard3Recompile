"""Preserve pending GPU commands and expose their real IO mapping for diagnosis."""
import re


def prepare(sdk, output):
    source=sdk/'libs/video/cellGcmSys.c'
    text=source.read_text(encoding='utf-8')
    old='    if (v < 0) v = getenv("GCM_FLIPCB_ONTICK") ? 1 : 0;'
    if text.count(old)!=1: raise ValueError('Pinned flip callback policy changed')
    # Never enter guest code synchronously on the FIFO owner. The existing
    # pending-callback pump delivers completion on a guest thread after Present.
    text=text.replace(old,'    if (v < 0) v = 1;')
    start=text.index('s32 cellGcmSetPrepareFlip(void* ctx, u32 bufferId)\n{')
    end=text.index('\n}\n',start)+3
    if 's_flip_request_count++' not in text[start:end]:
        raise ValueError('Pinned prepare-flip implementation changed')
    text=text[:start]+r'''s32 cellGcmSetPrepareFlip(void* ctx, u32 bufferId)
{
    uintptr_t address=(uintptr_t)ctx;
    if(!address || address>0xFFFFFFEFu || bufferId>=CELL_GCM_MAX_DISPLAY_BUFFER_NUM ||
       !s_display_buffer_set[bufferId]) return CELL_GCM_ERROR_INVALID_VALUE;
    u32 context=(u32)address;
    u32 current=vm_read32(context+8),end=vm_read32(context+4);
    if(current>end || end-current<8) {
        u32 callback=vm_read32(context+12);
        if(!callback) return CELL_GCM_ERROR_FAILURE;
        extern u64 ppu_guest_call(u32,u64,u64,u64,u64,u64,u64,u64,u64);
        s32 result=(s32)ppu_guest_call(callback,context,8,0,0,0,0,0,0);
        if(result) return result;
        current=vm_read32(context+8);end=vm_read32(context+4);
    }
    u32 io=gcm_ea2io(current);
    if(current>end || end-current<8 || io==0xFFFFFFFFu)
        return CELL_GCM_ERROR_FAILURE;
    /* Preparation queues a command. Display ownership, callbacks and the
     * frame counter change only when the FIFO owner consumes this packet. */
    vm_write32(current,0x0004E924u);
    vm_write32(current+4,bufferId);
    vm_write32(context+8,current+8);
    u32 active_default=s_gcm_ctx_out_ea ? vm_read32(s_gcm_ctx_out_ea) : s_gcm_context_ea;
    { static unsigned trace_count;
      if(getenv("GCM_CTXDBG") && trace_count++<16)
          fprintf(stderr,"[prepare-flip] ctx=%08X active=%08X queued=%08X put=%08X publish=%d\n",
                  context,active_default,io,vm_read32(GCM_CONTROL_GUEST_ADDR),context==active_default); }
    if(context==active_default) vm_write32(GCM_CONTROL_GUEST_ADDR,io+8);
    return (s32)bufferId;
}
'''+text[end:]
    text='void port_renderer_submit_pending(void);\n'+text
    old='''        if (begin && end > begin && cur >= begin && cur + GCM_RECYCLE_SLACK >= end
                && head_consumed && s_title_overran) {'''
    if text.count(old)!=1: raise ValueError('Pinned ring recycle guard changed')
    text=text.replace(old,'''        /* The write head can include unpublished commands. A flip also leaves
         * the next frame queued on purpose. Never teleport GET past either:
         * all produced bytes must have been published AND actually consumed. */
        if (begin && end > begin && cur >= begin && cur + GCM_RECYCLE_SLACK >= end
                && head_consumed && s_title_overran && s_fifo_getoff == put
                && gcm_ea2io(cur) == put) {''')
    for old,new in (
        ('else if (m == 0x6Cu) vm_write32(la, v);',
         'else if (m == 0x6Cu) { port_renderer_submit_pending(); vm_write32(la, v); }'),
        ('                    rsx_process_method(&s_state, m, vm_read32(dea));',
         '                    if(m==0x50u || m==0x1D70u || m==0x1D74u) port_renderer_submit_pending();\n'
         '                    rsx_process_method(&s_state, m, vm_read32(dea));'),
    ):
        if text.count(old)!=1: raise ValueError('Pinned GPU fence anchor changed')
        text=text.replace(old,new)
    # Driver flip packets are frame boundaries just like FEAD flip markers.
    # Leave the following frame in the FIFO until the host presents this one.
    changes = (
        ('            for (u32 i = 0; i < count; i++) {\n'
         '                u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);\n'
         '                if (!dea) break;\n                u32 m',
         '            int port_flip_boundary = 0;\n'
         '            for (u32 i = 0; i < count; i++) {\n'
         '                u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);\n'
         '                if (!dea) break;\n                u32 m'),
        ('                    cellGcmSetFlipCommand(vm_read32(dea) & 7u);',
         '                    if (cellGcmSetFlipCommand(vm_read32(dea) & 7u) == 0)\n'
         '                        port_flip_boundary = 1;'),
        ('            s_fifo_getoff += 4 + count * 4;\n            continue;',
         '            s_fifo_getoff += 4 + count * 4;\n'
         '            if (port_flip_boundary) { why = "driver-flip"; break; }\n'
         '            continue;'),
    )
    for old,new in changes:
        if text.count(old)!=1: raise ValueError('Pinned FIFO flip anchor changed')
        text=text.replace(old,new)
    anchor='void gcm_note_spu_put(u32 ea, u32 size)\n{'
    if text.count(anchor)!=1: raise ValueError('GCM DMA trace anchor changed')
    text=text.replace(anchor,anchor+'\n    port_gpu_spu_write(ea,size);\n    port_gcm_notify_write(ea,size);')
    text='void port_gpu_spu_write(unsigned int,unsigned int);\nvoid port_gcm_notify_write(unsigned int,unsigned int);\nstatic volatile long s_port_park_ea;\n'+text
    if text.count('static u32 gcm_io2ea(u32 io)')!=1:
        raise ValueError('Pinned GCM IO mapping helper changed')
    start=text.index('static void gcm_fifo_resync_why(')
    end=text.index('static void gcm_fifo_dump_around(',start)
    original=text[start:end]
    if '*getoff = land;' not in original:
        raise ValueError('Pinned FIFO resync body changed')
    text=text[:start]+'''/* Preserve the command stream until the real producer releases it.
 * A write-head change does not authorize discarding a parked segment. */
static void gcm_fifo_resync_why(const char* why, u32* getoff, u32 put)
{
    _InterlockedExchange(&s_port_park_ea,(long)gcm_io2ea(*getoff));
    static u32 last_get=0xFFFFFFFFu,last_put=0xFFFFFFFFu;
    static unsigned count;
    if ((*getoff!=last_get || put!=last_put) && count++<32) {
        u32 ea=gcm_io2ea(*getoff);
        fprintf(stderr,"[gcm-fifo] holding %s get=%08X put=%08X word=%08X\\n",
                why,*getoff,put,ea?vm_read32(ea):0);
    }
    last_get=*getoff; last_put=put;
}

'''+text[end:]
    old='            s_fifo_getoff = put;\n            why = "no-io-mapping";'
    if text.count(old)!=1:
        raise ValueError('Pinned unmapped FIFO path changed')
    text=text.replace(old,'            why = "no-io-mapping";')
    anchor='/* Serializes fence publication between the present ticker'
    if text.count(anchor)!=1: raise ValueError('Pinned kick helper changed')
    text=text.replace(anchor,'''/* Wake the existing FIFO owner after an actual producer write. This never
 * executes GPU work on a producer thread or changes a guest fence/label. */
void port_gcm_notify_write(u32 ea,u32 size) {
    if(!size || !s_gcm_kick_ev) return;
    /* Aligned 32-bit Windows load; a stale hint only delays a wake until the
     * bounded timer fallback. Avoid a locked RMW on every guest store. */
    u32 parked=(u32)s_port_park_ea;
    if(ea==GCM_CONTROL_GUEST_ADDR ||
       (parked && ea<=parked && (u64)ea+size>parked)) SetEvent(s_gcm_kick_ev);
}

'''+anchor)
    def include(m):
        p=source.parent/m[1]
        return '#include "'+p.resolve().as_posix()+'"' if p.is_file() else m[0]
    text=re.sub(r'#include "([^"]+)"',include,text)
    text+='\n/* Read-only mapping used by title FIFO snapshots. */\nu32 port_gcm_io_ea(u32 io) { return gcm_io2ea(io); }\n'
    text+='''
/* Isolated native contract fixture; only called by --check-gcm-park. */
void port_gcm_contract_begin(u32 ea) {
    s_gcm_syscall_mode=1; s_gcm_context_ea=0; s_gcm_ctx_out_ea=0;
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));
    s_ea_address_table[0]=(u16)(ea>>20);
    memset(s_io_address_table,0xFF,sizeof(s_io_address_table));
    s_io_address_table[ea>>20]=0;
    s_fifo_getoff=0; s_fifo_calloff=0;
    vm_write32(GCM_CONTROL_GUEST_ADDR,0); vm_write32(GCM_CONTROL_GUEST_ADDR+4,0);
}
u32 port_gcm_contract_get(void) { return s_fifo_getoff; }
/* Called only by the FIFO owner's opt-in clock profiler. No guest mutation. */
void port_gcm_trace_progress(void) {
    u32 context=s_gcm_ctx_out_ea ? vm_read32(s_gcm_ctx_out_ea) : s_gcm_context_ea;
    u32 ea=gcm_io2ea(s_fifo_getoff);
    fprintf(stderr,"[gcm-progress] flips=%u status=%u pending=%ld get=%08X put=%08X word=%08X ctx=%08X current=%08X callbacks=%ld\\n",
        s_flip_request_count,s_flip_status,(long)s_flip_pending,s_fifo_getoff,
        vm_read32(GCM_CONTROL_GUEST_ADDR),ea?vm_read32(ea):0,context,
        context?vm_read32(context+8):0,(long)s_gcm_pending);
}
void port_gcm_contract_default_context(u32 context) {
    s_gcm_ctx_out_ea=context+0x10;
    vm_write32(s_gcm_ctx_out_ea,context);
}
'''
    target=output/'cellGcmSys_compat.c'
    if not target.exists() or target.read_text(encoding='utf-8')!=text:
        target.write_text(text,encoding='utf-8')
