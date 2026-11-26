/* Production display-buffer routing contract on the caller's D3D12 queue. */
int port_check_display_buffers_gpu(ID3D12CommandAllocator *allocator,
                                   ID3D12GraphicsCommandList *list, ID3D12CommandQueue *queue,
                                   ID3D12Fence *fence, HANDLE event)
{
    if (!s_d3d.device || !allocator || !list || !queue || !fence || !event)
        return 2;
    const u32 off[3] = {0, 0x10000, 0x20000};
    const float color[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    const u8 expected[4][4] = {
        {0, 0, 255, 255}, {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 255, 0, 255}};
    int slots[3] = {-1, -1, -1}, ok = 0;
    unsigned checked = 0;
    HRESULT hr = S_OK;
    ID3D12Resource *scan = NULL;
    ID3D12Resource *rb = NULL;
    ID3D12Resource *saved_rt = s_d3d.render_targets[0];
    int saved_present = s_present_this_frame;
    unsigned old_id = cellGcmGetCurrentDisplayBufferId();
    CellGcmDisplayInfo saved_display[3];
    int saved_registered[3] = {0, 0, 0};
    for (unsigned i = 0; i < 3; i++)
    {
        const CellGcmDisplayInfo *info = cellGcmGetDisplayInfo(i);
        if (info)
        {
            saved_display[i] = *info;
            saved_registered[i] = info->pitch != 0;
        }
        else
            memset(&saved_display[i], 0, sizeof(saved_display[i]));
    }
    D3D12_RESOURCE_DESC sd = {0}, bd = {0};
    D3D12_CLEAR_VALUE cv = {0};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {0};
    UINT rows = 0;
    UINT64 bytes = 0;
#define DISPLAY_GPU_TRY(x)                                                                         \
    do                                                                                             \
    {                                                                                              \
        hr = (x);                                                                                  \
        if (FAILED(hr))                                                                            \
            goto cleanup;                                                                          \
    } while (0)
    for (unsigned i = 0; i < 3; i++)
        if (cellGcmSetDisplayBuffer(i, off[i], 64, 16, 16))
            goto cleanup;
    DISPLAY_GPU_TRY(allocator->lpVtbl->Reset(allocator));
    DISPLAY_GPU_TRY(list->lpVtbl->Reset(list, allocator, NULL));
    s_d3d.cmd_list = list;
    for (unsigned i = 0; i < 3; i++)
    {
        slots[i] = off_rt_get(off[i], port_color_address(0xFEED0000u, off[i]), 64, 16, 16, 0x108);
        if (slots[i] < 0)
            goto cleanup;
        off_rt_transition(slots[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->lpVtbl->ClearRenderTargetView(list, off_rt_rtv(slots[i]), color[i], 0, NULL);
        off_rt_transition(slots[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    sd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    sd.Width = 16;
    sd.Height = 16;
    sd.DepthOrArraySize = 1;
    sd.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    sd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    cv.Format = sd.Format;
    DISPLAY_GPU_TRY(s_d3d.device->lpVtbl->CreateCommittedResource(
        s_d3d.device, &(D3D12_HEAP_PROPERTIES){D3D12_HEAP_TYPE_DEFAULT}, D3D12_HEAP_FLAG_NONE, &sd,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void **)&scan));
    s_d3d.render_targets[0] = scan;
    s_present_this_frame = 1;
    s_d3d.device->lpVtbl->GetCopyableFootprints(s_d3d.device, &sd, 0, 1, 0, &fp, &rows, NULL,
                                                &bytes);
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)fp.Footprint.RowPitch * 16 * 4;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DISPLAY_GPU_TRY(s_d3d.device->lpVtbl->CreateCommittedResource(
        s_d3d.device, &(D3D12_HEAP_PROPERTIES){D3D12_HEAP_TYPE_READBACK}, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb));
    const unsigned order[4] = {2, 0, 1, 1};
    for (unsigned n = 0; n < 4; n++)
    {
        if (cellGcmSetFlipCommand(order[n]))
            goto cleanup;
        composite_present(0);
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = scan;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->lpVtbl->ResourceBarrier(list, 1, &b);
        D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
        src.pResource = scan;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        dst.PlacedFootprint.Offset = (UINT64)n * fp.Footprint.RowPitch * 16;
        list->lpVtbl->CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->lpVtbl->ResourceBarrier(list, 1, &b);
    }
    DISPLAY_GPU_TRY(list->lpVtbl->Close(list));
    {
        ID3D12CommandList *lists[] = {(ID3D12CommandList *)list};
        queue->lpVtbl->ExecuteCommandLists(queue, 1, lists);
    }
    UINT64 signal = fence->lpVtbl->GetCompletedValue(fence) + 1;
    DISPLAY_GPU_TRY(queue->lpVtbl->Signal(queue, fence, signal));
    DISPLAY_GPU_TRY(fence->lpVtbl->SetEventOnCompletion(fence, signal, event));
    if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
        goto cleanup;
    {
        u8 *p = NULL;
        D3D12_RANGE rr = {0, (SIZE_T)bd.Width};
        DISPLAY_GPU_TRY(rb->lpVtbl->Map(rb, 0, &rr, (void **)&p));
        ok = 1;
        for (unsigned n = 0; n < 4; n++)
            for (unsigned y = 0; y < 16; y++)
                for (unsigned x = 0; x < 16; x++)
                {
                    const u8 *got = p + (size_t)n * fp.Footprint.RowPitch * 16 +
                                    (size_t)y * fp.Footprint.RowPitch + x * 4;
                    checked++;
                    if (memcmp(got, expected[n], 4))
                    {
                        if (ok)
                            fprintf(stderr,
                                    "[compat-check] display buffers FAIL plane=%u xy=%u,%u "
                                    "rgba=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                                    n, x, y, got[0], got[1], got[2], got[3], expected[n][0],
                                    expected[n][1], expected[n][2], expected[n][3]);
                        ok = 0;
                    }
                }
        rb->lpVtbl->Unmap(rb, 0, NULL);
    }
cleanup:
    if (s_d3d.render_targets[0] == scan)
        s_d3d.render_targets[0] = saved_rt;
    s_present_this_frame = saved_present;
    if (scan)
        scan->lpVtbl->Release(scan);
    if (rb)
        rb->lpVtbl->Release(rb);
    for (unsigned i = 0; i < 3; i++)
        if (saved_registered[i])
            cellGcmSetDisplayBuffer(i, saved_display[i].offset, saved_display[i].pitch,
                                    saved_display[i].width, saved_display[i].height);
    if (old_id < 3)
        cellGcmSetFlipCommand(old_id);
    if (ok)
        fprintf(stderr, "[compat-check] display buffers GPU pixels=%u PASS\n", checked);
    if (FAILED(hr))
        fprintf(stderr, "[compat-check] display buffers GPU HRESULT=%08lX FAIL\n",
                (unsigned long)hr);
    return ok ? 0 : 1;
#undef DISPLAY_GPU_TRY
}
