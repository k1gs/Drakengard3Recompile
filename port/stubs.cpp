// No game behavior is stubbed here. Real PRX integration will be added when needed.
extern "C" void ps3_load_prx_modules(void) {}
#ifdef PORT_WITHOUT_SPU
extern "C" void drakengard_spu_register_all(void) {}
#endif
#ifdef PORT_WITHOUT_RAW_JOBS
extern "C" void drakengard_raw_jobs_register_all(void) {}
#endif
