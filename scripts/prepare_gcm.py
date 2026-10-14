"""Preserve pending GPU commands and expose their real IO mapping for diagnosis."""
import re


def prepare(sdk, output):
    source=sdk/'libs/video/cellGcmSys.c'
    text=source.read_text(encoding='utf-8')
    anchor='void gcm_note_spu_put(u32 ea, u32 size)\n{'
    if text.count(anchor)!=1: raise ValueError('GCM DMA trace anchor changed')
    text=text.replace(anchor,anchor+'\n    port_gpu_spu_write(ea,size);')
    text='void port_gpu_spu_write(unsigned int,unsigned int);\n'+text
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
    s_fifo_getoff=0; s_fifo_calloff=0;
    vm_write32(GCM_CONTROL_GUEST_ADDR,0); vm_write32(GCM_CONTROL_GUEST_ADDR+4,0);
}
u32 port_gcm_contract_get(void) { return s_fifo_getoff; }
'''
    target=output/'cellGcmSys_compat.c'
    if not target.exists() or target.read_text(encoding='utf-8')!=text:
        target.write_text(text,encoding='utf-8')
