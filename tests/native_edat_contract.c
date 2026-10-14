/* Synthetic format contract checks. No retail keys or game data in fixtures. */
#include "edat_compat.c" /* Build-local SDK copy; exposes its tested primitives. */

static const uint8_t fixture_key[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
static const uint8_t plaintext[16] = "synthetic-data!";

static void put32(uint8_t* p, uint32_t v)
{ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }

static int fixture(uint32_t flags)
{
    uint8_t blob[0x130] = {0};
    memcpy(blob,"NPD\0",4); put32(blob+4,3); put32(blob+8,3);
    for (int i=0;i<16;i++) blob[0x40+i]=(uint8_t)(i+1);
    put32(blob+0x80,flags); put32(blob+0x84,16); put32(blob+0x8C,16);
    uint8_t auth[16], mac[20], bk[16]={0}, kres[16], hseed[16], rk[176];
    for(int i=0;i<16;i++) auth[i]=fixture_key[i]^NP_OMAC_KEY_2[i];
    aes128_cmac(auth,blob,0x60,blob+0x60);
    memcpy(bk,blob+0x60,12); aes128_expand(fixture_key,rk);
    aes128_encrypt_block(rk,bk,kres);
    if(flags & EDAT_FLAG_0x10) aes128_encrypt_block(rk,kres,hseed);
    else memcpy(hseed,kres,16);
    uint8_t ckey[16], hkey[20]={0}, zero[16]={0};
    if(flags & EDAT_ENCRYPTED_KEY_FLAG) {
        aes128_cbc_decrypt(EDAT_KEY_0,zero,kres,ckey,16);
        aes128_cbc_decrypt(EDAT_KEY_0,zero,hseed,hkey,16);
    } else { memcpy(ckey,kres,16); memcpy(hkey,hseed,16); }
    const size_t meta_size = flags & EDAT_FLAG_0x20 ? 32 : 16;
    uint8_t input[16];
    for(int i=0;i<16;i++) input[i]=plaintext[i]^blob[0x40+i];
    aes128_expand(ckey,rk); aes128_encrypt_block(rk,input,blob+0x100+meta_size);
    if(flags & EDAT_FLAG_0x10) hmac_sha1(hkey,20,blob+0x100+meta_size,16,mac);
    else aes128_cmac(hkey,blob+0x100+meta_size,16,mac);
    memcpy(blob+0x100,mac,16);
    FILE* f=fopen("fixture.edat","wb"); if(!f) return 1;
    int bad=fwrite(blob,1,0x100+meta_size+16,f)!=0x100+meta_size+16;
    fclose(f); return bad;
}

static int output_exists(void)
{ FILE* f=fopen("decoded.bin","rb"); if(!f) return 0; fclose(f); return 1; }

int main(void)
{
    unsigned passed=0;
    const uint32_t flags[2]={0,0x3C};
    for(unsigned n=0;n<2;n++) {
        remove("decoded.bin"); if(fixture(flags[n])) return 1;
        if(edat_decrypt_file_with_klicensee("fixture.edat","decoded.bin",fixture_key)) return 2;
        uint8_t actual[17]={0}; FILE* f=fopen("decoded.bin","rb"); if(!f) return 3;
        size_t got=fread(actual,1,sizeof actual,f); fclose(f);
        if(got!=16 || memcmp(actual,plaintext,16)) return 4;
        passed++;
    }
    remove("decoded.bin");
    uint8_t wrong[16]={0};
    if(edat_decrypt_file_with_klicensee("fixture.edat","decoded.bin",wrong)!=-13 || output_exists()) return 5;
    passed++;
    FILE* f=fopen("fixture.edat","r+b"); if(!f) return 6;
    fseek(f,0x120,SEEK_SET); int byte=fgetc(f); fseek(f,0x120,SEEK_SET); fputc(byte^1,f); fclose(f);
    if(edat_decrypt_file_with_klicensee("fixture.edat","decoded.bin",fixture_key)!=-11 || output_exists()) return 7;
    passed++;
    if(fixture(0x3C)) return 8;
    f=fopen("fixture.edat","r+b"); if(!f) return 9;
    fseek(f,0x0B,SEEK_SET); fputc(2,f); fclose(f);
    if(edat_decrypt_file_with_klicensee("fixture.edat","decoded.bin",fixture_key)!=-14 || output_exists()) return 10;
    passed++;
    if(edat_decrypt_file_with_klicensee("fixture.edat","decoded.bin",NULL)!=-14 || output_exists()) return 11;
    passed++;
    printf("EDAT contract: %u/6 PASS (CMAC, wrapped HMAC, wrong key, corruption, license boundary, absent key)\n",passed);
    return passed==6 ? 0 : 12;
}
