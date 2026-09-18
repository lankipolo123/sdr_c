/* Runtime decoder for tools/obfuscate.py's XOR-encoded string literals -
 * generated/copied into the protected build only (build_obf/src/), never
 * part of the normal readable source tree. static inline so it's safe
 * to include in every translation unit without a separate .c file. */
#pragma once

static inline const char *obf_decode(char *out, const unsigned char *enc, int enc_len) {
    int i;
    for (i = 0; i < enc_len; i++) {
        out[i] = (char)(enc[i] ^ 0x5A);
    }
    out[enc_len] = '\0';
    return out;
}
