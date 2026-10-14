"""Build-local EDAT adapter for the klicensee supplied to the normal guest API.

Only RAP-free license type 3 accepts this input. Existing MAC validation remains
mandatory. No keys are embedded, searched for, logged, or written to sidecars.
"""
from pathlib import Path


def prepare(sdk: Path, out: Path):
    source = sdk/'libs/filesystem/edat.c'
    text = source.read_text(encoding='utf-8')
    replacements = [
        ('int edat_decrypt_file(const char* in_path, const char* out_path)',
         'static int edat_decrypt_file_impl(const char* in_path, const char* out_path, const uint8_t* supplied)'),
        ('    if (flags & SDAT_FLAG) {\n        for (int i = 0; i < 16; i++) key[i]',
         '''    if (supplied) {
        if (license != 3 || (flags & SDAT_FLAG)) {
            fclose(in); return -14; /* never substitute klicensee for a RIF */
        }
        uint8_t ck[16], mac[16];
        for (int i = 0; i < 16; i++) ck[i] = supplied[i] ^ NP_OMAC_KEY_2[i];
        aes128_cmac(ck, hdr, 0x60, mac);
        if (memcmp(mac, dev_hash, 16) != 0) {
            fprintf(stderr, "[edat] guest-supplied klicensee verification failed\\n");
            fclose(in); return -13;
        }
        memcpy(key, supplied, 16);
        have_key = 1;
        fprintf(stderr, "[edat] guest-supplied klicensee verified (license 3)\\n");
    } else if (flags & SDAT_FLAG) {
        for (int i = 0; i < 16; i++) key[i]'''),
        ('const char* edat_resolve(const char* host_path, char* buf, size_t cap)',
         '''int edat_decrypt_file(const char* in_path, const char* out_path)
{
    return edat_decrypt_file_impl(in_path, out_path, NULL);
}

int edat_decrypt_file_with_klicensee(const char* in_path, const char* out_path, const uint8_t* supplied)
{
    if (!supplied) return -14;
    return edat_decrypt_file_impl(in_path, out_path, supplied);
}

const char* edat_resolve(const char* host_path, char* buf, size_t cap)'''),
    ]
    for old, new in replacements:
        if text.count(old) != 1:
            raise ValueError(f'EDAT SDK source changed: {old[:60]}')
        text = text.replace(old, new)
    text = text.replace('"edat.h"', '"'+(source.parent/'edat.h').resolve().as_posix()+'"')
    if not out.exists() or out.read_text(encoding='utf-8') != text:
        out.write_text(text, encoding='utf-8')
