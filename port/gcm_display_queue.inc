/* libgcm gives the PPU an index into an eight-entry ring. The FIFO packet
 * holds the display buffer ID, which can differ from that index. */
enum { PORT_FLIP_SLOTS = 8 };
typedef struct {
    u32 buffer;
    int ready;
} PortPreparedDisplay;

static SRWLOCK port_display_queue_lock = SRWLOCK_INIT;
static PortPreparedDisplay port_display_slots[PORT_FLIP_SLOTS];
static unsigned port_display_read;
static unsigned port_display_write;

static unsigned port_next_display_slot(unsigned index)
{
    return (index + 1) % PORT_FLIP_SLOTS;
}

static void port_display_queue_reset(void)
{
    AcquireSRWLockExclusive(&port_display_queue_lock);
    port_display_read = port_display_write = 0;
    ReleaseSRWLockExclusive(&port_display_queue_lock);
}

/* Called after validating packet capacity and the context callback. Reserve
 * before publishing the guest's PUT, which can wake the FIFO owner. */
static s32 port_prepare_display_slot(u32 buffer)
{
    AcquireSRWLockExclusive(&port_display_queue_lock);
    unsigned slot = port_next_display_slot(port_display_write);
    if (slot == port_display_read) {
        ReleaseSRWLockExclusive(&port_display_queue_lock);
        return CELL_GCM_ERROR_FAILURE;
    }
    port_display_slots[slot] = (PortPreparedDisplay){buffer, 0};
    port_display_write = slot;
    ReleaseSRWLockExclusive(&port_display_queue_lock);
    return (s32)slot;
}

/* Commands may be copied into another command buffer. The driver packet
 * carries the buffer ID. Resolve repeated IDs in submission order. */
static void port_display_packet_ready(u32 buffer)
{
    AcquireSRWLockExclusive(&port_display_queue_lock);
    for (unsigned i = port_display_read; i != port_display_write;) {
        i = port_next_display_slot(i);
        PortPreparedDisplay* slot = &port_display_slots[i];
        if (!slot->ready && slot->buffer == buffer) {
            slot->ready = 1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&port_display_queue_lock);
}

static PortPreparedDisplay* port_find_display_slot(unsigned index)
{
    for (unsigned i = port_display_read; i != port_display_write;) {
        i = port_next_display_slot(i);
        if (i == index)
            return &port_display_slots[i];
    }
    return NULL;
}

s32 cellGcmGetDisplayBufferByFlipIndex(u32 index)
{
    AcquireSRWLockShared(&port_display_queue_lock);
    PortPreparedDisplay* slot = port_find_display_slot(index);
    s32 buffer = slot ? (s32)slot->buffer : -1;
    ReleaseSRWLockShared(&port_display_queue_lock);
    return buffer;
}

s32 cellGcmSetFlipImmediate(u32 index)
{
    AcquireSRWLockExclusive(&port_display_queue_lock);
    PortPreparedDisplay* slot = port_find_display_slot(index);
    s32 result = CELL_GCM_ERROR_FAILURE;
    if (slot && slot->ready) {
        result = cellGcmSetFlipCommand(slot->buffer);
        if (result == CELL_OK)
            port_display_read = index;
    }
    ReleaseSRWLockExclusive(&port_display_queue_lock);
    return result;
}
