/* The frame thread and asynchronous shader workers share process-start options.
 * Native renderer checks can also enter these paths before backend startup. */
typedef struct {
    int debug_pick;
    int dump_vertices;
    int log_movie_binds;
    int log_texture_formats;
    int inline_fp_constants;
    int disable_alpha_test;
    int log_blend;
    int dump_pass_limit;
    int has_selected_pass;
    int selected_pass;
    int view_pass_target;
    int save_target_alpha;
    int save_target;
} PortRendererOptions;

static const PortRendererOptions* port_renderer_options(void);

static INIT_ONCE port_options_once = INIT_ONCE_STATIC_INIT;
static volatile LONG port_options_ready;
static PortRendererOptions port_options;

static BOOL CALLBACK port_read_renderer_options(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
    const char* pick = getenv("RSX_DBG_PICK");
    const char* pass = getenv("RTT_PASS");
    const char* dump = getenv("RTT_DUMP");
    const char* constants = getenv("FP_CONSTBUF");

    port_options.debug_pick = pick ? atoi(pick) : -2;
    port_options.dump_vertices = getenv("VTX_DUMP") != NULL;
    port_options.log_movie_binds = getenv("MOVIE_BIND") != NULL;
    port_options.log_texture_formats = getenv("TEXFMTDBG") != NULL;
    port_options.inline_fp_constants = constants && constants[0] == '0';
    port_options.disable_alpha_test = getenv("NO_ALPHATEST") != NULL;
    port_options.log_blend = getenv("BLENDDBG") != NULL;
    if (dump) {
        int limit = atoi(dump);
        port_options.dump_pass_limit = limit < 2 ? 14 : limit;
    }
    port_options.has_selected_pass = pass != NULL;
    port_options.selected_pass = pass ? atoi(pass) : 0;
    port_options.view_pass_target = getenv("RTT_VIEWRT") != NULL;
    port_options.save_target_alpha = getenv("RTT_SAVEA") != NULL;
    port_options.save_target = getenv("RTT_SAVERT") != NULL;
    WriteRelease(&port_options_ready, 1);
    return TRUE;
}

static const PortRendererOptions* port_renderer_options(void)
{
    if (!ReadAcquire(&port_options_ready))
        InitOnceExecuteOnce(&port_options_once, port_read_renderer_options, NULL, NULL);
    return &port_options;
}
