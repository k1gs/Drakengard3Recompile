/* Optional PIX capture ABI, matching microsoft/PixEvents include/pix3_win.h.
 * No PIX link dependency: ordinary builds/runs do not load instrumentation. */
typedef HRESULT (WINAPI *PortPixCaptureNextFrame)(PCWSTR,UINT32);
static HMODULE port_pix_module;
static PortPixCaptureNextFrame port_pix_capture_next;
static int port_pix_requested;
static void port_pix_initialize(void)
{
    wchar_t path[32768];
    DWORD n=GetEnvironmentVariableW(L"PORT_PIX_GPU_CAPTURER",path,32768);
    if(!n) return;
    if(n>=32768 || !((path[0] && path[1]==L':' && (path[2]==L'\\' || path[2]==L'/')) ||
                     (path[0]==L'\\' && path[1]==L'\\'))) {
        fprintf(stderr,"[PIX] capturer path must be absolute\n");exit(2);
    }
    port_pix_module=GetModuleHandleW(L"WinPixGpuCapturer.dll");
    if(!port_pix_module) port_pix_module=LoadLibraryExW(path,NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!port_pix_module) {
        fprintf(stderr,"[PIX] capturer load failed error=%lu\n",GetLastError());exit(2);
    }
    port_pix_capture_next=(PortPixCaptureNextFrame)GetProcAddress(port_pix_module,"CaptureNextFrame");
    if(!port_pix_capture_next) {fprintf(stderr,"[PIX] CaptureNextFrame export unavailable\n");exit(2);}
    typedef HRESULT (WINAPI *SetHud)(UINT);
    SetHud hide=(SetHud)GetProcAddress(port_pix_module,"SetHUDOptions");
    if(hide) hide(4); /* PIX_HUD_SHOW_ON_NO_WINDOWS */
    /* Keep instrumentation loaded until process exit, after all D3D objects. */
    fprintf(stderr,"[PIX] capturer loaded before D3D12 initialization\n");
}
static void port_pix_poll_capture(void)
{
    if(!port_pix_capture_next || port_pix_requested) return;
    wchar_t trigger[32768],file[32768];
    DWORD n=GetEnvironmentVariableW(L"PORT_PIX_CAPTURE_TRIGGER",trigger,32768);
    if(!n || n>=32768 || GetFileAttributesW(trigger)==INVALID_FILE_ATTRIBUTES) return;
    port_pix_requested=1;
    n=GetEnvironmentVariableW(L"PORT_PIX_CAPTURE_FILE",file,32768);
    if(!n || n>=32768) {fprintf(stderr,"[PIX] capture filename missing\n");return;}
    HRESULT hr=port_pix_capture_next(file,1);
    fprintf(stderr,"[PIX] one Present-to-Present frame requested hr=%08lX\n",(unsigned long)hr);
}
