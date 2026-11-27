"""Preserve pending GPU commands and expose their real IO mapping for diagnosis."""
from pathlib import Path
from prepare_nv0039 import prepare as prepare_nv0039
from prepare_fifo_constant_stream import prepare as prepare_fifo_constant_stream
from compat_source import resolve_local_includes, write_if_changed


def prepare_display_queue(text, source, sdk, output):
    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned display queue anchor changed: ' + old[:80])
        text = text.replace(old, new)

    # The guest export returns a signed buffer ID, including -1 for a stale
    # index. The upstream declaration returns a host pointer to display info.
    header = source.with_suffix('.h')
    decl = 'CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index)'
    contents = header.read_text(encoding='utf-8')
    if contents.count(decl) != 1:
        raise ValueError('Pinned flip-index return type changed')
    contents = contents.replace(decl, 's32 cellGcmGetDisplayBufferByFlipIndex(u32 index)')
    write_if_changed(output / 'cellGcmSys_compat.h', resolve_local_includes(contents, header, sdk))
    replace('#include "libs/video/cellGcmSys.h"', '#include "cellGcmSys_compat.h"')

    for signature in (decl, 's32 cellGcmSetFlipImmediate(u32 bufferId)'):
        begin = text.index(signature + '\n{')
        end = text.index('\n}\n', begin) + len('\n}\n')
        text = text[:begin] + text[end:]

    anchor = 'static u32 s_sema_resume_arg;'
    replace(anchor, '#include "gcm_display_queue.inc"\n' + anchor)
    replace('static void port_sema_reset(void)\n{',
            'static void port_sema_reset(void)\n{\n    port_display_queue_reset();')
    replace('void port_gcm_contract_begin(u32 ea) {',
            'void port_gcm_contract_begin(u32 ea) {\n    port_display_queue_reset();')
    # E940/E944 queue scanout for a head. E920/E924 actually flip it.
    anchor = '                } else if (mfull == 0xE920u || mfull == 0xE924u) {'
    replace(anchor, '''                } else if (mfull == 0xE940u || mfull == 0xE944u) {
                    const u32 buffer = vm_read32(dea);
                    if (buffer < CELL_GCM_MAX_DISPLAY_BUFFER_NUM) {
                        extern void port_rsx_display_queued(u32, u32);
                        port_renderer_submit_pending();
                        if (mfull == 0xE944u) port_display_packet_ready(buffer);
                        port_rsx_display_queued((mfull - 0xE940u) / 4, buffer);
                    }
''' + anchor)
    return text


def prepare_semaphore_wait(text):
    """Dispatch PFIFO semaphore methods in order, including packed packets."""
    start = text.index('            /* NV406E semaphore: OFFSET')
    end = text.index('            int port_flip_boundary = 0;', start)
    if 'getenv("GCM_SEMA_ACQUIRE")' not in text[start:end]:
        raise ValueError('Pinned optional FIFO semaphore wait changed')
    text = text[:start] + text[end:]

    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned FIFO semaphore anchor changed: ' + old[:70])
        text = text.replace(old, new)

    replace('static u32 s_sema_offset = 0;', '''/* GET stays at the packet header while an argument is waiting. The FIFO
 * owner retains its method index so earlier arguments run exactly once. */
static u32 s_sema_resume_arg;
static int s_sema_waiting;
static u32 s_sema_offset = 0;''')
    # Recycling command storage does not reset the channel's registers.
    replace('static void port_fifo_wrap_reset(void);',
            'static void port_fifo_wrap_reset(void);\nstatic void port_sema_reset(void);')
    anchor = 'static void port_fifo_wrap_reset(void) { memset(&port_fifo_wrap,0,sizeof(port_fifo_wrap)); }'
    replace(anchor, anchor + '''
static void port_sema_reset(void)
{
    s_sema_offset = 0;
    s_sema_resume_arg = 0;
    s_sema_waiting = 0;
}''')
    init = '    port_fifo_wrap_reset();\n    port_gcm_report_reset();'
    if text.count(init) != 2:
        raise ValueError('Pinned GCM init and shutdown anchors changed')
    text = text.replace(init, '    port_sema_reset();\n' + init)
    replace('if (s_fifo_getoff == last && s_fifo_getoff != put)',
            'if (!s_sema_waiting && s_fifo_getoff == last && s_fifo_getoff != put)')
    # Libgcm installs its handler through the HLE API. A low-level RSX
    # context instead has a driver event queue and runs its own interrupt handler.
    replace('                    rsx_raise_user_cmd(vm_read32(dea));', '''
                    const u32 cause = vm_read32(dea);
                    if (s_user_handler_opd || !s_gcm_syscall_mode)
                        cellGcmQueueUserCommand(cause);
                    else
                        rsx_raise_user_cmd(cause);'''.lstrip('\n'))
    for reset in ('    s_fifo_getoff  = get;', '    s_fifo_getoff=0; s_fifo_calloff=0;'):
        replace(reset, reset + '\n    s_sema_resume_arg = 0;\n    s_sema_waiting = 0;')
    replace('            int port_flip_boundary = 0;\n            for (u32 i = 0; i < count; i++) {',
            '''            int port_flip_boundary = 0;
            for (u32 i = s_sema_resume_arg; i < count; i++) {''')
    insert = '                /* GCM_SET_USER_COMMAND is method 0xEB00'
    replace(insert, '''                if (subch == 0 && m == 0x64u) {
                    s_sema_offset = vm_read32(dea);
                    continue;
                }
                if (subch == 0 && (m == 0x68u || m == 0x6Cu)) {
                    const u32 address = GCM_LABEL_GUEST_BASE + (s_sema_offset & 0xFFFFu);
                    const u32 value = vm_read32(dea);
                    if (m == 0x6Cu) {
                        port_renderer_submit_pending();
                        vm_write32(address, value);
                    } else {
                        const u32 observed = vm_read32(address);
                        if (observed != value) {
                            static unsigned waits_logged;
                            if (!s_sema_waiting && waits_logged < 12) {
                                ++waits_logged;
                                fprintf(stderr, "[fifo-semaphore] get=%08X arg=%u label=%08X "
                                        "observed=%08X expected=%08X\\n",
                                        s_fifo_getoff, i, address, observed, value);
                            }
                            s_sema_resume_arg = i;
                            s_sema_waiting = 1;
                            break;
                        }
                        s_sema_waiting = 0;
                    }
                    continue;
                }
''' + insert)
    tail = '            s_fifo_getoff += 4 + count * 4;\n            if (port_flip_boundary)'
    replace(tail, '''            if (s_sema_waiting) {
                why = "semaphore-acquire";
                break;
            }
            s_sema_resume_arg = 0;
''' + tail)
    return text


def prepare(sdk, output):
    source=sdk/'libs/video/cellGcmSys.c'
    text=source.read_text(encoding='utf-8')
    for signature in ('s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset)',
                      's32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size)'):
        anchor = signature + '\n{'
        if text.count(anchor) != 1:
            raise ValueError('Pinned GCM user mapping changed: ' + signature)
        text = text.replace(anchor, anchor + '''
    if (port_invalid_user_mapping(ea, size))
        return CELL_GCM_ERROR_INVALID_VALUE;''')
    text = '#include "runtime/service_memory.h"\n' + text
    old='    /* Approximate: increment each time queried (games use this for timing) */\n    return s_vblank_count++;'
    if text.count(old)!=1: raise ValueError('Pinned vblank getter changed')
    text=text.replace(old,'    /* Display ticks advance the counter; polling only observes it. */\n    return s_vblank_count;')
    text+=r'''
/* Polling must not advance guest time. No ticker or guest callback is started. */
int port_check_vblank_counter(void)
{
    u32 saved=s_vblank_count;
    long pending=(long)GCM_PENDING_TAKE();
    s_vblank_count=42;
    int stable=1;
    for(unsigned i=0;i<1024;i++) if(cellGcmGetVBlankCount()!=42) stable=0;
    cellGcmTickVBlank();
    int tick=cellGcmGetVBlankCount()==43 && cellGcmGetVBlankCount()==43;
    cellGcm_request_tick();
    int request=cellGcmGetVBlankCount()==44 && cellGcmGetVBlankCount()==44;
    s_vblank_count=0xFFFFFFFFu;cellGcmTickVBlank();
    int wrap=cellGcmGetVBlankCount()==0 && cellGcmGetVBlankCount()==0;
    s_vblank_count=saved;GCM_PENDING_TAKE();GCM_PENDING_SET(pending);
    int ok=stable && tick && request && wrap;
    fprintf(stderr,"[compat-check] vblank polling=%d tick=%d request=%d wrap=%d %s\n",
            stable,tick,request,wrap,ok?"PASS":"FAIL");
    return ok?0:1;
}
'''
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
    /* libgcm queues head 1 here. SetFlipImmediate consumes the returned
     * queue index later, after the FIFO has finished this buffer. */
    s32 slot = port_prepare_display_slot(bufferId);
    if (slot < 0) return slot;
    vm_write32(current,0x0004E944u);
    vm_write32(current+4,bufferId);
    vm_write32(context+8,current+8);
    u32 active_default=s_gcm_ctx_out_ea ? vm_read32(s_gcm_ctx_out_ea) : s_gcm_context_ea;
    { static unsigned trace_count;
      if(getenv("GCM_CTXDBG") && trace_count++<16)
          fprintf(stderr,"[prepare-flip] ctx=%08X active=%08X queued=%08X put=%08X publish=%d\n",
                  context,active_default,io,vm_read32(GCM_CONTROL_GUEST_ADDR),context==active_default); }
    if(context==active_default) vm_write32(GCM_CONTROL_GUEST_ADDR,io+8);
    return slot;
}
'''+text[end:]
    text='void port_renderer_submit_pending(void);\nstatic const void* s_port_gcm_render_state;\n'+text
    anchor='    static rsx_state s_state;'
    if text.count(anchor)!=1: raise ValueError('Pinned FIFO render state changed')
    text=text.replace(anchor,anchor+'\n    s_port_gcm_render_state=&s_state;')
    # Drakengard's libgcm uses subchannel 1 for NV0039 report transfers.
    # Its FORMAT at 0x324 aliases NV4097 COLOR_MASK: sending both engines
    # through the 3D decoder turns a transfer format of 0x101 into G/B writes.
    start=text.index('                /* The subchannel is a binding slot, not an engine selector:')
    end=text.index('                /* GCM_SUBCH_GATE=',start)
    original=text[start:end]
    if 'static int s1_2d = -1;' not in original:
        raise ValueError('Pinned legacy subchannel-1 routing changed')
    text=text[:start]+'''                /* This title uses libgcm's default engine bindings: subchannel
                 * 0 is NV4097, subchannel 1 is NV0039. Keep transfer registers
                 * out of the 3D state; they overlap alpha, blend and colour
                 * mask registers. Transfers still go to the existing 2D
                 * dispatcher, including its explicit unsupported diagnostics.
                 * This does not implement NV0039 copies or assume they succeed.
                 */
'''+text[end:]
    old='} else if (subch == 0 || (subch == 1 && !s1_2d)) {'
    if text.count(old)!=1: raise ValueError('Pinned FIFO engine split changed')
    text=text.replace(old,'} else if (subch == 0) {')
    old='''        if (begin && end > begin && cur >= begin && cur + GCM_RECYCLE_SLACK >= end
                && head_consumed && s_title_overran) {'''
    if text.count(old)!=1: raise ValueError('Pinned ring recycle guard changed')
    text=text.replace(old,'''        /* The write head can include unpublished commands. A flip also leaves
         * the next frame queued on purpose. Never teleport GET past either:
         * all produced bytes must have been published AND actually consumed. */
        if (begin && end > begin && cur >= begin && cur + GCM_RECYCLE_SLACK >= end
                && !port_fifo_wrap.context && head_consumed && s_title_overran && s_fifo_getoff == put
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
    text=resolve_local_includes(text, source, sdk)
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
void port_gcm_contract_render_state(u32 out[8]) {
    const rsx_state* st=(const rsx_state*)s_port_gcm_render_state;
    if(!st) { memset(out,0,8*sizeof(u32));return; }
    out[0]=st->color_mask;out[1]=st->blend_enable;
    out[2]=st->blend_sfactor;out[3]=st->blend_dfactor;
    out[4]=st->blend_equation;out[5]=st->blend_color;
    out[6]=st->alpha_ref;out[7]=st->stencil_test_enable;
}
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
    text=prepare_nv0039(text)
    recycle_start=text.index('void cellGcm_fifo_recycle(u32 ctx_ea)')
    recycle_end=text.index('\n/* A host that brings its backend up',recycle_start)
    if 'spins < 2000' not in text[recycle_start:recycle_end]:
        raise ValueError('Pinned FIFO callback changed')
    recycle=(Path(__file__).resolve().parents[1]/'port/gcm_fifo_recycle.inc').read_text(encoding='utf-8')
    text=text[:recycle_start]+recycle+'\n'+text[recycle_end:]
    jump='              if (tgt == s_fifo_getoff) {'
    if text.count(jump)!=1:
        raise ValueError('Pinned FIFO wrap acknowledgement changed')
    wrap_anchor='static u32 s_fifo_getoff  = 0;'
    if text.count(wrap_anchor)!=1:
        raise ValueError('Pinned FIFO cursor changed')
    text=text.replace(wrap_anchor,'''/* Protected by s_gcm_fifo_lock, like the FIFO cursor. */
static struct {
    u32 context,current,end,from,to,passes,last_start;
    const char* last_stop;
    int jump_published,acknowledged;
} port_fifo_wrap;
static void port_fifo_wrap_reset(void) { memset(&port_fifo_wrap,0,sizeof(port_fifo_wrap)); }

'''+wrap_anchor)
    text=text.replace(jump,'''              if(port_fifo_wrap.jump_published && s_fifo_getoff==port_fifo_wrap.from &&
                 tgt==port_fifo_wrap.to && put==tgt && tgt!=s_fifo_getoff)
                  port_fifo_wrap.acknowledged=1;
'''+jump+'\n                  why = "jump-park";')
    publish='    g_gcm_fifo_drained_ea = gcm_io2ea(s_fifo_getoff);'
    if text.count(publish)!=1:
        raise ValueError('Pinned FIFO progress publication changed')
    text=text.replace(publish,'''    if(port_fifo_wrap.context) {
        port_fifo_wrap.passes++;
        port_fifo_wrap.last_start=start_off;
        port_fifo_wrap.last_stop=budget<=0 && s_fifo_getoff!=put ? "budget" : why;
        /* Keep advancing a pending wrap even if its caller timed out.
         * PUT and the producer cursor must still belong to this request. */
        if(!port_fifo_wrap.jump_published && s_fifo_getoff==port_fifo_wrap.from &&
           put==port_fifo_wrap.from && vm_read32(GCM_CONTROL_GUEST_ADDR)==put &&
           vm_read32(port_fifo_wrap.context+4)==port_fifo_wrap.current &&
           vm_read32(port_fifo_wrap.context+8)==port_fifo_wrap.current &&
           gcm_io2ea(put)==port_fifo_wrap.current &&
           vm_read32(port_fifo_wrap.current)==(0x20000000u|port_fifo_wrap.to)) {
            port_fifo_wrap.jump_published=1;
            put=port_fifo_wrap.to;
            vm_write32(GCM_CONTROL_GUEST_ADDR,put);
        }
    }
'''+publish)
    report_start=text.index('                    /* NV4097_GET_REPORT:')
    report_end=text.index('\n                } else\n                    gcm_2d_method', report_start)
    if '0xFFFFu : 0u' not in text[report_start:report_end]:
        raise ValueError('Pinned synthetic report handler changed')
    text=text[:report_start]+'                    port_gcm_report_method(m, vm_read32(dea));'+text[report_end:]
    report_anchor='/* NV0039 byte-column copies into ordinary guest memory.'
    report_reset='    port_nv0039_reset();\n'
    if text.count(report_anchor)!=1 or text.count(report_reset)!=2:
        raise ValueError('Pinned GCM report placement changed')
    report=(Path(__file__).resolve().parents[1]/'port/gcm_zpass.inc').read_text(encoding='utf-8')
    text=text.replace(report_anchor,report+'\n'+report_anchor)
    text=text.replace('    port_nv0039_reset();\n    memset(s_report_data,',
                      '    port_fifo_wrap_reset();\n    port_gcm_report_reset();\n    port_nv0039_reset();\n    memset(s_report_data,')
    text='static void port_fifo_wrap_reset(void);\nstatic void port_gcm_report_reset(void);\n'+text
    text = prepare_semaphore_wait(text)
    text = prepare_fifo_constant_stream(text)
    text = prepare_display_queue(text, source, sdk, output)
    target=output/'cellGcmSys_compat.c'
    if not target.exists() or target.read_text(encoding='utf-8')!=text:
        target.write_text(text,encoding='utf-8')
