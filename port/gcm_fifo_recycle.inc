/* Only movement toward the owned tail can extend the stall deadline.
 * A slow drain can outlive several stall intervals while still making
 * progress. Returning then lets guest callers proceed without room. */
typedef struct {
    ULONGLONG last_progress;
    u32 furthest_ea;
} PortFifoWait;

static int port_fifo_wait_expired(PortFifoWait* wait,ULONGLONG now,u32 progress_ea,u32 stall_ms)
{
    if(progress_ea>wait->furthest_ea) {
        wait->furthest_ea=progress_ea;
        wait->last_progress=now;
    }
    return now-wait->last_progress>=stall_ms;
}

/* The producer can reuse the head only after the FIFO owner has taken its
 * exact wrap jump. A failed callback retains the outstanding wrap for retry. */
static s32 port_fifo_recycle(u32 ctx_ea,u32 stall_ms)
{
    if(!ctx_ea)return CELL_GCM_ERROR_INVALID_VALUE;
    AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    const u32 begin=vm_read32(ctx_ea),end=vm_read32(ctx_ea+4),current=vm_read32(ctx_ea+8);
    const u32 to=gcm_ea2io(begin),from=gcm_ea2io(current);
    if(!begin || end<=begin || current<begin || current>end ||
        ((begin|current)&3u) || to>0x1FFFFFFCu || from==0xFFFFFFFFu ||
        gcm_io2ea(from)!=current || gcm_io2ea(to)!=begin) {
        ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
        return CELL_GCM_ERROR_INVALID_VALUE;
    }
    if(port_fifo_wrap.context) {
        if(port_fifo_wrap.context!=ctx_ea || port_fifo_wrap.current!=current ||
           port_fifo_wrap.from!=from || port_fifo_wrap.to!=to || end!=current) {
            ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
            return CELL_GCM_ERROR_FAILURE;
        }
    } else if(current!=begin) {
        port_fifo_wrap.context=ctx_ea;
        port_fifo_wrap.current=current;
        port_fifo_wrap.end=end;
        port_fifo_wrap.from=from;
        port_fifo_wrap.to=to;
        /* Inline guest reservations consult end before calling the callback.
         * Reserve the jump even if a later short write follows a timeout. */
        vm_write32(ctx_ea+4,current);
        vm_write32(current,0x20000000u|to);
        atomic_thread_fence(memory_order_release);
        /* Drain to the tail before moving PUT back to the head. Otherwise
         * GET==PUT at the head can hide a whole unpublished tail. */
        vm_write32(GCM_CONTROL_GUEST_ADDR,from);
    } else {
        ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
        return CELL_OK;
    }
    const ULONGLONG started=GetTickCount64();
    const u32 initial_ea=gcm_io2ea(s_fifo_getoff);
    PortFifoWait wait={started,initial_ea>=begin && initial_ea<=current ? initial_ea : begin};
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock);

    for(;;) {
        AcquireSRWLockExclusive(&s_gcm_fifo_lock);
        const u32 get=s_fifo_getoff,put=vm_read32(GCM_CONTROL_GUEST_ADDR);
        const int owned=port_fifo_wrap.context==ctx_ea && port_fifo_wrap.current==current &&
            port_fifo_wrap.from==from && port_fifo_wrap.to==to &&
            vm_read32(ctx_ea+4)==current && vm_read32(ctx_ea+8)==current;
        if(owned && port_fifo_wrap.acknowledged && get==to && put==to) {
            vm_write32(ctx_ea+8,begin);
            vm_write32(ctx_ea+4,port_fifo_wrap.end);
            port_fifo_wrap_reset();
            ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
            return CELL_OK;
        }
        const u32 get_ea=gcm_io2ea(get);
        u32 progress_ea=0;
        if(owned && !port_fifo_wrap.jump_published && put==from && get_ea>=begin && get_ea<=current)
            progress_ea=get_ea;
        if(owned && port_fifo_wrap.jump_published && put==to)
            progress_ea=current;
        const ULONGLONG now=GetTickCount64();
        if(port_fifo_wait_expired(&wait,now,progress_ea,stall_ms)) {
            const u32 word=get_ea?vm_read32(get_ea):0;
            const u32 passes=port_fifo_wrap.passes,last_start=port_fifo_wrap.last_start;
            const int published=port_fifo_wrap.jump_published,acknowledged=port_fifo_wrap.acknowledged;
            const char* stop=port_fifo_wrap.last_stop;
            ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
            fprintf(stderr,"[fifo-recycle] timeout ctx=%08X current=%08X jump=%08X->%08X get=%08X put=%08X ea=%08X word=%08X passes=%u last_start=%08X stop=%s published=%d ack=%d owned=%d elapsed_ms=%llu stalled_ms=%llu\n",
                ctx_ea,current,from,to,get,put,get_ea,word,passes,last_start,stop?stop:"none",published,acknowledged,owned,
                (unsigned long long)(now-started),(unsigned long long)(now-wait.last_progress));
            return CELL_GCM_ERROR_FAILURE;
        }
        ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
        Sleep(1);
    }
}

s32 cellGcm_fifo_recycle(u32 ctx_ea)
{
    return port_fifo_recycle(ctx_ea,2000);
}

int port_check_fifo_callback_recycle(u32 ea)
{
    const u32 ctx=ea+0x8000,tail=ea+24,end=ea+0x1000,label=ppu_hle_inject_base+0xFF0;
    if(cellGcmSetDisplayBuffer(0,0,5120,1280,720))return 0;
    vm_write32(ctx,ea);vm_write32(ctx+4,end);vm_write32(ctx+8,tail);
    vm_write32(ctx+12,0);
    vm_write32(label,0);
    vm_write32(ea,0x0004E924);vm_write32(ea+4,0);
    vm_write32(ea+8,0x00041D6C);vm_write32(ea+12,0xFF0);
    vm_write32(ea+16,0x00041D70);vm_write32(ea+20,0x00660400);
    PortFifoWait stall={0,ea};
    PortFifoWait slow={0,ea};
    int progress_wait=!port_fifo_wait_expired(&stall,1900,ea+8,2000) &&
        !port_fifo_wait_expired(&stall,2000,ea,2000) &&
        !port_fifo_wait_expired(&stall,3899,ea+8,2000) &&
        port_fifo_wait_expired(&stall,3900,0,2000);
    for(unsigned step=1;step<=6;step++)
        progress_wait &= !port_fifo_wait_expired(&slow,1500ull*step,ea+4*step,2000);
    progress_wait &= !port_fifo_wait_expired(&slow,10999,tail,2000) &&
        port_fifo_wait_expired(&slow,11000,tail,2000);
    int timeout=port_fifo_recycle(ctx,0)==CELL_GCM_ERROR_FAILURE &&
        vm_read32(ctx+8)==tail && !vm_read32(label);
    /* With plenty of nominal room, this normal eight-byte producer would
     * overwrite the outstanding jump unless the pending wrap reserves it. */
    vm_write32(tail+4,0x12345678u);
    const s32 short_result=cellGcmSetPrepareFlip((void*)(uintptr_t)ctx,0);
    const int short_held=short_result==CELL_GCM_ERROR_FAILURE &&
        vm_read32(ctx+8)==tail && vm_read32(tail)==0x20000000u &&
        vm_read32(tail+4)==0x12345678u && vm_read32(GCM_CONTROL_GUEST_ADDR)==24;
    if(!short_held) {
        fprintf(stderr,"[compat-check] FIFO short prepare-flip overwrote pending tail rc=%08X current=%08X word=%08X FAIL\n",
            (u32)short_result,vm_read32(ctx+8),vm_read32(tail));
        return 0;
    }
    cellGcm_rsx_process_fifo();
    int after_flip=port_fifo_recycle(ctx,0)==CELL_GCM_ERROR_FAILURE &&
        vm_read32(ctx+8)==tail && !vm_read32(label);
    /* A changed write head is not owned by this wrap request. */
    vm_write32(ctx+8,tail+4);
    cellGcm_rsx_process_fifo();
    int ownership_held=!port_fifo_wrap.jump_published && vm_read32(GCM_CONTROL_GUEST_ADDR)==24;
    vm_write32(ctx+8,tail);
    cellGcm_rsx_process_fifo();
    int drained=port_fifo_wrap.jump_published && !port_fifo_wrap.acknowledged &&
        vm_read32(ctx+4)==tail && vm_read32(ctx+8)==tail && vm_read32(label)==0x466;
    cellGcm_rsx_process_fifo();
    int recycled=port_fifo_wrap.acknowledged && vm_read32(ctx+4)==tail && vm_read32(ctx+8)==tail &&
        port_fifo_recycle(ctx,0)==CELL_OK && vm_read32(ctx+8)==ea && vm_read32(ctx+4)==end;
    const int resumed=cellGcmSetPrepareFlip((void*)(uintptr_t)ctx,0)==0 &&
        vm_read32(ctx+8)==ea+8 && vm_read32(ea)==0x0004E924u;
    int ok=progress_wait && timeout && short_held && after_flip && ownership_held && drained && recycled && resumed;
    fprintf(stderr,"[compat-check] FIFO callback progress-wait=%d timeout=%d short-held=%d flip-tail-held=%d ownership-held=%d autonomous-publish=%d acknowledged=%d short-resumed=%d %s\n",
        progress_wait,timeout,short_held,after_flip,ownership_held,drained,recycled,resumed,ok?"PASS":"FAIL");
    return ok;
}
