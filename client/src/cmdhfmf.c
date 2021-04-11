//-----------------------------------------------------------------------------
// Copyright (C) 2011,2012 Merlok
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// High frequency MIFARE commands
//-----------------------------------------------------------------------------

#include "cmdhfmf.h"
#include <ctype.h>

#include "cmdparser.h"    // command_t
#include "commonutil.h"   // ARRAYLEN
#include "comms.h"        // clearCommandBuffer
#include "fileutils.h"
#include "cmdtrace.h"
#include "mifare/mifaredefault.h"          // mifare default key array
#include "cliparser.h"          // argtable
#include "hardnested_bf_core.h" // SetSIMDInstr
#include "mifare/mad.h"
#include "mifare/ndef.h"
#include "protocols.h"
#include "util_posix.h"         // msclock
#include "cmdhfmfhard.h"
#include "des.h"                // des ecb
#include "crapto1/crapto1.h"    // prng_successor
#include "cmdhf14a.h"           // exchange APDU
#include "crypto/libpcrypto.h"

#define MFBLOCK_SIZE 16

#define MIFARE_4K_MAXBLOCK 256
#define MIFARE_2K_MAXBLOCK 128
#define MIFARE_1K_MAXBLOCK 64
#define MIFARE_MINI_MAXBLOCK 20

#define MIFARE_MINI_MAXSECTOR 5
#define MIFARE_1K_MAXSECTOR 16
#define MIFARE_2K_MAXSECTOR 32
#define MIFARE_4K_MAXSECTOR 40

static int CmdHelp(const char *Cmd);

/*
 * static int usage_hf14_sniff(void) {
    PrintAndLogEx(NORMAL, "It continuously gets data from the field and saves it to: log, emulator, emulator file.");
    PrintAndLogEx(NORMAL, "Usage:  hf mf sniff [h] [l] [d] [f]");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "      h    this help");
    PrintAndLogEx(NORMAL, "      l    save encrypted sequence to logfile `uid.log`");
    PrintAndLogEx(NORMAL, "      d    decrypt sequence and put it to log file `uid.log`");
//  PrintAndLogEx(NORMAL, " n/a  e     decrypt sequence, collect read and write commands and save the result of the sequence to emulator memory");
    PrintAndLogEx(NORMAL, "      f    decrypt sequence, collect read and write commands and save the result of the sequence to emulator dump file `uid.eml`");
    PrintAndLogEx(NORMAL, "Example:");
    PrintAndLogEx(NORMAL, _YELLOW_("           hf mf sniff l d f"));
    return PM3_SUCCESS;
}
*/
static int usage_hf14_hardnested(void) {
    PrintAndLogEx(NORMAL, "Usage:");
    PrintAndLogEx(NORMAL, "      hf mf hardnested <block number> <key A|B> <key (12 hex symbols)>");
    PrintAndLogEx(NORMAL, "                       <target block number> <target key A|B> [known target key (12 hex symbols)] [w] [s]");
    PrintAndLogEx(NORMAL, "  or  hf mf hardnested r [known target key]");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "      h         this help");
    PrintAndLogEx(NORMAL, "      w         acquire nonces and UID, and write them to binary file with default name hf-mf-<UID>-nonces.bin");
    PrintAndLogEx(NORMAL, "      s         slower acquisition (required by some non standard cards)");
    PrintAndLogEx(NORMAL, "      r         read hf-mf-<UID>-nonces.bin if tag present, otherwise read nonces.bin, then start attack");
    PrintAndLogEx(NORMAL, "      u <UID>   read/write hf-mf-<UID>-nonces.bin instead of default name");
    PrintAndLogEx(NORMAL, "      f <name>  read/write <name> instead of default name");
    PrintAndLogEx(NORMAL, "      t         tests?");
    PrintAndLogEx(NORMAL, "      i <X>     set type of SIMD instructions. Without this flag programs autodetect it.");
#if defined(COMPILER_HAS_SIMD_AVX512)
    PrintAndLogEx(NORMAL, "        i 5   = AVX512");
#endif
#if defined(COMPILER_HAS_SIMD)
    PrintAndLogEx(NORMAL, "        i 2   = AVX2");
    PrintAndLogEx(NORMAL, "        i a   = AVX");
    PrintAndLogEx(NORMAL, "        i s   = SSE2");
    PrintAndLogEx(NORMAL, "        i m   = MMX");
#endif
    PrintAndLogEx(NORMAL, "        i n   = none (use CPU regular instruction set)");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf hardnested 0 A FFFFFFFFFFFF 4 A"));
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf hardnested 0 A FFFFFFFFFFFF 4 A w"));
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf hardnested 0 A FFFFFFFFFFFF 4 A f nonces.bin w s"));
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf hardnested r"));
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf hardnested r a0a1a2a3a4a5"));
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Add the known target key to check if it is present in the remaining key space:");
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf hardnested 0 A A0A1A2A3A4A5 4 A FFFFFFFFFFFF"));
    return PM3_SUCCESS;
}

static int usage_hf14_autopwn(void) {
    PrintAndLogEx(NORMAL, "Usage:");
    PrintAndLogEx(NORMAL, "      hf mf autopwn [k] <sector number> <key A|B> <key (12 hex symbols)>");
    PrintAndLogEx(NORMAL, "                    [* <card memory>] [f <dictionary>[.dic]] [s] [i <simd type>] [l] [v]");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Description:");
    PrintAndLogEx(NORMAL, "      This command automates the key recovery process on MIFARE Classic cards.");
    PrintAndLogEx(NORMAL, "      It uses the darkside, nested, hardnested and staticnested to recover keys.");
    PrintAndLogEx(NORMAL, "      If all keys are found, try dumping card content.");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "      h                          this help");
    PrintAndLogEx(NORMAL, "      k <sector> <key A|B> <key> known key is supplied");
    PrintAndLogEx(NORMAL, "      f <dictionary>[.dic]       key dictionary file");
    PrintAndLogEx(NORMAL, "      s                          slower acquisition for hardnested (required by some non standard cards)");
    PrintAndLogEx(NORMAL, "      v                          verbose output (statistics)");
    PrintAndLogEx(NORMAL, "      l                          legacy mode (use the slow 'mf chk' for the key enumeration)");
    PrintAndLogEx(NORMAL, "      * <card memory>            all sectors based on card memory");
    PrintAndLogEx(NORMAL, "        * 0   = MINI(320 bytes)");
    PrintAndLogEx(NORMAL, "        * 1   = 1k  (default)");
    PrintAndLogEx(NORMAL, "        * 2   = 2k");
    PrintAndLogEx(NORMAL, "        * 4   = 4k");
    PrintAndLogEx(NORMAL, "      i <simd type>              set type of SIMD instructions for hardnested. Default: autodetection.");
#if defined(COMPILER_HAS_SIMD_AVX512)
    PrintAndLogEx(NORMAL, "        i 5   = AVX512");
#endif
#if defined(COMPILER_HAS_SIMD)
    PrintAndLogEx(NORMAL, "        i 2   = AVX2");
    PrintAndLogEx(NORMAL, "        i a   = AVX");
    PrintAndLogEx(NORMAL, "        i s   = SSE2");
#endif
    PrintAndLogEx(NORMAL, "        i m   = MMX");
    PrintAndLogEx(NORMAL, "        i n   = none (use CPU regular instruction set)");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf autopwn")"                                             -- target MIFARE Classic card with default keys");
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf autopwn * 1 f mfc_default_keys")"                      -- target MIFARE Classic card (size 1k) with default dictionary");
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf autopwn k 0 A FFFFFFFFFFFF")"                          -- target MIFARE Classic card with Sector0 typeA with known key 'FFFFFFFFFFFF'");
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf autopwn k 0 A FFFFFFFFFFFF * 1 f mfc_default_keys")"   -- this command combines the two above (reduce the need for nested / hardnested attacks, by using a dictionary)");
    return PM3_SUCCESS;
}

/*
static int usage_hf14_keybrute(void) {
    PrintAndLogEx(NORMAL, "J_Run's 2nd phase of multiple sector nested authentication key recovery");
    PrintAndLogEx(NORMAL, "You have a known 4 last bytes of a key recovered with mf_nonce_brute tool.");
    PrintAndLogEx(NORMAL, "First 2 bytes of key will be bruteforced");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, " ---[ This attack is obsolete,  try hardnested instead ]---");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Usage:  hf mf keybrute [h] <block number> <A|B> <key>");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "      h               this help");
    PrintAndLogEx(NORMAL, "      <block number>  target block number");
    PrintAndLogEx(NORMAL, "      <A|B>           target key type");
    PrintAndLogEx(NORMAL, "      <key>           candidate key from mf_nonce_brute tool");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, _YELLOW_("           hf mf keybrute 1 A 000011223344"));
    return 0;
}
*/
static int usage_hf14_restore(void) {
    PrintAndLogEx(NORMAL, "Usage:   hf mf restore [card memory] u <UID> k <name> f <name> [w]");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "  [card memory]: 0 = 320 bytes (MIFARE Mini), 1 = 1K (default), 2 = 2K, 4 = 4K");
    PrintAndLogEx(NORMAL, "  u <UID>      : uid, try to restore from hf-mf-<UID>-key.bin and hf-mf-<UID>-dump.bin");
    PrintAndLogEx(NORMAL, "  k <name>     : key filename, specific the full filename of key file");
    PrintAndLogEx(NORMAL, "  f <name>     : data filename, specific the full filename of data file");
    PrintAndLogEx(NORMAL, "  w            : use specified keyfile to authenticate with");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, _YELLOW_("         hf mf restore") "                            -- read the UID from tag first, then restore from hf-mf-<UID>-key.bin and and hf-mf-<UID>-dump.bin");
    PrintAndLogEx(NORMAL, _YELLOW_("         hf mf restore 1 u 12345678") "               -- restore from hf-mf-12345678-key.bin and hf-mf-12345678-dump.bin");
    PrintAndLogEx(NORMAL, _YELLOW_("         hf mf restore 1 u 12345678 k dumpkey.bin") " -- restore from dumpkey.bin and hf-mf-12345678-dump.bin");
    PrintAndLogEx(NORMAL, _YELLOW_("         hf mf restore 4") "                          -- read the UID from tag with 4K memory first, then restore from hf-mf-<UID>-key.bin and and hf-mf-<UID>-dump.bin");
    return PM3_SUCCESS;
}
static int usage_hf14_decryptbytes(void) {
    PrintAndLogEx(NORMAL, "Decrypt Crypto-1 encrypted bytes given some known state of crypto. See tracelog to gather needed values\n");
    PrintAndLogEx(NORMAL, "Usage:   hf mf decrypt [h] <nt> <ar_enc> <at_enc> <data>");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "      h            this help");
    PrintAndLogEx(NORMAL, "      <nt>         tag nonce");
    PrintAndLogEx(NORMAL, "      <ar_enc>     encrypted reader response");
    PrintAndLogEx(NORMAL, "      <at_enc>     encrypted tag response");
    PrintAndLogEx(NORMAL, "      <data>       encrypted data, taken directly after at_enc and forward");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, _YELLOW_("         hf mf decrypt b830049b 9248314a 9280e203 41e586f9"));
    PrintAndLogEx(NORMAL, "\n  this sample decrypts 41e586f9 -> 3003999a  Annotated: 30 03 [99 9a]  read block 3 [crc]");
    return PM3_SUCCESS;
}

static int usage_hf14_csetblk(void) {
    PrintAndLogEx(NORMAL, "Set block data for magic Chinese card. Only works with magic cards");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Usage:  hf mf csetblk [h] <block number> <block data (32 hex symbols)> [w]");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "       h         this help");
    PrintAndLogEx(NORMAL, "       w         wipe card before writing");
    PrintAndLogEx(NORMAL, "       <block>   block number");
    PrintAndLogEx(NORMAL, "       <data>    block data to write (32 hex symbols)");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, _YELLOW_("       hf mf csetblk 1 01020304050607080910111213141516"));
    PrintAndLogEx(NORMAL, _YELLOW_("       hf mf csetblk 1 01020304050607080910111213141516 w"));
    return PM3_SUCCESS;
}

static int usage_hf14_gen3uid(void) {
    PrintAndLogEx(NORMAL, "Set UID for magic GEN 3 card without manufacturer block changing");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Usage:  hf mf gen3uid [h] <uid>");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "       h        this help");
    PrintAndLogEx(NORMAL, "       <uid>    UID 8/14 hex symbols");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf gen3uid 01020304"));
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf gen3uid 01020304050607"));
    return PM3_SUCCESS;
}
static int usage_hf14_gen3block(void) {
    PrintAndLogEx(NORMAL, "Overwrite full manufacturer block for magic GEN 3 card");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Usage:  hf mf gen3blk [h] [block data (up to 32 hex symbols)]");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "       h        this help");
    PrintAndLogEx(NORMAL, "       [block]  manufacturer block data up to 32 hex symbols to write");
    PrintAndLogEx(NORMAL, "                - If block data not specified, it prints current");
    PrintAndLogEx(NORMAL, "                  data without changes");
    PrintAndLogEx(NORMAL, "                - You can specify part of manufacturer block as");
    PrintAndLogEx(NORMAL, "                  4/7-bytes for UID change only for example");
    PrintAndLogEx(NORMAL, "                NOTE: BCC, SAK, ATQA will be calculated automatically");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf gen3blk 01020304FFFFFFFF0102030405060708"));
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf gen3blk 01020304"));
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf gen3blk 01020304050607"));
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf gen3blk"));
    return PM3_SUCCESS;
}
static int usage_hf14_gen3freeze(void) {
    PrintAndLogEx(NORMAL, "Perma lock further UID changes. No more UID changes available after operation completed");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Usage:  hf mf gen3freeze [h] <y>");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "       h        this help");
    PrintAndLogEx(NORMAL, "       <y>      confirm UID locks operation");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, _YELLOW_("      hf mf gen3freeze y"));
    return PM3_SUCCESS;
}

int mfc_ev1_print_signature(uint8_t *uid, uint8_t uidlen, uint8_t *signature, int signature_len) {

    // ref:  MIFARE Classic EV1 Originality Signature Validation
#define PUBLIC_MFCEV1_ECDA_KEYLEN 33
    const ecdsa_publickey_t nxp_mfc_public_keys[] = {
        {"NXP Mifare Classic MFC1C14_x", "044F6D3F294DEA5737F0F46FFEE88A356EED95695DD7E0C27A591E6F6F65962BAF"},
    };

    uint8_t i;
    bool is_valid = false;

    for (i = 0; i < ARRAYLEN(nxp_mfc_public_keys); i++) {

        int dl = 0;
        uint8_t key[PUBLIC_MFCEV1_ECDA_KEYLEN];
        param_gethex_to_eol(nxp_mfc_public_keys[i].value, 0, key, PUBLIC_MFCEV1_ECDA_KEYLEN, &dl);

        int res = ecdsa_signature_r_s_verify(MBEDTLS_ECP_DP_SECP128R1, key, uid, uidlen, signature, signature_len, false);
        is_valid = (res == 0);
        if (is_valid)
            break;
    }

    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Tag Signature"));
    if (is_valid == false || i == ARRAYLEN(nxp_mfc_public_keys)) {
        PrintAndLogEx(INFO, "    Elliptic curve parameters: NID_secp128r1");
        PrintAndLogEx(INFO, "             TAG IC Signature: %s", sprint_hex_inrow(signature, 32));
        PrintAndLogEx(SUCCESS, "       Signature verification: " _RED_("failed"));
        return PM3_ESOFT;
    }

    PrintAndLogEx(INFO, " IC signature public key name: %s", nxp_mfc_public_keys[i].desc);
    PrintAndLogEx(INFO, "IC signature public key value: %s", nxp_mfc_public_keys[i].value);
    PrintAndLogEx(INFO, "    Elliptic curve parameters: NID_secp128r1");
    PrintAndLogEx(INFO, "             TAG IC Signature: %s", sprint_hex_inrow(signature, 32));
    PrintAndLogEx(SUCCESS, "       Signature verification: " _GREEN_("successful"));
    return PM3_SUCCESS;
}

static int GetHFMF14AUID(uint8_t *uid, int *uidlen) {
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
    PacketResponseNG resp;
    if (!WaitForResponseTimeout(CMD_ACK, &resp, 2500)) {
        PrintAndLogEx(WARNING, "iso14443a card select failed");
        DropField();
        return 0;
    }

    iso14a_card_select_t card;
    memcpy(&card, (iso14a_card_select_t *)resp.data.asBytes, sizeof(iso14a_card_select_t));
    memcpy(uid, card.uid, card.uidlen * sizeof(uint8_t));
    *uidlen = card.uidlen;
    return 1;
}

static char *GenerateFilename(const char *prefix, const char *suffix) {
    if (! IfPm3Iso14443a()) {
        return NULL;
    }
    uint8_t uid[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int uidlen = 0;
    char *fptr = calloc(sizeof(char) * (strlen(prefix) + strlen(suffix)) + sizeof(uid) * 2 + 1,  sizeof(uint8_t));

    GetHFMF14AUID(uid, &uidlen);
    if (!uidlen) {
        PrintAndLogEx(WARNING, "No tag found.");
        free(fptr);
        return NULL;
    }

    strcpy(fptr, prefix);
    FillFileNameByUID(fptr, uid, suffix, uidlen);
    return fptr;
}

static int32_t initSectorTable(sector_t **src, int32_t items) {

    (*src) = calloc(items, sizeof(sector_t));

    if (*src == NULL)
        return -1;

    // empty e_sector
    for (int i = 0; i < items; ++i) {
        for (int j = 0; j < 2; ++j) {
            (*src)[i].Key[j] = 0xffffffffffff;
            (*src)[i].foundKey[j] = false;
        }
    }
    return items;
}

static void decode_print_st(uint16_t blockno, uint8_t *data) {
    if (mfIsSectorTrailer(blockno)) {
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "----------------------- " _CYAN_("Sector trailer decoder") " -----------------------");
        PrintAndLogEx(INFO, "key A........ " _GREEN_("%s"), sprint_hex_inrow(data, 6));
        PrintAndLogEx(INFO, "acr.......... " _GREEN_("%s"), sprint_hex_inrow(data + 6, 3));
        PrintAndLogEx(INFO, "user / gdb... " _GREEN_("%02x"), data[9]);
        PrintAndLogEx(INFO, "key B........ " _GREEN_("%s"), sprint_hex_inrow(data + 10, 6));
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "  # | Access rights");
        PrintAndLogEx(INFO, "----+-----------------------------------------------------------------");

        int bln = mfFirstBlockOfSector(mfSectorNum(blockno));
        int blinc = (mfNumBlocksPerSector(mfSectorNum(blockno)) > 4) ? 5 : 1;
        for (int i = 0; i < 4; i++) {
            PrintAndLogEx(INFO, "%3d%c| " _YELLOW_("%s"), bln, ((blinc > 1) && (i < 3) ? '+' : ' '), mfGetAccessConditionsDesc(i, &data[6]));
            bln += blinc;
        }
        PrintAndLogEx(INFO, "----------------------------------------------------------------------");
        PrintAndLogEx(NORMAL, "");
    }
}

static uint16_t NumOfBlocks(char card) {
    switch (card) {
        case '0' :
            return MIFARE_MINI_MAXBLOCK;
        case '1' :
            return MIFARE_1K_MAXBLOCK;
        case '2' :
            return MIFARE_2K_MAXBLOCK;
        case '4' :
            return MIFARE_4K_MAXBLOCK;
        default  :
            return 0;
    }
}

static uint8_t NumOfSectors(char card) {
    switch (card) {
        case '0' :
            return MIFARE_MINI_MAXSECTOR;
        case '1' :
            return MIFARE_1K_MAXSECTOR;
        case '2' :
            return MIFARE_2K_MAXSECTOR;
        case '4' :
            return MIFARE_4K_MAXSECTOR;
        default  :
            return 0;
    }
}

static uint8_t FirstBlockOfSector(uint8_t sectorNo) {
    if (sectorNo < 32) {
        return sectorNo * 4;
    } else {
        return 32 * 4 + (sectorNo - 32) * 16;
    }
}

static uint8_t NumBlocksPerSector(uint8_t sectorNo) {
    if (sectorNo < 32) {
        return 4;
    } else {
        return 16;
    }
}

static uint8_t GetSectorFromBlockNo(uint8_t blockNo) {
    if (blockNo < 128)
        return blockNo / 4;
    else
        return 32 + ((128 - blockNo) / 16);
}

static char GetFormatFromSector(uint8_t sectorNo) {
    switch (sectorNo) {
        case MIFARE_MINI_MAXSECTOR:
            return '0';
        case MIFARE_1K_MAXSECTOR:
            return '1';
        case MIFARE_2K_MAXSECTOR:
            return '2';
        case MIFARE_4K_MAXSECTOR:
            return '4';
        default  :
            return ' ';
    }
}

static void mf_print_block(uint8_t blockno, uint8_t *d) {
    if (blockno == 0) {
        PrintAndLogEx(INFO, "%3d | " _RED_("%s"), blockno, sprint_hex_ascii(d, MFBLOCK_SIZE));
    } else if (mfIsSectorTrailer(blockno)) {
        PrintAndLogEx(INFO, "%3d | " _YELLOW_("%s"), blockno, sprint_hex_ascii(d, MFBLOCK_SIZE));
    } else {
        PrintAndLogEx(INFO, "%3d | %s ", blockno, sprint_hex_ascii(d, MFBLOCK_SIZE));
    }
}

static void mf_print_blocks(uint16_t n, uint8_t *d) {
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");
    PrintAndLogEx(INFO, "blk | data                                            | ascii");
    PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");
    for (uint16_t i = 0; i < n; i++) {
        mf_print_block(i, d + (i * MFBLOCK_SIZE));
    }
    PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");
    PrintAndLogEx(NORMAL, "");
}


static int CmdHF14AMfDarkside(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf darkside",
                  "Darkside attack",
                  "hf mf darkside\n"
                  "hf mf darkside --blk 16\n"
                  "hf mf darkside --blk 16 -b\n");

    void *argtable[] = {
        arg_param_begin,
        arg_int0(NULL, "blk", "<dec> ", "Simulation type to use"),
        arg_lit0("b", NULL, "Target key B instead of default key A"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    uint8_t blockno = arg_get_u32_def(ctx, 1, 0);

    uint8_t key_type = MIFARE_AUTH_KEYA;

    if (arg_get_lit(ctx, 2)) {
        PrintAndLogEx(INFO, "Targeting key B");
        key_type = MIFARE_AUTH_KEYB;
    }

    CLIParserFree(ctx);

    uint64_t key = 0;

    int isOK = mfDarkside(blockno, key_type, &key);
    PrintAndLogEx(NORMAL, "");
    switch (isOK) {
        case -1 :
            PrintAndLogEx(WARNING, "button pressed. Aborted.");
            return PM3_ESOFT;
        case -2 :
            PrintAndLogEx(FAILED, "card is not vulnerable to Darkside attack (doesn't send NACK on authentication requests).");
            return PM3_ESOFT;
        case -3 :
            PrintAndLogEx(FAILED, "card is not vulnerable to Darkside attack (its random number generator is not predictable).");
            return PM3_ESOFT;
        case -4 :
            PrintAndLogEx(FAILED, "card is not vulnerable to Darkside attack (its random number generator seems to be based on the wellknown");
            PrintAndLogEx(FAILED, "generating polynomial with 16 effective bits only, but shows unexpected behaviour.");
            return PM3_ESOFT;
        case -5 :
            PrintAndLogEx(WARNING, "aborted via keyboard.");
            return PM3_ESOFT;
        default :
            PrintAndLogEx(SUCCESS, "found valid key: "_YELLOW_("%012" PRIx64), key);
            break;
    }
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

static int CmdHF14AMfWrBl(const char *Cmd) {
    uint8_t blockNo = 0;
    uint8_t keyType = 0;
    uint8_t key[6] = {0, 0, 0, 0, 0, 0};
    uint8_t bldata[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    char cmdp = 0x00;

    if (strlen(Cmd) < 3) {
        PrintAndLogEx(NORMAL, "Usage:  hf mf wrbl    <block number> <key A/B> <key (12 hex symbols)> <block data (32 hex symbols)>");
        PrintAndLogEx(NORMAL, "Examples:");
        PrintAndLogEx(NORMAL, "        hf mf wrbl 1 A FFFFFFFFFFFF 000102030405060708090A0B0C0D0E0F");
        return PM3_SUCCESS;
    }

    blockNo = param_get8(Cmd, 0);
    cmdp = tolower(param_getchar(Cmd, 1));
    if (cmdp == 0x00) {
        PrintAndLogEx(NORMAL, "Key type must be A or B");
        return PM3_EINVARG;
    }

    if (cmdp != 'a')
        keyType = 1;

    if (param_gethex(Cmd, 2, key, 12)) {
        PrintAndLogEx(NORMAL, "Key must include 12 HEX symbols");
        return PM3_EINVARG;
    }

    if (param_gethex(Cmd, 3, bldata, 32)) {
        PrintAndLogEx(NORMAL, "Block data must include 32 HEX symbols");
        return PM3_EINVARG;
    }

    PrintAndLogEx(NORMAL, "--block no %d, key %c - %s", blockNo, keyType ? 'B' : 'A', sprint_hex(key, 6));
    PrintAndLogEx(NORMAL, "--data: %s", sprint_hex(bldata, 16));

    uint8_t data[26];
    memcpy(data, key, 6);
    memcpy(data + 10, bldata, 16);
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_MIFARE_WRITEBL, blockNo, keyType, 0, data, sizeof(data));

    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500)) {
        uint8_t isOK  = resp.oldarg[0] & 0xff;
        PrintAndLogEx(NORMAL, "isOk:%02x", isOK);
    } else {
        PrintAndLogEx(NORMAL, "Command execute timeout");
    }

    return PM3_SUCCESS;
}

static int CmdHF14AMfRdBl(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf rdbl",
                  "Read MIFARE Classic block",
                  "hf mf rdbl --blk 0 -k FFFFFFFFFFFF\n"
                  "hf mf rdbl -b 3 -v   -> get block 3, decode sector trailer\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1(NULL, "blk", "<dec>", "block number"),
        arg_lit0("a", NULL, "input key type is key A (def)"),
        arg_lit0("b", NULL, "input key type is key B"),
        arg_str0("k", "key", "<hex>", "key specified as 6 hex bytes"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int b = arg_get_int_def(ctx, 1, 0);

    uint8_t keytype = 0;
    if (arg_get_lit(ctx, 2) && arg_get_lit(ctx, 3)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 3)) {
        keytype = 1;
    }

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 4, key, &keylen);
    bool verbose = arg_get_lit(ctx, 5);
    CLIParserFree(ctx); 

    if (b > 255) {
        return PM3_EINVARG;
    }
    uint8_t blockno = (uint8_t)b;

    uint8_t data[16] = {0};
    int res =  mfReadBlock(blockno, keytype, key, data);
    if (res == PM3_SUCCESS) {

        uint8_t sector = GetSectorFromBlockNo(blockno);
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "  # | data  - sector %02d / 0x%02X                        | ascii", sector, sector);
        PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");
        mf_print_block(blockno, data);
        if (verbose) {
            decode_print_st(blockno, data);
        }
    }
    PrintAndLogEx(NORMAL, "");
    return res;
}

static int CmdHF14AMfRdSc(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf rdbl",
                  "Read MIFARE Classic sector",
                  "hf mf rdbl -s 0 -k FFFFFFFFFFFF\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("a", NULL, "input key specified is A key (def)"),
        arg_lit0("b", NULL, "input key specified is B key"),
        arg_str0("k", "key", "<hex>", "key specified as 6 hex bytes"),
        arg_int1("s", "sec", "<dec>", "sector number"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    uint8_t keytype = 0;
    if (arg_get_lit(ctx, 1) && arg_get_lit(ctx, 2)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 2)) {
        keytype = 1;
    }

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 3, key, &keylen);

    int s = arg_get_int_def(ctx, 4, 0);
    bool verbose = arg_get_lit(ctx, 5);
    CLIParserFree(ctx); 

    if (s > MIFARE_4K_MAXSECTOR) {
        PrintAndLogEx(WARNING, "Sector number must be less then 40");
        return PM3_EINVARG;
    }
    uint8_t sector = (uint8_t)s;
    uint8_t sc_size = mfNumBlocksPerSector(sector) * MFBLOCK_SIZE;
    uint8_t *data = calloc(sc_size, sizeof(uint8_t));
    if (data == NULL) {
        PrintAndLogEx(ERR, "failed to allocate memory");
        return PM3_EMALLOC;
    }

    int res =  mfReadSector(sector, keytype, key, data);
    if (res == PM3_SUCCESS) {

        uint8_t blocks = NumBlocksPerSector(sector);
        uint8_t start = FirstBlockOfSector(sector);

        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "  # | data  - sector %02d / 0x%02X                        | ascii", sector, sector);
        PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");
        for (int i = 0; i < blocks; i++) {
            mf_print_block(start + i, data + (i * MFBLOCK_SIZE));
        }

        if (verbose) {
            decode_print_st(start + blocks - 1, data + ((blocks - 1) * MFBLOCK_SIZE));
        }       
    }
    free(data);
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

static int FastDumpWithEcFill(uint8_t numsectors) {
    PacketResponseNG resp;

    mfc_eload_t payload;
    payload.sectorcnt = numsectors;
    payload.keytype = 0;

    // ecfill key A
    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_EML_LOAD, (uint8_t *)&payload, sizeof(payload));

    bool res = WaitForResponseTimeout(CMD_HF_MIFARE_EML_LOAD, &resp, 2500);
    if (res == false) {
        PrintAndLogEx(WARNING, "Command execute timeout");
        return PM3_ETIMEOUT;
    }

    if (resp.status != PM3_SUCCESS) {
        PrintAndLogEx(INFO, "fast dump reported back failure w KEY A,  swapping to KEY B");

        // ecfill key B
        payload.keytype = 1;

        clearCommandBuffer();
        SendCommandNG(CMD_HF_MIFARE_EML_LOAD, (uint8_t *)&payload, sizeof(payload));
        res = WaitForResponseTimeout(CMD_HF_MIFARE_EML_LOAD, &resp, 2500);
        if (res == false) {
            PrintAndLogEx(WARNING, "Command execute timeout");
            return PM3_ETIMEOUT;
        }

        if (resp.status != PM3_SUCCESS) {
            PrintAndLogEx(INFO, "fast dump reported back failure w KEY B");
            PrintAndLogEx(INFO, "Dump file is " _RED_("PARTIAL") " complete");
        }
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfDump(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf dump",
                  "Dump MIFARE Classic tag to binary file\n"
                  "If no <name> given, UID will be used as filename",
                  "hf mf dump --mini                        --> MIFARE Mini\n"
                  "hf mf dump --1k                          --> MIFARE Classic 1k\n"
                  "hf mf dump --2k                          --> MIFARE 2k\n"
                  "hf mf dump --4k                          --> MIFARE 4k\n"
                  "hf mf dump --keys hf-mf-066C8B78-key.bin --> MIFARE 1k with keys from specified file\n");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("f", "file", "<fn>", "filename of dump"),
        arg_str0("k", "keys", "<fn>", "filename of keys"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int datafnlen = 0;
    char dataFilename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)dataFilename, FILE_PATH_SIZE, &datafnlen);

    int keyfnlen = 0;
    char keyFilename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 2), (uint8_t *)keyFilename, FILE_PATH_SIZE, &keyfnlen);

    bool m0 = arg_get_lit(ctx, 3);
    bool m1 = arg_get_lit(ctx, 4);
    bool m2 = arg_get_lit(ctx, 5);
    bool m4 = arg_get_lit(ctx, 6);

    CLIParserFree(ctx);

    uint64_t t1 = msclock();

    uint8_t sectorNo, blockNo;
    uint8_t keyA[40][6];
    uint8_t keyB[40][6];
    uint8_t rights[40][4];
    uint8_t carddata[256][16];

    FILE *f;
    PacketResponseNG resp;

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t numSectors = MIFARE_1K_MAXSECTOR;

    if (m0) {
        numSectors = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        numSectors = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        numSectors = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        numSectors = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    char *fptr;

    if (keyFilename[0] == 0x00) {
        fptr = GenerateFilename("hf-mf-", "-key.bin");
        if (fptr == NULL)
            return PM3_ESOFT;

        strcpy(keyFilename, fptr);
        free(fptr);
    }

    if ((f = fopen(keyFilename, "rb")) == NULL) {
        PrintAndLogEx(WARNING, "Could not find file " _YELLOW_("%s"), keyFilename);
        return PM3_EFILE;
    }

    PrintAndLogEx(INFO, "Using `" _YELLOW_("%s") "`", keyFilename);

    // Read keys A from file
    size_t bytes_read;
    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        bytes_read = fread(keyA[sectorNo], 1, 6, f);
        if (bytes_read != 6) {
            PrintAndLogEx(ERR, "File reading error.");
            fclose(f);
            return PM3_EFILE;
        }
    }

    // Read keys B from file
    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        bytes_read = fread(keyB[sectorNo], 1, 6, f);
        if (bytes_read != 6) {
            PrintAndLogEx(ERR, "File reading error.");
            fclose(f);
            return PM3_EFILE;
        }
    }

    fclose(f);

    PrintAndLogEx(INFO, "Reading sector access bits...");
    PrintAndLogEx(INFO, "." NOLF);

    uint8_t tries;
    mf_readblock_t payload;
    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        for (tries = 0; tries < MIFARE_SECTOR_RETRY; tries++) {
            PrintAndLogEx(NORMAL, "." NOLF);
            fflush(stdout);

            payload.blockno = FirstBlockOfSector(sectorNo) + NumBlocksPerSector(sectorNo) - 1;
            payload.keytype = 0;
            memcpy(payload.key, keyA[sectorNo], sizeof(payload.key));

            clearCommandBuffer();
            SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));

            if (WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500)) {

                uint8_t *data = resp.data.asBytes;
                if (resp.status == PM3_SUCCESS) {
                    rights[sectorNo][0] = ((data[7] & 0x10) >> 2) | ((data[8] & 0x1) << 1) | ((data[8] & 0x10) >> 4); // C1C2C3 for data area 0
                    rights[sectorNo][1] = ((data[7] & 0x20) >> 3) | ((data[8] & 0x2) << 0) | ((data[8] & 0x20) >> 5); // C1C2C3 for data area 1
                    rights[sectorNo][2] = ((data[7] & 0x40) >> 4) | ((data[8] & 0x4) >> 1) | ((data[8] & 0x40) >> 6); // C1C2C3 for data area 2
                    rights[sectorNo][3] = ((data[7] & 0x80) >> 5) | ((data[8] & 0x8) >> 2) | ((data[8] & 0x80) >> 7); // C1C2C3 for sector trailer
                    break;
                } else if (tries == 2) { // on last try set defaults
                    PrintAndLogEx(FAILED, "\ncould not get access rights for sector %2d. Trying with defaults...", sectorNo);
                    rights[sectorNo][0] = rights[sectorNo][1] = rights[sectorNo][2] = 0x00;
                    rights[sectorNo][3] = 0x01;
                }
            } else {
                PrintAndLogEx(FAILED, "\ncommand execute timeout when trying to read access rights for sector %2d. Trying with defaults...", sectorNo);
                rights[sectorNo][0] = rights[sectorNo][1] = rights[sectorNo][2] = 0x00;
                rights[sectorNo][3] = 0x01;
            }
        }
    }
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(SUCCESS, "Finished reading sector access bits");
    PrintAndLogEx(INFO, "Dumping all blocks from card...");

    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        for (blockNo = 0; blockNo < NumBlocksPerSector(sectorNo); blockNo++) {
            bool received = false;

            for (tries = 0; tries < MIFARE_SECTOR_RETRY; tries++) {
                if (blockNo == NumBlocksPerSector(sectorNo) - 1) { // sector trailer. At least the Access Conditions can always be read with key A.

                    payload.blockno = FirstBlockOfSector(sectorNo) + blockNo;
                    payload.keytype = 0;
                    memcpy(payload.key, keyA[sectorNo], sizeof(payload.key));

                    clearCommandBuffer();
                    SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));
                    received = WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500);
                } else {                                           // data block. Check if it can be read with key A or key B
                    uint8_t data_area = (sectorNo < 32) ? blockNo : blockNo / 5;
                    if ((rights[sectorNo][data_area] == 0x03) || (rights[sectorNo][data_area] == 0x05)) { // only key B would work

                        payload.blockno = FirstBlockOfSector(sectorNo) + blockNo;
                        payload.keytype = 1;
                        memcpy(payload.key, keyB[sectorNo], sizeof(payload.key));

                        clearCommandBuffer();
                        SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));
                        received = WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500);
                    } else if (rights[sectorNo][data_area] == 0x07) {                                     // no key would work
                        PrintAndLogEx(WARNING, "access rights do not allow reading of sector %2d block %3d", sectorNo, blockNo);
                        // where do you want to go??  Next sector or block?
                        break;
                    } else {                                                                              // key A would work

                        payload.blockno = FirstBlockOfSector(sectorNo) + blockNo;
                        payload.keytype = 0;
                        memcpy(payload.key, keyA[sectorNo], sizeof(payload.key));

                        clearCommandBuffer();
                        SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));
                        received = WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500);
                    }
                }
                if (received) {
                    if (resp.status == PM3_SUCCESS) {
                        // break the re-try loop
                        break;
                    }
                }
            }

            if (received) {
                uint8_t *data  = resp.data.asBytes;
                if (blockNo == NumBlocksPerSector(sectorNo) - 1) { // sector trailer. Fill in the keys.
                    data[0]  = (keyA[sectorNo][0]);
                    data[1]  = (keyA[sectorNo][1]);
                    data[2]  = (keyA[sectorNo][2]);
                    data[3]  = (keyA[sectorNo][3]);
                    data[4]  = (keyA[sectorNo][4]);
                    data[5]  = (keyA[sectorNo][5]);
                    data[10] = (keyB[sectorNo][0]);
                    data[11] = (keyB[sectorNo][1]);
                    data[12] = (keyB[sectorNo][2]);
                    data[13] = (keyB[sectorNo][3]);
                    data[14] = (keyB[sectorNo][4]);
                    data[15] = (keyB[sectorNo][5]);
                }
                if (resp.status == PM3_SUCCESS) {
                    memcpy(carddata[FirstBlockOfSector(sectorNo) + blockNo], data, 16);
                    PrintAndLogEx(SUCCESS, "successfully read block %2d of sector %2d.", blockNo, sectorNo);
                } else {
                    PrintAndLogEx(FAILED, "could not read block %2d of sector %2d", blockNo, sectorNo);
                    break;
                }
            } else {
                PrintAndLogEx(WARNING, "command execute timeout when trying to read block %2d of sector %2d.", blockNo, sectorNo);
                break;
            }
        }
    }

    PrintAndLogEx(SUCCESS, "time: %" PRIu64 " seconds\n", (msclock() - t1) / 1000);

    PrintAndLogEx(SUCCESS, "\nSucceeded in dumping all blocks");

    if (strlen(dataFilename) < 1) {
        fptr = GenerateFilename("hf-mf-", "-dump");
        if (fptr == NULL)
            return PM3_ESOFT;

        strcpy(dataFilename, fptr);
        free(fptr);
    }

    uint16_t bytes = 16 * (FirstBlockOfSector(numSectors - 1) + NumBlocksPerSector(numSectors - 1));

    saveFile(dataFilename, ".bin", (uint8_t *)carddata, bytes);
    saveFileEML(dataFilename, (uint8_t *)carddata, bytes, MFBLOCK_SIZE);
    saveFileJSON(dataFilename, jsfCardMemory, (uint8_t *)carddata, bytes, NULL);
    return PM3_SUCCESS;
}

static int CmdHF14AMfRestore(const char *Cmd) {
    uint8_t sectorNo, blockNo;
    uint8_t keyType = 0;
    uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t bldata[16] = {0x00};
    uint8_t keyA[40][6];
    uint8_t keyB[40][6];
    uint8_t numSectors = 16;
    uint8_t cmdp = 0;
    char keyFilename[FILE_PATH_SIZE] = "";
    char dataFilename[FILE_PATH_SIZE] = "";
    char szTemp[FILE_PATH_SIZE - 20] = "";
    char *fptr;
    FILE *fdump, *fkeys;

    bool use_keyfile_for_auth = false;
    while (param_getchar(Cmd, cmdp) != 0x00) {
        switch (tolower(param_getchar(Cmd, cmdp))) {
            case 'h':
                return usage_hf14_restore();
            case 'u':
                param_getstr(Cmd, cmdp + 1, szTemp, FILE_PATH_SIZE - 20);
                if (keyFilename[0] == 0x00)
                    snprintf(keyFilename, FILE_PATH_SIZE, "hf-mf-%s-key.bin", szTemp);
                if (dataFilename[0] == 0x00)
                    snprintf(dataFilename, FILE_PATH_SIZE, "hf-mf-%s-dump.bin", szTemp);
                cmdp += 2;
                break;
            case 'k':
                param_getstr(Cmd, cmdp + 1, keyFilename, FILE_PATH_SIZE);
                cmdp += 2;
                break;
            case 'f':
                param_getstr(Cmd, cmdp + 1, dataFilename, FILE_PATH_SIZE);
                cmdp += 2;
                break;
            case 'w':
                use_keyfile_for_auth = true;
                cmdp++;
                break;
            default:
                if (cmdp == 0) {
                    numSectors = NumOfSectors(param_getchar(Cmd, cmdp));
                    if (numSectors == 0) return usage_hf14_restore();
                    cmdp++;
                } else {
                    PrintAndLogEx(WARNING, "Unknown parameter '%c'\n", param_getchar(Cmd, cmdp));
                    return usage_hf14_restore();
                }
        }
    }

    if (keyFilename[0] == 0x00) {
        fptr = GenerateFilename("hf-mf-", "-key.bin");
        if (fptr == NULL)
            return 1;

        strcpy(keyFilename, fptr);
        free(fptr);
    }

    if ((fkeys = fopen(keyFilename, "rb")) == NULL) {
        PrintAndLogEx(WARNING, "Could not find file " _YELLOW_("%s"), keyFilename);
        return 1;
    }

    size_t bytes_read;
    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        bytes_read = fread(keyA[sectorNo], 1, 6, fkeys);
        if (bytes_read != 6) {
            PrintAndLogEx(ERR, "File reading error  " _YELLOW_("%s"), keyFilename);
            fclose(fkeys);
            return 2;
        }
    }

    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        bytes_read = fread(keyB[sectorNo], 1, 6, fkeys);
        if (bytes_read != 6) {
            PrintAndLogEx(ERR, "File reading error " _YELLOW_("%s"), keyFilename);
            fclose(fkeys);
            return 2;
        }
    }

    fclose(fkeys);

    if (dataFilename[0] == 0x00) {
        fptr = GenerateFilename("hf-mf-", "-dump.bin");
        if (fptr == NULL)
            return 1;

        strcpy(dataFilename, fptr);
        free(fptr);
    }

    if ((fdump = fopen(dataFilename, "rb")) == NULL) {
        PrintAndLogEx(WARNING, "Could not find file " _YELLOW_("%s"), dataFilename);
        return 1;
    }
    PrintAndLogEx(INFO, "Restoring " _YELLOW_("%s")" to card", dataFilename);

    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        for (blockNo = 0; blockNo < NumBlocksPerSector(sectorNo); blockNo++) {
            uint8_t data[26];
            bytes_read = fread(bldata, 1, 16, fdump);
            if (bytes_read != 16) {
                PrintAndLogEx(ERR, "File reading error " _YELLOW_("%s"), dataFilename);
                fclose(fdump);
                fdump = NULL;
                return 2;
            }

            if (use_keyfile_for_auth == false) {
                if (blockNo == NumBlocksPerSector(sectorNo) - 1) { // sector trailer
                    bldata[0]  = (keyA[sectorNo][0]);
                    bldata[1]  = (keyA[sectorNo][1]);
                    bldata[2]  = (keyA[sectorNo][2]);
                    bldata[3]  = (keyA[sectorNo][3]);
                    bldata[4]  = (keyA[sectorNo][4]);
                    bldata[5]  = (keyA[sectorNo][5]);
                    bldata[10] = (keyB[sectorNo][0]);
                    bldata[11] = (keyB[sectorNo][1]);
                    bldata[12] = (keyB[sectorNo][2]);
                    bldata[13] = (keyB[sectorNo][3]);
                    bldata[14] = (keyB[sectorNo][4]);
                    bldata[15] = (keyB[sectorNo][5]);
                }
            }

            memcpy(data + 10, bldata, 16);


            if (use_keyfile_for_auth) {
                for (uint8_t kt = 0; kt < 2; kt++) {

                    if (kt == 0)
                        memcpy(data, keyA[sectorNo], 6);
                    else
                        memcpy(data, keyB[sectorNo], 6);

                    PrintAndLogEx(NORMAL, "Writing to block %3d: %s", FirstBlockOfSector(sectorNo) + blockNo, sprint_hex(bldata, 16));
                    clearCommandBuffer();
                    SendCommandMIX(CMD_HF_MIFARE_WRITEBL, FirstBlockOfSector(sectorNo) + blockNo, kt, 0, data, sizeof(data));
                    PacketResponseNG resp;

                    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500)) {
                        uint8_t isOK  = resp.oldarg[0] & 0xff;
                        if (isOK == 0) {
                            PrintAndLogEx(FAILED, "isOk: %02x", isOK);
                        } else {
                            break;
                        }
                    } else {
                        PrintAndLogEx(WARNING, "Command execute timeout");
                    }
                }

            } else {
                memcpy(data, key, 6);
                PrintAndLogEx(NORMAL, "Writing to block %3d: %s", FirstBlockOfSector(sectorNo) + blockNo, sprint_hex(bldata, 16));
                clearCommandBuffer();
                SendCommandMIX(CMD_HF_MIFARE_WRITEBL, FirstBlockOfSector(sectorNo) + blockNo, keyType, 0, data, sizeof(data));

                PacketResponseNG resp;
                if (WaitForResponseTimeout(CMD_ACK, &resp, 1500)) {
                    uint8_t isOK  = resp.oldarg[0] & 0xff;
                    if (isOK == 0) {
                        PrintAndLogEx(FAILED, "isOk: %02x", isOK);
                    }
                } else {
                    PrintAndLogEx(WARNING, "Command execute timeout");
                }
            }
        }
    }
    fclose(fdump);
    PrintAndLogEx(INFO, "Done!");
    return PM3_SUCCESS;
}

static int CmdHF14AMfNested(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf nested",
                  "Execute Nested attack against MIFARE Classic card for key recovery",
                  "hf mf nested --single --blk 0 -a FFFFFFFFFFFF --tblk 4 --tkeya     --> Single sector key recovery. Use block 0 Key A to find block 4 Key A\n"
                  "hf mf nested --mini --blk 0 -a -k FFFFFFFFFFFF                     --> Key recovery against MIFARE Mini\n"
                  "hf mf nested --1k --blk 0 -a -k FFFFFFFFFFFF                       --> Key recovery against MIFARE Classic 1k\n"
                  "hf mf nested --2k --blk 0 -a -k FFFFFFFFFFFF                       --> Key recovery against MIFARE 2k\n"
                  "hf mf nested --4k --blk 0 -a -k FFFFFFFFFFFF                       --> Key recovery against MIFARE 4k");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("k", "key", "<hex>", "Key specified as 12 hex symbols"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_int0(NULL, "blk", "<dec>", "Input block number"),
        arg_lit0("a", NULL, "Input key specified is A key (default)"),
        arg_lit0("b", NULL, "Input key specified is B key"),
        arg_int0(NULL, "tblk", "<dec>", "Target block number"),
        arg_lit0(NULL, "tkeya", "Target A key (default)"),
        arg_lit0(NULL, "tkeyb", "Target B key"),
        arg_lit0(NULL, "emu", "Fill simulator keys from found keys"),
        arg_lit0(NULL, "dump", "Dump found keys to file"),
        arg_lit0(NULL, "single", "Single sector (defaults to All)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 1, key, &keylen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);

    uint8_t blockNo = arg_get_u32_def(ctx, 6, 0);

    uint8_t keyType = 0;

    if (arg_get_lit(ctx, 7) && arg_get_lit(ctx, 8)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 8)) {
        keyType = 1;
    }

    uint8_t trgBlockNo = arg_get_u32_def(ctx, 9, 0);

    uint8_t trgKeyType = 0;

    if (arg_get_lit(ctx, 10) && arg_get_lit(ctx, 11)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Target key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 11)) {
        trgKeyType = 1;
    }

    bool transferToEml = arg_get_lit(ctx, 12);
    bool createDumpFile = arg_get_lit(ctx, 13);
    bool singleSector = arg_get_lit(ctx, 14);

    CLIParserFree(ctx);

    //validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    }

    uint8_t SectorsCnt = 1;
    if (m0) {
        SectorsCnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        SectorsCnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        SectorsCnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        SectorsCnt = MIFARE_4K_MAXSECTOR;
    }

    if (singleSector == false) {
        if (SectorsCnt == 0) {
            PrintAndLogEx(WARNING, "Invalid MIFARE Type");
            return PM3_EINVARG;
        }
    }

    if (keylen != 6) {
        PrintAndLogEx(WARNING, "Input key must include 12 HEX symbols");
        return PM3_EINVARG;
    }

    sector_t *e_sector = NULL;
    uint8_t keyBlock[(ARRAYLEN(g_mifare_default_keys) + 1) * 6];
    uint64_t key64 = 0;

    // check if tag doesn't have static nonce
    if (detect_classic_static_nonce() == NONCE_STATIC) {
        PrintAndLogEx(WARNING, "Static nonce detected. Quitting...");
        PrintAndLogEx(INFO, "\t Try use " _YELLOW_("`hf mf staticnested`"));
        return PM3_EOPABORTED;
    }

    // check if we can authenticate to sector
    if (mfCheckKeys(blockNo, keyType, true, 1, key, &key64) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "Wrong key. Can't authenticate to block:%3d key type:%c", blockNo, keyType ? 'B' : 'A');
        return PM3_EOPABORTED;
    }

    if (singleSector) {
        int16_t isOK = mfnested(blockNo, keyType, key, trgBlockNo, trgKeyType, keyBlock, true);
        switch (isOK) {
            case PM3_ETIMEOUT:
                PrintAndLogEx(ERR, "Command execute timeout\n");
                break;
            case PM3_EOPABORTED:
                PrintAndLogEx(WARNING, "Button pressed. Aborted.\n");
                break;
            case PM3_EFAILED:
                PrintAndLogEx(FAILED, "Tag isn't vulnerable to Nested Attack (PRNG is not predictable).\n");
                break;
            case PM3_ESOFT:
                PrintAndLogEx(FAILED, "No valid key found");
                break;
            case PM3_SUCCESS:
                key64 = bytes_to_num(keyBlock, 6);

                // transfer key to the emulator
                if (transferToEml) {
                    uint8_t sectortrailer;

                    if (trgBlockNo < 32 * 4) {  // 4 block sector
                        sectortrailer = trgBlockNo | 0x03;
                    } else {                    // 16 block sector
                        sectortrailer = trgBlockNo | 0x0f;
                    }
                    mfEmlGetMem(keyBlock, sectortrailer, 1);

                    if (!trgKeyType)
                        num_to_bytes(key64, 6, keyBlock);
                    else
                        num_to_bytes(key64, 6, &keyBlock[10]);

                    mfEmlSetMem(keyBlock, sectortrailer, 1);
                    PrintAndLogEx(SUCCESS, "Key transferred to emulator memory.");
                }
                return PM3_SUCCESS;
            default :
                PrintAndLogEx(ERR, "Unknown error.\n");
        }
        return PM3_SUCCESS;
    } else { // ------------------------------------  multiple sectors working
        uint64_t t1 = msclock();

        e_sector = calloc(SectorsCnt, sizeof(sector_t));
        if (e_sector == NULL) return PM3_EMALLOC;

        // add our known key
        e_sector[GetSectorFromBlockNo(blockNo)].foundKey[keyType] = 1;
        e_sector[GetSectorFromBlockNo(blockNo)].Key[keyType] = key64;

        //test current key and additional standard keys first
        // add parameter key
        memcpy(keyBlock + (ARRAYLEN(g_mifare_default_keys) * 6), key, 6);

        for (int cnt = 0; cnt < ARRAYLEN(g_mifare_default_keys); cnt++) {
            num_to_bytes(g_mifare_default_keys[cnt], 6, (uint8_t *)(keyBlock + cnt * 6));
        }

        PrintAndLogEx(SUCCESS, "Testing known keys. Sector count "_YELLOW_("%d"), SectorsCnt);
        int res = mfCheckKeys_fast(SectorsCnt, true, true, 1, ARRAYLEN(g_mifare_default_keys) + 1, keyBlock, e_sector, false);
        if (res == PM3_SUCCESS) {
            PrintAndLogEx(SUCCESS, "Fast check found all keys");
            goto jumptoend;
        }

        uint64_t t2 = msclock() - t1;
        PrintAndLogEx(SUCCESS, "Time to check " _YELLOW_("%zu") " known keys: %.0f seconds\n", ARRAYLEN(g_mifare_default_keys), (float)t2 / 1000.0);
        PrintAndLogEx(SUCCESS, "enter nested key recovery");

        // nested sectors
        bool calibrate = true;

        for (trgKeyType = 0; trgKeyType < 2; ++trgKeyType) {
            for (uint8_t sectorNo = 0; sectorNo < SectorsCnt; ++sectorNo) {
                for (int i = 0; i < MIFARE_SECTOR_RETRY; i++) {

                    if (e_sector[sectorNo].foundKey[trgKeyType]) continue;

                    int16_t isOK = mfnested(blockNo, keyType, key, FirstBlockOfSector(sectorNo), trgKeyType, keyBlock, calibrate);
                    switch (isOK) {
                        case PM3_ETIMEOUT:
                            PrintAndLogEx(ERR, "Command execute timeout\n");
                            break;
                        case PM3_EOPABORTED:
                            PrintAndLogEx(WARNING, "button pressed. Aborted.\n");
                            break;
                        case PM3_EFAILED :
                            PrintAndLogEx(FAILED, "Tag isn't vulnerable to Nested Attack (PRNG is not predictable).\n");
                            break;
                        case PM3_ESOFT:
                            //key not found
                            calibrate = false;
                            continue;
                        case PM3_SUCCESS:
                            calibrate = false;
                            e_sector[sectorNo].foundKey[trgKeyType] = 1;
                            e_sector[sectorNo].Key[trgKeyType] = bytes_to_num(keyBlock, 6);

                            mfCheckKeys_fast(SectorsCnt, true, true, 2, 1, keyBlock, e_sector, false);
                            continue;
                        default :
                            PrintAndLogEx(ERR, "Unknown error.\n");
                    }
                    free(e_sector);
                    return PM3_ESOFT;
                }
            }
        }

        t1 = msclock() - t1;
        PrintAndLogEx(SUCCESS, "time in nested " _YELLOW_("%.0f") " seconds\n", (float)t1 / 1000.0);


        // 20160116 If Sector A is found, but not Sector B,  try just reading it of the tag?
        PrintAndLogEx(INFO, "trying to read key B...");
        for (int i = 0; i < SectorsCnt; i++) {
            // KEY A but not KEY B
            if (e_sector[i].foundKey[0] && !e_sector[i].foundKey[1]) {

                uint8_t sectrail = (FirstBlockOfSector(i) + NumBlocksPerSector(i) - 1);

                PrintAndLogEx(SUCCESS, "reading block %d", sectrail);

                mf_readblock_t payload;
                payload.blockno = sectrail;
                payload.keytype = 0;

                num_to_bytes(e_sector[i].Key[0], 6, payload.key); // KEY A

                clearCommandBuffer();
                SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));

                PacketResponseNG resp;
                if (!WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500)) continue;

                if (resp.status != PM3_SUCCESS) continue;

                uint8_t *data = resp.data.asBytes;
                key64 = bytes_to_num(data + 10, 6);
                if (key64) {
                    PrintAndLogEx(SUCCESS, "data: %s", sprint_hex(data + 10, 6));
                    e_sector[i].foundKey[1] = true;
                    e_sector[i].Key[1] = key64;
                }
            }
        }

jumptoend:

        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(SUCCESS, _GREEN_("found keys:"));

        //print them
        printKeyTable(SectorsCnt, e_sector);

        // transfer them to the emulator
        if (transferToEml) {
            // fast push mode
            conn.block_after_ACK = true;
            for (int i = 0; i < SectorsCnt; i++) {
                mfEmlGetMem(keyBlock, FirstBlockOfSector(i) + NumBlocksPerSector(i) - 1, 1);

                if (e_sector[i].foundKey[0])
                    num_to_bytes(e_sector[i].Key[0], 6, keyBlock);

                if (e_sector[i].foundKey[1])
                    num_to_bytes(e_sector[i].Key[1], 6, &keyBlock[10]);

                if (i == SectorsCnt - 1) {
                    // Disable fast mode on last packet
                    conn.block_after_ACK = false;
                }
                mfEmlSetMem(keyBlock, FirstBlockOfSector(i) + NumBlocksPerSector(i) - 1, 1);
            }
            PrintAndLogEx(SUCCESS, "keys transferred to emulator memory.");
        }

        // Create dump file
        if (createDumpFile) {
            char *fptr = GenerateFilename("hf-mf-", "-key.bin");
            if (createMfcKeyDump(fptr, SectorsCnt, e_sector) != PM3_SUCCESS) {
                PrintAndLogEx(ERR, "Failed to save keys to file");
                free(e_sector);
                free(fptr);
                return PM3_ESOFT;
            }
            free(fptr);
        }
        free(e_sector);
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfNestedStatic(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf staticnested",
                  "Execute Nested attack against MIFARE Classic card with static nonce for key recovery",
                  "hf mf staticnested --mini --blk 0 -a -k FFFFFFFFFFFF                   --> Key recovery against MIFARE Mini\n"
                  "hf mf staticnested --1k --blk 0 -a -k FFFFFFFFFFFF                     --> Key recovery against MIFARE Classic 1k\n"
                  "hf mf staticnested --2k --blk 0 -a -k FFFFFFFFFFFF                     --> Key recovery against MIFARE 2k\n"
                  "hf mf staticnested --4k --blk 0 -a -k FFFFFFFFFFFF                     --> Key recovery against MIFARE 4k\n");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("k", "key", "<hex>", "Key specified as 12 hex symbols"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_int0(NULL, "blk", "<dec>", "Input block number"),
        arg_lit0("a", NULL, "Input key specified is A key (default)"),
        arg_lit0("b", NULL, "Input key specified is B key"),
        arg_lit0("e", "emukeys", "Fill simulator keys from found keys"),
        arg_lit0(NULL, "dumpkeys", "Dump found keys to file"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 1, key, &keylen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);

    uint8_t blockNo = arg_get_u32_def(ctx, 6, 0);

    uint8_t keyType = 0;

    if (arg_get_lit(ctx, 7) && arg_get_lit(ctx, 8)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 8)) {
        keyType = 1;
    }

    bool transferToEml = arg_get_lit(ctx, 9);
    bool createDumpFile = arg_get_lit(ctx, 10);

    CLIParserFree(ctx);

    //validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    }

    uint8_t SectorsCnt = 1;
    if (m0) {
        SectorsCnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        SectorsCnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        SectorsCnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        SectorsCnt = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    if (keylen != 6) {
        PrintAndLogEx(WARNING, "Input key must include 12 HEX symbols");
        return PM3_EINVARG;
    }

    sector_t *e_sector = NULL;

    uint8_t trgKeyType = 0;

    uint8_t keyBlock[(ARRAYLEN(g_mifare_default_keys) + 1) * 6];
    uint64_t key64 = 0;

    // check if tag have static nonce
    if (detect_classic_static_nonce() != NONCE_STATIC) {
        PrintAndLogEx(WARNING, "Normal nonce detected, or failed read of card. Quitting...");
        PrintAndLogEx(INFO, "\t Try use " _YELLOW_("`hf mf nested`"));
        return PM3_EOPABORTED;
    }

    // check if we can authenticate to sector
    if (mfCheckKeys(blockNo, keyType, true, 1, key, &key64) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "Wrong key. Can't authenticate to block: %3d key type: %c", blockNo, keyType ? 'B' : 'A');
        return PM3_EOPABORTED;
    }

    if (IfPm3Flash()) {
        PrintAndLogEx(INFO, "RDV4 with flashmemory supported detected.");
    }

    uint64_t t1 = msclock();

    e_sector = calloc(SectorsCnt, sizeof(sector_t));
    if (e_sector == NULL) return PM3_EMALLOC;

    // add our known key
    e_sector[GetSectorFromBlockNo(blockNo)].foundKey[keyType] = 1;
    e_sector[GetSectorFromBlockNo(blockNo)].Key[keyType] = key64;

    //test current key and additional standard keys first
    // add parameter key
    memcpy(keyBlock + (ARRAYLEN(g_mifare_default_keys) * 6), key, 6);

    for (int cnt = 0; cnt < ARRAYLEN(g_mifare_default_keys); cnt++) {
        num_to_bytes(g_mifare_default_keys[cnt], 6, (uint8_t *)(keyBlock + cnt * 6));
    }

    PrintAndLogEx(SUCCESS, "Testing known keys. Sector count "_YELLOW_("%d"), SectorsCnt);
    int res = mfCheckKeys_fast(SectorsCnt, true, true, 1, ARRAYLEN(g_mifare_default_keys) + 1, keyBlock, e_sector, false);
    if (res == PM3_SUCCESS) {
        // all keys found
        PrintAndLogEx(SUCCESS, "Fast check found all keys");
        goto jumptoend;
    }

    uint64_t t2 = msclock() - t1;
    PrintAndLogEx(SUCCESS, "Time to check "_YELLOW_("%zu") " known keys: %.0f seconds\n", ARRAYLEN(g_mifare_default_keys), (float)t2 / 1000.0);
    PrintAndLogEx(SUCCESS, "enter static nested key recovery");

    // nested sectors
    for (trgKeyType = 0; trgKeyType < 2; ++trgKeyType) {
        for (uint8_t sectorNo = 0; sectorNo < SectorsCnt; ++sectorNo) {

            for (int i = 0; i < 1; i++) {

                if (e_sector[sectorNo].foundKey[trgKeyType]) continue;

                int16_t isOK = mfStaticNested(blockNo, keyType, key, FirstBlockOfSector(sectorNo), trgKeyType, keyBlock);
                switch (isOK) {
                    case PM3_ETIMEOUT :
                        PrintAndLogEx(ERR, "Command execute timeout");
                        break;
                    case PM3_EOPABORTED :
                        PrintAndLogEx(WARNING, "aborted via keyboard.");
                        break;
                    case PM3_ESOFT :
                        continue;
                    case PM3_SUCCESS :
                        e_sector[sectorNo].foundKey[trgKeyType] = 1;
                        e_sector[sectorNo].Key[trgKeyType] = bytes_to_num(keyBlock, 6);

                        mfCheckKeys_fast(SectorsCnt, true, true, 2, 1, keyBlock, e_sector, false);
                        continue;
                    default :
                        PrintAndLogEx(ERR, "unknown error.\n");
                }
                free(e_sector);
                return PM3_ESOFT;
            }
        }
    }

    t1 = msclock() - t1;
    PrintAndLogEx(SUCCESS, "time in static nested " _YELLOW_("%.0f") " seconds\n", (float)t1 / 1000.0);


    // 20160116 If Sector A is found, but not Sector B,  try just reading it of the tag?
    PrintAndLogEx(INFO, "trying to read key B...");
    for (int i = 0; i < SectorsCnt; i++) {
        // KEY A but not KEY B
        if (e_sector[i].foundKey[0] && !e_sector[i].foundKey[1]) {

            uint8_t sectrail = (FirstBlockOfSector(i) + NumBlocksPerSector(i) - 1);

            PrintAndLogEx(SUCCESS, "reading block %d", sectrail);

            mf_readblock_t payload;
            payload.blockno = sectrail;
            payload.keytype = 0;

            num_to_bytes(e_sector[i].Key[0], 6, payload.key); // KEY A

            clearCommandBuffer();
            SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));

            PacketResponseNG resp;
            if (!WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500)) continue;

            if (resp.status != PM3_SUCCESS) continue;

            uint8_t *data = resp.data.asBytes;
            key64 = bytes_to_num(data + 10, 6);
            if (key64) {
                PrintAndLogEx(SUCCESS, "data: %s", sprint_hex(data + 10, 6));
                e_sector[i].foundKey[1] = true;
                e_sector[i].Key[1] = key64;
            }
        }
    }

jumptoend:

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(SUCCESS, _GREEN_("found keys:"));

    //print them
    printKeyTable(SectorsCnt, e_sector);

    // transfer them to the emulator
    if (transferToEml) {
        // fast push mode
        conn.block_after_ACK = true;
        for (int i = 0; i < SectorsCnt; i++) {
            mfEmlGetMem(keyBlock, FirstBlockOfSector(i) + NumBlocksPerSector(i) - 1, 1);

            if (e_sector[i].foundKey[0])
                num_to_bytes(e_sector[i].Key[0], 6, keyBlock);

            if (e_sector[i].foundKey[1])
                num_to_bytes(e_sector[i].Key[1], 6, &keyBlock[10]);

            if (i == SectorsCnt - 1) {
                // Disable fast mode on last packet
                conn.block_after_ACK = false;
            }
            mfEmlSetMem(keyBlock, FirstBlockOfSector(i) + NumBlocksPerSector(i) - 1, 1);
        }
        PrintAndLogEx(SUCCESS, "keys transferred to emulator memory.");
    }

    // Create dump file
    if (createDumpFile) {
        char *fptr = GenerateFilename("hf-mf-", "-key.bin");
        if (createMfcKeyDump(fptr, SectorsCnt, e_sector) != PM3_SUCCESS) {
            PrintAndLogEx(ERR, "Failed to save keys to file");
            free(e_sector);
            free(fptr);
            return PM3_ESOFT;
        }
        free(fptr);
    }
    free(e_sector);

    return PM3_SUCCESS;
}

static int CmdHF14AMfNestedHard(const char *Cmd) {
    uint8_t blockNo = 0;
    uint8_t keyType = 0;
    uint8_t trgBlockNo = 0;
    uint8_t trgKeyType = 0;
    uint8_t key[6] = {0, 0, 0, 0, 0, 0};
    uint8_t trgkey[6] = {0, 0, 0, 0, 0, 0};
    uint8_t cmdp = 0;
    char filename[FILE_PATH_SIZE] = {0};
    char szTemp[FILE_PATH_SIZE - 20];
    char ctmp;

    bool know_target_key = false;
    bool nonce_file_read = false;
    bool nonce_file_write = false;
    bool slow = false;
    int tests = 0;

    switch (tolower(param_getchar(Cmd, cmdp))) {
        case 'h':
            return usage_hf14_hardnested();
        case 'r': {
            char *fptr = GenerateFilename("hf-mf-", "-nonces.bin");
            if (fptr == NULL)
                strncpy(filename, "nonces.bin", FILE_PATH_SIZE - 1);
            else
                strncpy(filename, fptr, FILE_PATH_SIZE - 1);

            free(fptr);
            nonce_file_read = true;
            if (!param_gethex(Cmd, cmdp + 1, trgkey, 12)) {
                know_target_key = true;
            }
            cmdp++;
            break;
        }
        case 't':
            tests = param_get32ex(Cmd, cmdp + 1, 100, 10);
            if (!param_gethex(Cmd, cmdp + 2, trgkey, 12)) {
                know_target_key = true;
            }
            cmdp += 2;
            break;
        default:
            if (param_getchar(Cmd, cmdp) == 0x00) {
                PrintAndLogEx(WARNING, "Block number is missing");
                return usage_hf14_hardnested();
            }

            blockNo = param_get8(Cmd, cmdp);
            ctmp = tolower(param_getchar(Cmd, cmdp + 1));
            if (ctmp != 'a' && ctmp != 'b') {
                PrintAndLogEx(WARNING, "Key type must be A or B");
                return 1;
            }

            if (ctmp != 'a') {
                keyType = 1;
            }

            if (param_gethex(Cmd, cmdp + 2, key, 12)) {
                PrintAndLogEx(WARNING, "Key must include 12 HEX symbols");
                return 1;
            }

            if (param_getchar(Cmd, cmdp + 3) == 0x00) {
                PrintAndLogEx(WARNING, "Target block number is missing");
                return 1;
            }

            trgBlockNo = param_get8(Cmd, cmdp + 3);

            ctmp = tolower(param_getchar(Cmd, cmdp + 4));
            if (ctmp != 'a' && ctmp != 'b') {
                PrintAndLogEx(WARNING, "Target key type must be A or B");
                return 1;
            }
            if (ctmp != 'a') {
                trgKeyType = 1;
            }
            cmdp += 5;
    }
    if (!param_gethex(Cmd, cmdp, trgkey, 12)) {
        know_target_key = true;
        cmdp++;
    }

    while ((ctmp = param_getchar(Cmd, cmdp))) {
        switch (tolower(ctmp)) {
            case 's':
                slow = true;
                break;
            case 'w': {
                nonce_file_write = true;
                char *fptr = GenerateFilename("hf-mf-", "-nonces.bin");
                if (fptr == NULL)
                    return 1;
                strncpy(filename, fptr, FILE_PATH_SIZE - 1);
                free(fptr);
                break;
            }
            case 'u':
                param_getstr(Cmd, cmdp + 1, szTemp, FILE_PATH_SIZE - 20);
                snprintf(filename, FILE_PATH_SIZE, "hf-mf-%s-nonces.bin", szTemp);
                cmdp++;
                break;
            case 'f':
                param_getstr(Cmd, cmdp + 1, szTemp, FILE_PATH_SIZE - 20);
                strncpy(filename, szTemp, FILE_PATH_SIZE - 20);
                cmdp++;
                break;
            case 'i':
                SetSIMDInstr(SIMD_AUTO);
                ctmp = tolower(param_getchar(Cmd, cmdp + 1));
                switch (ctmp) {
#if defined(COMPILER_HAS_SIMD_AVX512)
                    case '5':
                        SetSIMDInstr(SIMD_AVX512);
                        break;
#endif
#if defined(COMPILER_HAS_SIMD)
                    case '2':
                        SetSIMDInstr(SIMD_AVX2);
                        break;
                    case 'a':
                        SetSIMDInstr(SIMD_AVX);
                        break;
                    case 's':
                        SetSIMDInstr(SIMD_SSE2);
                        break;
                    case 'm':
                        SetSIMDInstr(SIMD_MMX);
                        break;
#endif
                    case 'n':
                        SetSIMDInstr(SIMD_NONE);
                        break;
                    default:
                        PrintAndLogEx(WARNING, "Unknown SIMD type. %c", ctmp);
                        return 1;
                }
                cmdp += 2;
                break;
            default:
                PrintAndLogEx(WARNING, "Unknown parameter '%c'\n", ctmp);
                usage_hf14_hardnested();
                return 1;
        }
        cmdp++;
    }

    if (!know_target_key && nonce_file_read == false) {

        // check if tag doesn't have static nonce
        if (detect_classic_static_nonce() == NONCE_STATIC) {
            PrintAndLogEx(WARNING, "Static nonce detected. Quitting...");
            PrintAndLogEx(HINT, "\tTry use `" _YELLOW_("hf mf staticnested") "`");
            return PM3_EOPABORTED;
        }

        uint64_t key64 = 0;
        // check if we can authenticate to sector
        if (mfCheckKeys(blockNo, keyType, true, 1, key, &key64) != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, "Key is wrong. Can't authenticate to block: %3d  key type: %c", blockNo, keyType ? 'B' : 'A');
            return 3;
        }
    }

    PrintAndLogEx(INFO, "Target block no:%3d, target key type:%c, known target key: 0x%02x%02x%02x%02x%02x%02x%s",
                  trgBlockNo,
                  trgKeyType ? 'B' : 'A',
                  trgkey[0], trgkey[1], trgkey[2], trgkey[3], trgkey[4], trgkey[5],
                  know_target_key ? "" : " (not set)"
                 );
    PrintAndLogEx(INFO, "File action: %s, Slow: %s, Tests: %d ",
                  nonce_file_write ? "write" : nonce_file_read ? "read" : "none",
                  slow ? "Yes" : "No",
                  tests);

    uint64_t foundkey = 0;
    int16_t isOK = mfnestedhard(blockNo, keyType, key, trgBlockNo, trgKeyType, know_target_key ? trgkey : NULL, nonce_file_read, nonce_file_write, slow, tests, &foundkey, filename);

    if ((tests == 0) && IfPm3Iso14443a()) {
        DropField();
    }

    if (isOK) {
        switch (isOK) {
            case 1 :
                PrintAndLogEx(ERR, "Error: No response from Proxmark3.\n");
                break;
            case 2 :
                PrintAndLogEx(NORMAL, "Button pressed. Aborted.\n");
                break;
            default :
                break;
        }
        return 2;
    }
    return 0;
}

static int CmdHF14AMfAutoPWN(const char *Cmd) {
    // Nested and Hardnested parameter
    uint8_t blockNo = 0;
    uint8_t keyType = 0;
    uint8_t key[6] = {0};
    uint64_t key64 = 0;
    bool calibrate = true;
    // Attack key storage variables
    uint8_t *keyBlock = NULL;
    uint32_t key_cnt = 0;
    sector_t *e_sector;
    uint8_t sectors_cnt = MIFARE_1K_MAXSECTOR;
    int block_cnt = MIFARE_1K_MAXBLOCK;
    uint8_t tmp_key[6] = {0};
    bool know_target_key = false;
    // For the timer
    uint64_t t1;
    // Parameters and dictionary file
    char filename[FILE_PATH_SIZE] = {0};
    uint8_t cmdp = 0;
    char ctmp;
    // Nested and Hardnested returned status
    uint64_t foundkey = 0;
    int isOK = 0;
    int current_sector_i = 0, current_key_type_i = 0;
    // Dumping and transfere to simulater memory
    uint8_t block[16] = {0x00};
    uint8_t *dump;
    int bytes;
    // Settings
    bool slow = false;
    bool legacy_mfchk = false;
    int prng_type = PM3_EUNDEF;
    int has_staticnonce;
    bool verbose = false;
    bool has_filename = false;
    bool errors = false;
    uint8_t num_found_keys = 0;

    // Parse the options given by the user
    while ((ctmp = param_getchar(Cmd, cmdp)) && !errors) {
        switch (tolower(ctmp)) {
            case 'h':
                return usage_hf14_autopwn();
            case 'f':
                if (param_getstr(Cmd, cmdp + 1, filename, FILE_PATH_SIZE) >= FILE_PATH_SIZE) {
                    PrintAndLogEx(FAILED, "Filename too long");
                    errors = true;
                } else {
                    has_filename = true;
                }
                cmdp += 2;
                break;
            case 'l':
                legacy_mfchk = true;
                cmdp++;
                break;
            case 'v':
                verbose = true;
                cmdp++;
                break;
            case '*':
                // Get the number of sectors
                sectors_cnt = NumOfSectors(param_getchar(Cmd, cmdp + 1));
                block_cnt = NumOfBlocks(param_getchar(Cmd, cmdp + 1));
                cmdp += 2;
                break;
            case 'k':
                // Get the known block number
                if (param_getchar(Cmd, cmdp + 1) == 0x00) {
                    errors = true;
                    break;
                }

                blockNo = param_get8(Cmd, cmdp + 1);

                // Get the knonwn block type
                ctmp = tolower(param_getchar(Cmd, cmdp + 2));
                if (ctmp != 'a' && ctmp != 'b') {
                    PrintAndLogEx(WARNING, "Key type must be A or B");
                    errors = true;
                    break;
                }

                if (ctmp != 'a') {
                    keyType = 1;
                }

                // Get the known block key
                if (param_gethex(Cmd, cmdp + 3, key, 12)) {
                    PrintAndLogEx(WARNING, "Key must include 12 HEX symbols");
                    errors = true;
                }
                know_target_key = true;
                cmdp += 3;
            case 's':
                slow = true;
                cmdp++;
                break;
            case 'i':
                SetSIMDInstr(SIMD_AUTO);
                ctmp = tolower(param_getchar(Cmd, cmdp + 1));
                switch (ctmp) {
#if defined(COMPILER_HAS_SIMD_AVX512)
                    case '5':
                        SetSIMDInstr(SIMD_AVX512);
                        break;
#endif
#if defined(COMPILER_HAS_SIMD)
                    case '2':
                        SetSIMDInstr(SIMD_AVX2);
                        break;
                    case 'a':
                        SetSIMDInstr(SIMD_AVX);
                        break;
                    case 's':
                        SetSIMDInstr(SIMD_SSE2);
                        break;
                    case 'm':
                        SetSIMDInstr(SIMD_MMX);
                        break;
#endif
                    case 'n':
                        SetSIMDInstr(SIMD_NONE);
                        break;
                    default:
                        PrintAndLogEx(WARNING, "Unknown SIMD type. %c", ctmp);
                        return PM3_EINVARG;
                }
                cmdp += 2;
                break;
            default:
                PrintAndLogEx(WARNING, "Unknown parameter '%c'\n", ctmp);
                return usage_hf14_autopwn();
        }
    }

    if (errors) {
        return usage_hf14_autopwn();
    }

    // create/initialize key storage structure
    int32_t res = initSectorTable(&e_sector, sectors_cnt);
    if (res != sectors_cnt) {
        free(e_sector);
        return PM3_EMALLOC;
    }

    // read uid to generate a filename for the key file
    char *fptr = GenerateFilename("hf-mf-", "-key.bin");

    // check if tag doesn't have static nonce
    has_staticnonce = detect_classic_static_nonce();

    // card prng type (weak=1 / hard=0 / select/card comm error = negative value)
    if (has_staticnonce == NONCE_NORMAL)  {
        prng_type = detect_classic_prng();
        if (prng_type < 0) {
            PrintAndLogEx(FAILED, "\nNo tag detected or other tag communication error");
            free(e_sector);
            free(fptr);
            return prng_type;
        }
    }

    // print parameters
    if (verbose) {
        PrintAndLogEx(INFO, "======================= " _YELLOW_("SETTINGS") " =======================");
        PrintAndLogEx(INFO, " card sectors .. " _YELLOW_("%d"), sectors_cnt);
        PrintAndLogEx(INFO, " key supplied .. " _YELLOW_("%s"), know_target_key ? "True" : "False");
        PrintAndLogEx(INFO, " known sector .. " _YELLOW_("%d"), blockNo);
        PrintAndLogEx(INFO, " keytype ....... " _YELLOW_("%c"), keyType ? 'B' : 'A');
        PrintAndLogEx(INFO, " known key ..... " _YELLOW_("%s"), sprint_hex(key, sizeof(key)));

        if (has_staticnonce == NONCE_STATIC)
            PrintAndLogEx(INFO, " card PRNG ..... " _YELLOW_("STATIC"));
        else if (has_staticnonce == NONCE_NORMAL)
            PrintAndLogEx(INFO, " card PRNG ..... " _YELLOW_("%s"), prng_type ? "WEAK" : "HARD");
        else
            PrintAndLogEx(INFO, " card PRNG ..... " _YELLOW_("Could not determine PRNG,") " " _RED_("read failed."));

        PrintAndLogEx(INFO, " dictionary .... " _YELLOW_("%s"), strlen(filename) ? filename : "NONE");
        PrintAndLogEx(INFO, " legacy mode ... " _YELLOW_("%s"), legacy_mfchk ? "True" : "False");

        PrintAndLogEx(INFO, "========================================================================");
    }

    // Start the timer
    t1 = msclock();

    // check the user supplied key
    if (know_target_key == false) {
        PrintAndLogEx(WARNING, "no known key was supplied, key recovery might fail");
    } else {
        if (verbose) {
            PrintAndLogEx(INFO, "======================= " _YELLOW_("START KNOWN KEY ATTACK") " =======================");
        }

        if (mfCheckKeys(FirstBlockOfSector(blockNo), keyType, true, 1, key, &key64) == PM3_SUCCESS) {
            PrintAndLogEx(INFO, "target sector:%3u key type: %c -- using valid key [ " _GREEN_("%s") "] (used for nested / hardnested attack)",
                          blockNo,
                          keyType ? 'B' : 'A',
                          sprint_hex(key, sizeof(key))
                         );

            // Store the key for the nested / hardnested attack (if supplied by the user)
            e_sector[blockNo].Key[keyType] = key64;
            e_sector[blockNo].foundKey[keyType] = 'U';

            ++num_found_keys;
        } else {
            know_target_key = false;
            PrintAndLogEx(FAILED, "Key is wrong. Can't authenticate to sector:"_RED_("%3d") " key type: "_RED_("%c") " key: " _RED_("%s"),
                          blockNo,
                          keyType ? 'B' : 'A',
                          sprint_hex(key, sizeof(key))
                         );
            PrintAndLogEx(WARNING, "falling back to dictionary");
        }

        // Check if the user supplied key is used by other sectors
        for (int i = 0; i < sectors_cnt; i++) {
            for (int j = 0; j < 2; j++) {
                if (e_sector[i].foundKey[j] == 0) {
                    if (mfCheckKeys(FirstBlockOfSector(i), j, true, 1, key, &key64) == PM3_SUCCESS) {
                        e_sector[i].Key[j] = bytes_to_num(key, 6);
                        e_sector[i].foundKey[j] = 'U';

                        // If the user supplied secctor / keytype was wrong --> just be nice and correct it ;)
                        if (know_target_key == false) {
                            num_to_bytes(e_sector[i].Key[j], 6, key);
                            know_target_key = true;
                            blockNo = i;
                            keyType = j;
                            PrintAndLogEx(SUCCESS, "target sector:%3u key type: %c -- found valid key [ " _GREEN_("%s") "] (used for nested / hardnested attack)",
                                          i,
                                          j ? 'B' : 'A',
                                          sprint_hex(key, sizeof(key))
                                         );
                        } else {
                            PrintAndLogEx(SUCCESS, "target sector:%3u key type: %c -- found valid key [ " _GREEN_("%s") "]",
                                          i,
                                          j ? 'B' : 'A',
                                          sprint_hex(key, sizeof(key))
                                         );
                        }
                        ++num_found_keys;
                    }
                }
            }
        }

        if (num_found_keys == sectors_cnt * 2) {
            goto all_found;
        }
    }

    bool load_success = true;
    // Load the dictionary
    if (has_filename) {
        res = loadFileDICTIONARY_safe(filename, (void **) &keyBlock, 6, &key_cnt);
        if (res != PM3_SUCCESS || key_cnt == 0 || keyBlock == NULL) {
            PrintAndLogEx(FAILED, "An error occurred while loading the dictionary! (we will use the default keys now)");
            if (keyBlock != NULL) {
                free(keyBlock);
            }
            load_success = false;
        }
    }

    if (has_filename == false || load_success == false) {
        keyBlock = calloc(ARRAYLEN(g_mifare_default_keys), 6);
        if (keyBlock == NULL) {
            free(e_sector);
            free(fptr);
            return PM3_EMALLOC;
        }

        for (int cnt = 0; cnt < ARRAYLEN(g_mifare_default_keys); cnt++) {
            num_to_bytes(g_mifare_default_keys[cnt], 6, keyBlock + cnt * 6);
        }
        key_cnt = ARRAYLEN(g_mifare_default_keys);
        PrintAndLogEx(SUCCESS, "loaded " _GREEN_("%2d") " keys from hardcoded default array", key_cnt);
    }

    // Use the dictionary to find sector keys on the card
    if (verbose) PrintAndLogEx(INFO, "======================= " _YELLOW_("START DICTIONARY ATTACK") " =======================");

    if (legacy_mfchk) {
        PrintAndLogEx(INFO, "." NOLF);
        // Check all the sectors
        for (int i = 0; i < sectors_cnt; i++) {
            for (int j = 0; j < 2; j++) {
                // Check if the key is known
                if (e_sector[i].foundKey[j] == 0) {
                    for (uint32_t k = 0; k < key_cnt; k++) {
                        PrintAndLogEx(NORMAL, "." NOLF);
                        fflush(stdout);

                        if (mfCheckKeys(FirstBlockOfSector(i), j, true, 1, (keyBlock + (6 * k)), &key64) == PM3_SUCCESS) {
                            e_sector[i].Key[j] = bytes_to_num((keyBlock + (6 * k)), 6);
                            e_sector[i].foundKey[j] = 'D';
                            ++num_found_keys;
                            break;
                        }
                    }
                }
            }
        }
        PrintAndLogEx(NORMAL, "");
    } else {

        uint32_t chunksize = key_cnt > (PM3_CMD_DATA_SIZE / 6) ? (PM3_CMD_DATA_SIZE / 6) : key_cnt;
        bool firstChunk = true, lastChunk = false;

        for (uint8_t strategy = 1; strategy < 3; strategy++) {
            PrintAndLogEx(INFO, "running strategy %u", strategy);
            // main keychunk loop
            for (uint32_t i = 0; i < key_cnt; i += chunksize) {

                if (kbd_enter_pressed()) {
                    PrintAndLogEx(WARNING, "\naborted via keyboard!\n");
                    i = key_cnt;
                    strategy = 3;
                    break; // Exit the loop
                }
                uint32_t size = ((key_cnt - i)  > chunksize) ? chunksize : key_cnt - i;
                // last chunk?
                if (size == key_cnt - i)
                    lastChunk = true;

                res = mfCheckKeys_fast(sectors_cnt, firstChunk, lastChunk, strategy, size, keyBlock + (i * 6), e_sector, false);
                if (firstChunk)
                    firstChunk = false;
                // all keys,  aborted
                if (res == PM3_SUCCESS) {
                    i = key_cnt;
                    strategy = 3;
                    break; // Exit the loop
                }
            } // end chunks of keys
            firstChunk = true;
            lastChunk = false;
        } // end strategy
    }

    // Analyse the dictionary attack
    for (int i = 0; i < sectors_cnt; i++) {
        for (int j = 0; j < 2; j++) {
            if (e_sector[i].foundKey[j] == 1) {
                e_sector[i].foundKey[j] = 'D';
                num_to_bytes(e_sector[i].Key[j], 6, tmp_key);

                // Store valid credentials for the nested / hardnested attack if none exist
                if (know_target_key == false) {
                    num_to_bytes(e_sector[i].Key[j], 6, key);
                    know_target_key = true;
                    blockNo = i;
                    keyType = j;
                    PrintAndLogEx(SUCCESS, "target sector:%3u key type: %c -- found valid key [ " _GREEN_("%s") "] (used for nested / hardnested attack)",
                                  i,
                                  j ? 'B' : 'A',
                                  sprint_hex(tmp_key, sizeof(tmp_key))
                                 );
                } else {
                    PrintAndLogEx(SUCCESS, "target sector:%3u key type: %c -- found valid key [ " _GREEN_("%s") "]",
                                  i,
                                  j ? 'B' : 'A',
                                  sprint_hex(tmp_key, sizeof(tmp_key))
                                 );
                }
            }
        }
    }

    // Check if at least one sector key was found
    if (know_target_key == false) {
        // Check if the darkside attack can be used
        if (prng_type && has_staticnonce != NONCE_STATIC) {
            if (verbose) {
                PrintAndLogEx(INFO, "======================= " _YELLOW_("START DARKSIDE ATTACK") " =======================");
            }
            isOK = mfDarkside(FirstBlockOfSector(blockNo), keyType + 0x60, &key64);

            switch (isOK) {
                case -1 :
                    PrintAndLogEx(WARNING, "\nButton pressed. Aborted.");
                    goto noValidKeyFound;
                case -2 :
                    PrintAndLogEx(FAILED, "\nCard is not vulnerable to Darkside attack (doesn't send NACK on authentication requests).");
                    goto noValidKeyFound;
                case -3 :
                    PrintAndLogEx(FAILED, "\nCard is not vulnerable to Darkside attack (its random number generator is not predictable).");
                    goto noValidKeyFound;
                case -4 :
                    PrintAndLogEx(FAILED, "\nCard is not vulnerable to Darkside attack (its random number generator seems to be based on the wellknown");
                    PrintAndLogEx(FAILED, "generating polynomial with 16 effective bits only, but shows unexpected behaviour.");
                    goto noValidKeyFound;
                case -5 :
                    PrintAndLogEx(WARNING, "\nAborted via keyboard.");
                    goto noValidKeyFound;
                default :
                    PrintAndLogEx(SUCCESS, "\nFound valid key: [ " _GREEN_("%012" PRIx64) " ]\n", key64);
                    break;
            }

            // Store the keys
            num_to_bytes(key64, 6, key);
            e_sector[blockNo].Key[keyType] = key64;
            e_sector[blockNo].foundKey[keyType] = 'S';
            PrintAndLogEx(SUCCESS, "target sector:%3u key type: %c -- found valid key [ " _GREEN_("%012" PRIx64) " ] (used for nested / hardnested attack)",
                          blockNo,
                          keyType ? 'B' : 'A',
                          key64
                         );
        } else {
noValidKeyFound:
            PrintAndLogEx(FAILED, "No usable key was found!");
            free(keyBlock);
            free(e_sector);
            free(fptr);
            return PM3_ESOFT;
        }
    }

    free(keyBlock);
    // Clear the needed variables
    num_to_bytes(0, 6, tmp_key);
    bool nested_failed = false;

    // Iterate over each sector and key(A/B)
    for (current_sector_i = 0; current_sector_i < sectors_cnt; current_sector_i++) {
        for (current_key_type_i = 0; current_key_type_i < 2; current_key_type_i++) {

            // If the key is already known, just skip it
            if (e_sector[current_sector_i].foundKey[current_key_type_i] == 0) {

                // Try the found keys are reused
                if (bytes_to_num(tmp_key, 6) != 0) {
                    // <!> The fast check --> mfCheckKeys_fast(sectors_cnt, true, true, 2, 1, tmp_key, e_sector, false);
                    // <!> Returns false keys, so we just stick to the slower mfchk.
                    for (int i = 0; i < sectors_cnt; i++) {
                        for (int j = 0; j < 2; j++) {
                            // Check if the sector key is already broken
                            if (e_sector[i].foundKey[j])
                                continue;

                            // Check if the key works
                            if (mfCheckKeys(FirstBlockOfSector(i), j, true, 1, tmp_key, &key64) == PM3_SUCCESS) {
                                e_sector[i].Key[j] = bytes_to_num(tmp_key, 6);
                                e_sector[i].foundKey[j] = 'R';
                                PrintAndLogEx(SUCCESS, "target sector:%3u key type: %c -- found valid key [ " _GREEN_("%s") "]",
                                              i,
                                              j ? 'B' : 'A',
                                              sprint_hex(tmp_key, sizeof(tmp_key))
                                             );
                            }
                        }
                    }
                }
                // Clear the last found key
                num_to_bytes(0, 6, tmp_key);

                if (current_key_type_i == 1) {
                    if (e_sector[current_sector_i].foundKey[0] && !e_sector[current_sector_i].foundKey[1]) {
                        if (verbose) {
                            PrintAndLogEx(INFO, "======================= " _YELLOW_("START READ B KEY ATTACK") " =======================");
                            PrintAndLogEx(INFO, "reading  B  key: sector: %3d key type: %c",
                                          current_sector_i,
                                          current_key_type_i ? 'B' : 'A');
                        }
                        uint8_t sectrail = (FirstBlockOfSector(current_sector_i) + NumBlocksPerSector(current_sector_i) - 1);

                        mf_readblock_t payload;
                        payload.blockno = sectrail;
                        payload.keytype = 0;

                        num_to_bytes(e_sector[current_sector_i].Key[0], 6, payload.key); // KEY A

                        clearCommandBuffer();
                        SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));

                        PacketResponseNG resp;
                        if (!WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500)) goto skipReadBKey;

                        if (resp.status != PM3_SUCCESS) goto skipReadBKey;

                        uint8_t *data = resp.data.asBytes;
                        key64 = bytes_to_num(data + 10, 6);
                        if (key64) {
                            e_sector[current_sector_i].foundKey[current_key_type_i] = 'A';
                            e_sector[current_sector_i].Key[current_key_type_i] = key64;
                            num_to_bytes(key64, 6, tmp_key);
                            PrintAndLogEx(SUCCESS, "target sector:%3u key type: %c -- found valid key [ " _GREEN_("%s") "]",
                                          current_sector_i,
                                          current_key_type_i ? 'B' : 'A',
                                          sprint_hex(tmp_key, sizeof(tmp_key))
                                         );
                        } else {
                            if (verbose) {
                                PrintAndLogEx(WARNING, "unknown  B  key: sector: %3d key type: %c",
                                              current_sector_i,
                                              current_key_type_i ? 'B' : 'A'
                                             );
                                PrintAndLogEx(INFO, " -- reading the B key was not possible, maybe due to access rights?");

                            }

                        }
                    }
                }

                // Use the nested / hardnested attack
skipReadBKey:
                if (e_sector[current_sector_i].foundKey[current_key_type_i] == 0) {

                    if (has_staticnonce == NONCE_STATIC)
                        goto tryStaticnested;

                    if (prng_type && (nested_failed == false)) {
                        uint8_t retries = 0;
                        if (verbose) {
                            PrintAndLogEx(INFO, "======================= " _YELLOW_("START NESTED ATTACK") " =======================");
                            PrintAndLogEx(INFO, "sector no: %3d, target key type: %c",
                                          current_sector_i,
                                          current_key_type_i ? 'B' : 'A');
                        }
tryNested:
                        isOK = mfnested(FirstBlockOfSector(blockNo), keyType, key, FirstBlockOfSector(current_sector_i), current_key_type_i, tmp_key, calibrate);

                        switch (isOK) {
                            case PM3_ETIMEOUT: {
                                PrintAndLogEx(ERR, "\nError: No response from Proxmark3.");
                                free(e_sector);
                                free(fptr);
                                return PM3_ESOFT;
                            }
                            case PM3_EOPABORTED: {
                                PrintAndLogEx(WARNING, "\nButton pressed. Aborted.");
                                free(e_sector);
                                free(fptr);
                                return PM3_EOPABORTED;
                            }
                            case PM3_EFAILED: {
                                PrintAndLogEx(FAILED, "Tag isn't vulnerable to Nested Attack (PRNG is probably not predictable).");
                                PrintAndLogEx(FAILED, "Nested attack failed --> try hardnested");
                                goto tryHardnested;
                            }
                            case PM3_ESOFT: {
                                // key not found
                                calibrate = false;
                                // this can happen on some old cards, it's worth trying some more before switching to slower hardnested
                                if (retries++ < MIFARE_SECTOR_RETRY) {
                                    PrintAndLogEx(FAILED, "Nested attack failed, trying again (%i/%i)", retries, MIFARE_SECTOR_RETRY);
                                    goto tryNested;
                                } else {
                                    PrintAndLogEx(FAILED, "Nested attack failed, moving to hardnested");
                                    nested_failed = true;
                                    goto tryHardnested;
                                }
                                break;
                            }
                            case PM3_SUCCESS: {
                                calibrate = false;
                                e_sector[current_sector_i].Key[current_key_type_i] = bytes_to_num(tmp_key, 6);
                                e_sector[current_sector_i].foundKey[current_key_type_i] = 'N';
                                break;
                            }
                            default: {
                                PrintAndLogEx(ERR, "unknown Error.\n");
                                free(e_sector);
                                free(fptr);
                                return PM3_ESOFT;
                            }
                        }

                    } else {
tryHardnested: // If the nested attack fails then we try the hardnested attack
                        if (verbose) {
                            PrintAndLogEx(INFO, "======================= " _YELLOW_("START HARDNESTED ATTACK") " =======================");
                            PrintAndLogEx(INFO, "sector no: %3d, target key type: %c, Slow: %s",
                                          current_sector_i,
                                          current_key_type_i ? 'B' : 'A',
                                          slow ? "Yes" : "No");
                        }

                        isOK = mfnestedhard(FirstBlockOfSector(blockNo), keyType, key, FirstBlockOfSector(current_sector_i), current_key_type_i, NULL, false, false, slow, 0, &foundkey, NULL);
                        DropField();
                        if (isOK) {
                            switch (isOK) {
                                case 1: {
                                    PrintAndLogEx(ERR, "\nError: No response from Proxmark3.");
                                    break;
                                }
                                case 2: {
                                    PrintAndLogEx(NORMAL, "\nButton pressed. Aborted.");
                                    break;
                                }
                                default: {
                                    break;
                                }
                            }
                            free(e_sector);
                            free(fptr);
                            return PM3_ESOFT;
                        }

                        // Copy the found key to the tmp_key variale (for the following print statement, and the mfCheckKeys above)
                        num_to_bytes(foundkey, 6, tmp_key);
                        e_sector[current_sector_i].Key[current_key_type_i] = foundkey;
                        e_sector[current_sector_i].foundKey[current_key_type_i] = 'H';
                    }

                    if (has_staticnonce == NONCE_STATIC) {
tryStaticnested:
                        if (verbose) {
                            PrintAndLogEx(INFO, "======================= " _YELLOW_("START STATIC NESTED ATTACK") " =======================");
                            PrintAndLogEx(INFO, "sector no: %3d, target key type: %c",
                                          current_sector_i,
                                          current_key_type_i ? 'B' : 'A');
                        }

                        isOK = mfStaticNested(blockNo, keyType, key, FirstBlockOfSector(current_sector_i), current_key_type_i, tmp_key);
                        DropField();
                        switch (isOK) {
                            case PM3_ETIMEOUT: {
                                PrintAndLogEx(ERR, "\nError: No response from Proxmark3.");
                                free(e_sector);
                                free(fptr);
                                return PM3_ESOFT;
                            }
                            case PM3_EOPABORTED: {
                                PrintAndLogEx(WARNING, "\nButton pressed. Aborted.");
                                free(e_sector);
                                free(fptr);
                                return PM3_EOPABORTED;
                            }
                            case PM3_SUCCESS: {
                                e_sector[current_sector_i].Key[current_key_type_i] = bytes_to_num(tmp_key, 6);
                                e_sector[current_sector_i].foundKey[current_key_type_i] = 'C';
                                break;
                            }
                            default: {
                                break;
                            }
                        }
                    }

                    // Check if the key was found
                    if (e_sector[current_sector_i].foundKey[current_key_type_i]) {
                        PrintAndLogEx(SUCCESS, "target sector:%3u key type: %c -- found valid key [ " _GREEN_("%s") "]",
                                      current_sector_i,
                                      current_key_type_i ? 'B' : 'A',
                                      sprint_hex(tmp_key, sizeof(tmp_key))
                                     );
                    }
                }
            }
        }
    }

all_found:

    // Show the results to the user
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(SUCCESS, _GREEN_("found keys:"));

    printKeyTable(sectors_cnt, e_sector);

    // Dump the keys
    PrintAndLogEx(NORMAL, "");

    if (createMfcKeyDump(fptr, sectors_cnt, e_sector) != PM3_SUCCESS) {
        PrintAndLogEx(ERR, "Failed to save keys to file");
    }

    // clear emulator mem
    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_EML_MEMCLR, NULL, 0);

    PrintAndLogEx(SUCCESS, "transferring keys to simulator memory (Cmd Error: 04 can occur)");

    for (current_sector_i = 0; current_sector_i < sectors_cnt; current_sector_i++) {
        mfEmlGetMem(block, current_sector_i, 1);
        if (e_sector[current_sector_i].foundKey[0])
            num_to_bytes(e_sector[current_sector_i].Key[0], 6, block);
        if (e_sector[current_sector_i].foundKey[1])
            num_to_bytes(e_sector[current_sector_i].Key[1], 6, block + 10);

        mfEmlSetMem(block, FirstBlockOfSector(current_sector_i) + NumBlocksPerSector(current_sector_i) - 1, 1);
    }

    // use ecfill trick
    FastDumpWithEcFill(sectors_cnt);

    bytes = block_cnt * MFBLOCK_SIZE;
    dump = calloc(bytes, sizeof(uint8_t));
    if (!dump) {
        PrintAndLogEx(ERR, "Fail, cannot allocate memory");
        free(e_sector);
        free(fptr);
        return PM3_EMALLOC;
    }
    memset(dump, 0, bytes);

    PrintAndLogEx(INFO, "downloading the card content from emulator memory");
    if (!GetFromDevice(BIG_BUF_EML, dump, bytes, 0, NULL, 0, NULL, 2500, false)) {
        PrintAndLogEx(ERR, "Fail, transfer from device time-out");
        free(e_sector);
        free(dump);
        free(fptr);
        return PM3_ETIMEOUT;
    }

    char *fnameptr = GenerateFilename("hf-mf-", "-dump");
    if (fnameptr == NULL) {
        free(dump);
        free(e_sector);
        free(fptr);
        return PM3_ESOFT;
    }
    strcpy(filename, fnameptr);
    free(fnameptr);

    saveFile(filename, ".bin", dump, bytes);
    saveFileEML(filename, dump, bytes, MFBLOCK_SIZE);
    saveFileJSON(filename, jsfCardMemory, dump, bytes, NULL);

    // Generate and show statistics
    t1 = msclock() - t1;
    PrintAndLogEx(INFO, "autopwn execution time: " _YELLOW_("%.0f") " seconds", (float)t1 / 1000.0);

    free(dump);
    free(e_sector);
    free(fptr);
    return PM3_SUCCESS;
}

/*
static int randInRange(int min, int max) {
    return min + (int)(rand() / (double)(RAND_MAX) * (max - min + 1));
}
*/

//Fisher–Yates shuffle
/*
static void shuffle(uint8_t *array, uint16_t len) {
    uint8_t tmp[6];
    uint16_t x;
    time_t t;
    srand((unsigned) time(&t));
    while (len) {
        x = randInRange(0, (len -= 6)) | 0;  // 0 = i < n
        x %= 6;
        memcpy(tmp, array + x, 6);
        memcpy(array + x, array + len, 6);
        memcpy(array + len, tmp, 6);
    }
}
*/

static int CmdHF14AMfChk_fast(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf fchk",
                  "This is a improved checkkeys method speedwise. It checks MIFARE Classic tags sector keys against a dictionary file with keys",
                  "hf mf fchk --mini -k FFFFFFFFFFFF              --> Key recovery against MIFARE Mini\n"
                  "hf mf fchk --1k -k FFFFFFFFFFFF                --> Key recovery against MIFARE Classic 1k\n"
                  "hf mf fchk --2k -k FFFFFFFFFFFF                --> Key recovery against MIFARE 2k\n"
                  "hf mf fchk --4k -k FFFFFFFFFFFF                --> Key recovery against MIFARE 4k\n"
                  "hf mf fchk --1k -f mfc_default_keys.dic        --> Target 1K using default dictionary file\n"
                  "hf mf fchk --1k --emu                          --> Target 1K, write to emulator memory\n"
                  "hf mf fchk --1k --dump                         --> Target 1K, write to file\n"
                  "hf mf fchk --1k --mem                          --> Target 1K, use dictionary from flashmemory");

    void *argtable[] = {
        arg_param_begin,
        arg_strx0("k", "key", "<hex>", "Key specified as 12 hex symbols"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (default)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0(NULL, "emu", "Fill simulator keys from found keys"),
        arg_lit0(NULL, "dump", "Dump found keys to binary file"),
        arg_lit0(NULL, "mem", "Use dictionary from flashmemory"),
        arg_str0("f", "file", "<filename>", "filename of dictionary"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int keylen = 0;
    uint8_t key[255 * 6] = {0};
    CLIGetHexWithReturn(ctx, 1, key, &keylen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);

    bool transferToEml = arg_get_lit(ctx, 6);
    bool createDumpFile = arg_get_lit(ctx, 7);
    bool use_flashmemory = arg_get_lit(ctx, 8);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 9), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    CLIParserFree(ctx);

    //validations

    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t sectorsCnt = 1;
    if (m0) {
        sectorsCnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        sectorsCnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        sectorsCnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        sectorsCnt = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    // Handle Keys
    int keycnt = 0;
    uint32_t keyitems = ARRAYLEN(g_mifare_default_keys);
    uint8_t *keyBlock, *p;
    // Allocate memory for keys to be tested
    keyBlock = calloc(ARRAYLEN(g_mifare_default_keys), 6);
    if (keyBlock == NULL) return PM3_EMALLOC;

    // Copy default keys to list
    for (int cnt = 0; cnt < ARRAYLEN(g_mifare_default_keys); cnt++)
        num_to_bytes(g_mifare_default_keys[cnt], 6, (uint8_t *)(keyBlock + cnt * 6));

    // Handle user supplied key
    if (keylen >= 6) {
        int numKeys = keylen / 6;

        p = realloc(keyBlock, 6 * (keyitems + numKeys));
        if (!p) {
            PrintAndLogEx(FAILED, "cannot allocate memory for Keys");
            free(keyBlock);
            return PM3_EMALLOC;
        }
        keyBlock = p;

        memcpy(keyBlock + 6 * keycnt, key, 6 * numKeys);

        for (int i = 0; i < numKeys; i++) {
            PrintAndLogEx(NORMAL, "[%2d] key %s", keycnt, sprint_hex((keyBlock + 6 * keycnt), 6));
            keycnt++;
        }

    }

    // Handle user supplied dictionary file
    FILE *f;
    char buf[13];
    if (fnlen > 0) {
        char *dict_path;
        int res = searchFile(&dict_path, DICTIONARIES_SUBDIR, filename, ".dic", false);
        if (res != PM3_SUCCESS) {
            free(keyBlock);
            return PM3_EFILE;
        }
        f = fopen(dict_path, "r");
        if (!f) {
            PrintAndLogEx(FAILED, "File: " _YELLOW_("%s") ": not found or locked.", dict_path);
            free(dict_path);
            free(keyBlock);
            return PM3_EFILE;
        }
        free(dict_path);
        // load keys from dictionary file
        while (fgets(buf, sizeof(buf), f)) {
            if (strlen(buf) < 12 || buf[11] == '\n')
                continue;

            while (fgetc(f) != '\n' && !feof(f)) ;  //goto next line

            if (buf[0] == '#') continue; //The line start with # is comment, skip

            // codesmell, only checks first char?
            if (!isxdigit(buf[0])) {
                PrintAndLogEx(FAILED, "File content error. '" _YELLOW_("%s")"' must include 12 HEX symbols", buf);
                continue;
            }

            buf[12] = 0;

            if (keyitems - keycnt < 2) {
                p = realloc(keyBlock, 6 * (keyitems += 64));
                if (!p) {
                    PrintAndLogEx(FAILED, "Cannot allocate memory for defKeys");
                    free(keyBlock);
                    fclose(f);
                    return PM3_EMALLOC;
                }
                keyBlock = p;
            }
            memset(keyBlock + 6 * keycnt, 0, 6);
            num_to_bytes(strtoll(buf, NULL, 16), 6, keyBlock + 6 * keycnt);
            //PrintAndLogEx(NORMAL, "check key[%2d] %012" PRIx64, keycnt, bytes_to_num(keyBlock + 6*keycnt, 6));
            keycnt++;
            memset(buf, 0, sizeof(buf));
        }
        fclose(f);
        PrintAndLogEx(SUCCESS, "Loaded %2d keys from " _YELLOW_("%s"), keycnt, filename);
    }

    if (keycnt == 0 && !use_flashmemory) {
        PrintAndLogEx(SUCCESS, "No key specified, trying default keys");
        for (; keycnt < ARRAYLEN(g_mifare_default_keys); keycnt++)
            PrintAndLogEx(NORMAL, "[%2d] %02x%02x%02x%02x%02x%02x", keycnt,
                          (keyBlock + 6 * keycnt)[0], (keyBlock + 6 * keycnt)[1], (keyBlock + 6 * keycnt)[2],
                          (keyBlock + 6 * keycnt)[3], (keyBlock + 6 * keycnt)[4], (keyBlock + 6 * keycnt)[5]);
    }

    // create/initialize key storage structure
    sector_t *e_sector = NULL;
    int32_t res = initSectorTable(&e_sector, sectorsCnt);
    if (res != sectorsCnt) {
        free(keyBlock);
        return PM3_EMALLOC;
    }


    uint32_t chunksize = keycnt > (PM3_CMD_DATA_SIZE / 6) ? (PM3_CMD_DATA_SIZE / 6) : keycnt;
    bool firstChunk = true, lastChunk = false;

    int i = 0;
    // time
    uint64_t t1 = msclock();

    if (use_flashmemory) {
        PrintAndLogEx(SUCCESS, "Using dictionary in flash memory");
        mfCheckKeys_fast(sectorsCnt, true, true, 1, 0, keyBlock, e_sector, use_flashmemory);
    } else {

        // strategys. 1= deep first on sector 0 AB,  2= width first on all sectors
        for (uint8_t strategy = 1; strategy < 3; strategy++) {
            PrintAndLogEx(INFO, "Running strategy %u", strategy);

            // main keychunk loop
            for (i = 0; i < keycnt; i += chunksize) {

                if (kbd_enter_pressed()) {
                    PrintAndLogEx(WARNING, "\naborted via keyboard!\n");
                    goto out;
                }

                uint32_t size = ((keycnt - i)  > chunksize) ? chunksize : keycnt - i;

                // last chunk?
                if (size == keycnt - i)
                    lastChunk = true;

                res = mfCheckKeys_fast(sectorsCnt, firstChunk, lastChunk, strategy, size, keyBlock + (i * 6), e_sector, false);

                if (firstChunk)
                    firstChunk = false;

                // all keys,  aborted
                if (res == PM3_SUCCESS || res == 2)
                    goto out;
            } // end chunks of keys
            firstChunk = true;
            lastChunk = false;
        } // end strategy
    }
out:
    t1 = msclock() - t1;
    PrintAndLogEx(INFO, "time in checkkeys (fast) " _YELLOW_("%.1fs") "\n", (float)(t1 / 1000.0));

    // check..
    uint8_t found_keys = 0;
    for (i = 0; i < sectorsCnt; ++i) {

        if (e_sector[i].foundKey[0])
            found_keys++;

        if (e_sector[i].foundKey[1])
            found_keys++;
    }

    if (found_keys == 0) {
        PrintAndLogEx(WARNING, "No keys found");
    } else {

        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(SUCCESS, _GREEN_("found keys:"));

        printKeyTable(sectorsCnt, e_sector);

        if (use_flashmemory && found_keys == (sectorsCnt << 1)) {
            PrintAndLogEx(SUCCESS, "Card dumped as well. run " _YELLOW_("`%s %c`"),
                          "hf mf esave",
                          GetFormatFromSector(sectorsCnt)
                         );
        }

        if (transferToEml) {
            // fast push mode
            conn.block_after_ACK = true;
            uint8_t block[16] = {0x00};
            for (i = 0; i < sectorsCnt; ++i) {
                uint8_t blockno = FirstBlockOfSector(i) + NumBlocksPerSector(i) - 1;
                mfEmlGetMem(block, blockno, 1);

                if (e_sector[i].foundKey[0])
                    num_to_bytes(e_sector[i].Key[0], 6, block);

                if (e_sector[i].foundKey[1])
                    num_to_bytes(e_sector[i].Key[1], 6, block + 10);

                if (i == sectorsCnt - 1) {
                    // Disable fast mode on last packet
                    conn.block_after_ACK = false;
                }
                mfEmlSetMem(block, blockno, 1);
            }
            PrintAndLogEx(SUCCESS, "Found keys have been transferred to the emulator memory");

            if (found_keys == (sectorsCnt << 1)) {
                FastDumpWithEcFill(sectorsCnt);
            }
        }

        if (createDumpFile) {

            char *fptr = GenerateFilename("hf-mf-", "-key.bin");
            if (createMfcKeyDump(fptr, sectorsCnt, e_sector) != PM3_SUCCESS) {
                PrintAndLogEx(ERR, "Failed to save keys to file");
            }
            free(fptr);
        }
    }

    free(keyBlock);
    free(e_sector);
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

static int CmdHF14AMfChk(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf chk",
                  "Check keys on MIFARE Classic card",
                  "hf mf chk --mini -k FFFFFFFFFFFF              --> Check all sectors, all keys against MIFARE Mini\n"
                  "hf mf chk --1k -k FFFFFFFFFFFF                --> Check all sectors, all keys against MIFARE Classic 1k\n"
                  "hf mf chk --2k -k FFFFFFFFFFFF                --> Check all sectors, all keys against MIFARE 2k\n"
                  "hf mf chk --4k -k FFFFFFFFFFFF                --> Check all sectors, all keys against MIFARE 4k\n"
                  "hf mf chk --1k --emu                          --> Check all sectors, all keys, 1K, and write to emulator memory\n"
                  "hf mf chk --1k --dump                         --> Check all sectors, all keys, 1K, and write to file\n"
                  "hf mf chk -a --blk 0 -f mfc_default_keys.dic  --> Check dictionary against block 0, key A");

    void *argtable[] = {
        arg_param_begin,
        arg_strx0("k", "key", "<hex>", "Key specified as 12 hex symbols"),
        arg_int0(NULL, "blk", "<dec>", "Input block number"),
        arg_lit0("a", NULL, "Target Key A, if found also check Key B for duplicate"),
        arg_lit0("b", NULL, "Target Key B"),
        arg_lit0("*", "all", "Target both key A & B (default)"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (default)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0(NULL, "emu", "Fill simulator keys from found keys"),
        arg_lit0(NULL, "dump", "Dump found keys to binary file"),
        arg_str0("f", "file", "<filename>", "filename of dictionary"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int keylen = 0;
    uint8_t key[255 * 6] = {0};
    CLIGetHexWithReturn(ctx, 1, key, &keylen);

    int blockNo = arg_get_int_def(ctx, 2, -1);

    uint8_t keyType = 2;

    if ((arg_get_lit(ctx, 3) && arg_get_lit(ctx, 4)) || arg_get_lit(ctx, 5)) {
        keyType = 2;
    } else if (arg_get_lit(ctx, 3)) {
        keyType = 0;
    } else if (arg_get_lit(ctx, 4)) {
        keyType = 1;
    }

    bool m0 = arg_get_lit(ctx, 6);
    bool m1 = arg_get_lit(ctx, 7);
    bool m2 = arg_get_lit(ctx, 8);
    bool m4 = arg_get_lit(ctx, 9);

    bool transferToEml = arg_get_lit(ctx, 10);
    bool createDumpFile = arg_get_lit(ctx, 11);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 12), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    CLIParserFree(ctx);

    //validations

    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    }

    uint8_t SectorsCnt = 1;
    if (m0) {
        SectorsCnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        SectorsCnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        SectorsCnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        SectorsCnt = MIFARE_4K_MAXSECTOR;
    }

    if ((blockNo == -1) && (SectorsCnt == 1)) {
        // Default to 1K if block number not specified
        SectorsCnt = MIFARE_1K_MAXSECTOR;
    }

    if (blockNo > -1) {
        if (SectorsCnt == 0) {
            PrintAndLogEx(WARNING, "Invalid MIFARE Type");
            return PM3_EINVARG;
        }
    } else {
        blockNo = 3;
    }

    // Handle Keys
    int keycnt = 0;
    uint32_t keyitems = ARRAYLEN(g_mifare_default_keys);
    uint8_t *keyBlock, *p;
    // Allocate memory for keys to be tested
    keyBlock = calloc(ARRAYLEN(g_mifare_default_keys), 6);
    if (keyBlock == NULL) return PM3_EMALLOC;

    // Copy default keys to list
    for (int cnt = 0; cnt < ARRAYLEN(g_mifare_default_keys); cnt++)
        num_to_bytes(g_mifare_default_keys[cnt], 6, (uint8_t *)(keyBlock + cnt * 6));

    // Handle user supplied key
    if (keylen >= 6) {
        int numKeys = keylen / 6;

        p = realloc(keyBlock, 6 * (keyitems + numKeys));
        if (!p) {
            PrintAndLogEx(FAILED, "cannot allocate memory for Keys");
            free(keyBlock);
            return PM3_EMALLOC;
        }
        keyBlock = p;

        memcpy(keyBlock + 6 * keycnt, key, 6 * numKeys);

        for (int i = 0; i < numKeys; i++) {
            PrintAndLogEx(NORMAL, "[%2d] key %s", keycnt, sprint_hex((keyBlock + 6 * keycnt), 6));
            keycnt++;
        }

    }

    // Handle user supplied dictionary file
    FILE *f;
    char buf[13];
    if (fnlen > 0) {
        char *dict_path;
        int res = searchFile(&dict_path, DICTIONARIES_SUBDIR, filename, ".dic", false);
        if (res != PM3_SUCCESS) {
            free(keyBlock);
            return PM3_EFILE;
        }
        f = fopen(dict_path, "r");
        if (!f) {
            PrintAndLogEx(FAILED, "File: " _YELLOW_("%s") ": not found or locked.", dict_path);
            free(dict_path);
            free(keyBlock);
            return PM3_EFILE;
        }
        free(dict_path);
        // load keys from dictionary file
        while (fgets(buf, sizeof(buf), f)) {
            if (strlen(buf) < 12 || buf[11] == '\n')
                continue;

            while (fgetc(f) != '\n' && !feof(f)) ;  //goto next line

            if (buf[0] == '#') continue; //The line start with # is comment, skip

            // codesmell, only checks first char?
            if (!isxdigit(buf[0])) {
                PrintAndLogEx(FAILED, "File content error. '" _YELLOW_("%s")"' must include 12 HEX symbols", buf);
                continue;
            }

            buf[12] = 0;

            if (keyitems - keycnt < 2) {
                p = realloc(keyBlock, 6 * (keyitems += 64));
                if (!p) {
                    PrintAndLogEx(FAILED, "Cannot allocate memory for defKeys");
                    free(keyBlock);
                    fclose(f);
                    return PM3_EMALLOC;
                }
                keyBlock = p;
            }
            memset(keyBlock + 6 * keycnt, 0, 6);
            num_to_bytes(strtoll(buf, NULL, 16), 6, keyBlock + 6 * keycnt);
            //PrintAndLogEx(NORMAL, "check key[%2d] %012" PRIx64, keycnt, bytes_to_num(keyBlock + 6*keycnt, 6));
            keycnt++;
            memset(buf, 0, sizeof(buf));
        }
        fclose(f);
        PrintAndLogEx(SUCCESS, "Loaded %2d keys from " _YELLOW_("%s"), keycnt, filename);
    }

    uint64_t key64 = 0;

    if (keycnt == 0) {
        PrintAndLogEx(INFO, "No key specified, trying default keys");
        for (; keycnt < ARRAYLEN(g_mifare_default_keys); keycnt++)
            PrintAndLogEx(NORMAL, "[%2d] %02x%02x%02x%02x%02x%02x", keycnt,
                          (keyBlock + 6 * keycnt)[0],
                          (keyBlock + 6 * keycnt)[1],
                          (keyBlock + 6 * keycnt)[2],
                          (keyBlock + 6 * keycnt)[3],
                          (keyBlock + 6 * keycnt)[4],
                          (keyBlock + 6 * keycnt)[5]
                         );
    }

    // create/initialize key storage structure
    sector_t *e_sector = NULL;
    int32_t res = initSectorTable(&e_sector, SectorsCnt);
    if (res != SectorsCnt) {
        free(keyBlock);
        return PM3_EMALLOC;
    }

    uint8_t trgKeyType = 0;
    uint16_t max_keys = keycnt > KEYS_IN_BLOCK ? KEYS_IN_BLOCK : keycnt;

    PrintAndLogEx(INFO, "Start check for keys...");
    PrintAndLogEx(INFO, "." NOLF);

    // fast push mode
    conn.block_after_ACK = true;

    // clear trace log by first check keys call only
    bool clearLog = true;

    // time
    uint64_t t1 = msclock();

    // check keys.
    for (trgKeyType = (keyType == 2) ? 0 : keyType; trgKeyType < 2; (keyType == 2) ? (++trgKeyType) : (trgKeyType = 2)) {

        // loop sectors but block is used as to keep track of from which blocks to test
        int b = blockNo;
        for (int i = 0; i < SectorsCnt; ++i) {

            // skip already found keys.
            if (e_sector[i].foundKey[trgKeyType]) continue;

            for (uint16_t c = 0; c < keycnt; c += max_keys) {

                PrintAndLogEx(NORMAL, "." NOLF);
                fflush(stdout);

                if (kbd_enter_pressed()) {
                    PrintAndLogEx(INFO, "\naborted via keyboard!\n");
                    goto out;
                }

                uint16_t size = keycnt - c > max_keys ? max_keys : keycnt - c;

                if (mfCheckKeys(b, trgKeyType, clearLog, size, &keyBlock[6 * c], &key64) == PM3_SUCCESS) {
                    e_sector[i].Key[trgKeyType] = key64;
                    e_sector[i].foundKey[trgKeyType] = true;
                    clearLog = false;
                    break;
                }
                clearLog = false;
            }
            b < 127 ? (b += 4) : (b += 16);
        }
    }
    t1 = msclock() - t1;
    PrintAndLogEx(INFO, "\ntime in checkkeys " _YELLOW_("%.0f") " seconds\n", (float)t1 / 1000.0);

    // 20160116 If Sector A is found, but not Sector B,  try just reading it of the tag?
    if (keyType != 1) {
        PrintAndLogEx(INFO, "testing to read key B...");

        // loop sectors but block is used as to keep track of from which blocks to test
        int b = blockNo;
        for (int i = 0; i < SectorsCnt; i++) {

            // KEY A but not KEY B
            if (e_sector[i].foundKey[0] && !e_sector[i].foundKey[1]) {

                uint8_t s = GetSectorFromBlockNo(b);
                uint8_t sectrail = (FirstBlockOfSector(s) + NumBlocksPerSector(s) - 1);
                PrintAndLogEx(INFO, "Sector %u, First block of sector %u, Num of block %u", s, FirstBlockOfSector(s), NumBlocksPerSector(s));
                PrintAndLogEx(INFO, "Reading block %d", sectrail);

                mf_readblock_t payload;
                payload.blockno = sectrail;
                payload.keytype = 0;

                // Use key A
                num_to_bytes(e_sector[i].Key[0], 6, payload.key);

                clearCommandBuffer();
                SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));

                PacketResponseNG resp;
                if (!WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500)) continue;

                if (resp.status != PM3_SUCCESS) continue;

                uint8_t *data = resp.data.asBytes;
                key64 = bytes_to_num(data + 10, 6);
                if (key64) {
                    PrintAndLogEx(NORMAL, "Data:%s", sprint_hex(data + 10, 6));
                    e_sector[i].foundKey[1] = 1;
                    e_sector[i].Key[1] = key64;
                }
            }
            b < 127 ? (b += 4) : (b += 16);
        }
    }

out:
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(SUCCESS, _GREEN_("found keys:"));

    //print keys
    if (SectorsCnt == 1)
        printKeyTableEx(SectorsCnt, e_sector, GetSectorFromBlockNo(blockNo));
    else
        printKeyTable(SectorsCnt, e_sector);

    if (transferToEml) {
        // fast push mode
        conn.block_after_ACK = true;
        uint8_t block[16] = {0x00};
        for (int i = 0; i < SectorsCnt; ++i) {
            uint8_t blockno = FirstBlockOfSector(i) + NumBlocksPerSector(i) - 1;
            mfEmlGetMem(block, blockno, 1);

            if (e_sector[i].foundKey[0])
                num_to_bytes(e_sector[i].Key[0], 6, block);

            if (e_sector[i].foundKey[1])
                num_to_bytes(e_sector[i].Key[1], 6, block + 10);

            if (i == SectorsCnt - 1) {
                // Disable fast mode on last packet
                conn.block_after_ACK = false;
            }
            mfEmlSetMem(block, blockno, 1);
        }
        PrintAndLogEx(SUCCESS, "Found keys have been transferred to the emulator memory");
    }

    if (createDumpFile) {
        char *fptr = GenerateFilename("hf-mf-", "-key.bin");
        if (createMfcKeyDump(fptr, SectorsCnt, e_sector) != PM3_SUCCESS) {
            PrintAndLogEx(ERR, "Failed to save keys to file");
        }
        free(fptr);
    }

    free(keyBlock);
    free(e_sector);

    // Disable fast mode and send a dummy command to make it effective
    conn.block_after_ACK = false;
    SendCommandNG(CMD_PING, NULL, 0);
    if (!WaitForResponseTimeout(CMD_PING, NULL, 1000)) {
        PrintAndLogEx(WARNING, "command execution time out");
        return PM3_ETIMEOUT;
    }

    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

void showSectorTable(sector_t *k_sector, uint8_t k_sectorsCount) {
    if (k_sector != NULL) {
        printKeyTable(k_sectorsCount, k_sector);
        free(k_sector);
    }
}

void readerAttack(sector_t *k_sector, uint8_t k_sectorsCount, nonces_t data, bool setEmulatorMem, bool verbose) {

    uint64_t key = 0;
    bool success = false;

    if (k_sector == NULL) {
        int32_t res = initSectorTable(&k_sector, k_sectorsCount);
        if (res != k_sectorsCount) {
            free(k_sector);
            return;
        }
    }

    success = mfkey32_moebius(&data, &key);
    if (success) {
        uint8_t sector = data.sector;
        uint8_t keytype = data.keytype;

        PrintAndLogEx(INFO, "Reader is trying authenticate with: Key %s, sector %02d: [%012" PRIx64 "]"
                      , keytype ? "B" : "A"
                      , sector
                      , key
                     );

        k_sector[sector].Key[keytype] = key;
        k_sector[sector].foundKey[keytype] = true;

        //set emulator memory for keys
        if (setEmulatorMem) {
            uint8_t memBlock[16] = {0, 0, 0, 0, 0, 0, 0xff, 0x0F, 0x80, 0x69, 0, 0, 0, 0, 0, 0};
            num_to_bytes(k_sector[sector].Key[0], 6, memBlock);
            num_to_bytes(k_sector[sector].Key[1], 6, memBlock + 10);
            //iceman,  guessing this will not work so well for 4K tags.
            PrintAndLogEx(INFO, "Setting Emulator Memory Block %02d: [%s]"
                          , (sector * 4) + 3
                          , sprint_hex(memBlock, sizeof(memBlock))
                         );
            mfEmlSetMem(memBlock, (sector * 4) + 3, 1);
        }
    }

    free(k_sector);
}

static int CmdHF14AMfSim(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf sim",
                  "Simulate MIFARE card",
                  "hf mf sim --mini                         --> MIFARE Mini\n"
                  "hf mf sim --1k                           --> MIFARE Classic 1k (default)\n"
                  "hf mf sim --1k -u 0a0a0a0a               --> MIFARE Classic 1k with 4b UID\n"
                  "hf mf sim --1k -u 11223344556677         --> MIFARE Classic 1k with 7b UID\n"
                  "hf mf sim --1k -u 11223344 -i --crack    --> Perform reader attack in interactive mode\n"
                  "hf mf sim --2k                           --> MIFARE 2k\n"
                  "hf mf sim --4k                           --> MIFARE 4k");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("u", "uid", "<hex>", "UID 4,7 or 10bytes. If not specified, the UID 4b/7b from emulator memory will be used"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_str0(NULL, "atqa", "<hex>", "Provide explicit ATQA (2 bytes, overrides option t)"),
        arg_str0(NULL, "sak", "<hex>", "Provide explicit SAK (1 bytes, overrides option t)"),
        arg_int0("n", "num", "<dec> ", "Automatically exit simulation after <numreads> blocks have been read by reader. 0 = infinite"),
        arg_lit0("i", "interactive", "Console will not be returned until simulation finishes or is aborted"),
        arg_lit0(NULL, "crack", "Performs the 'reader attack', nr/ar attack against a reader"),
        arg_lit0("e", "emukeys", "Fill simulator keys from found keys"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    uint16_t flags = 0;

    int uidlen = 0;
    uint8_t uid[10] = {0};
    CLIGetHexWithReturn(ctx, 1, uid, &uidlen);

    char uidsize[8] = {0};
    if (uidlen > 0) {
        switch (uidlen) {
            case 10:
                flags |= FLAG_10B_UID_IN_DATA;
                snprintf(uidsize, sizeof(uidsize), "10 byte");
                break;
            case 7:
                flags |= FLAG_7B_UID_IN_DATA;
                snprintf(uidsize, sizeof(uidsize), "7 byte");
                break;
            case 4:
                flags |= FLAG_4B_UID_IN_DATA;
                snprintf(uidsize, sizeof(uidsize), "4 byte");
                break;
            default:
                PrintAndLogEx(WARNING, "Invalid parameter for UID");
                return PM3_EINVARG;
        }
    }

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);

    int atqalen = 0;
    uint8_t atqa[2] = {0};
    CLIGetHexWithReturn(ctx, 6, atqa, &atqalen);

    int saklen = 0;
    uint8_t sak[1] = {0};
    CLIGetHexWithReturn(ctx, 7, sak, &saklen);

    uint8_t exitAfterNReads = arg_get_u32_def(ctx, 8, 0);

    if (arg_get_lit(ctx, 9)) {
        flags |= FLAG_INTERACTIVE;
    }

    if (arg_get_lit(ctx, 10)) {
        flags |= FLAG_NR_AR_ATTACK;
    }

    bool setEmulatorMem = arg_get_lit(ctx, 11);
    bool verbose = arg_get_lit(ctx, 12);

    CLIParserFree(ctx);

    nonces_t data[1];

    sector_t *k_sector = NULL;

    //Validations
    if (atqalen > 0) {
        if (atqalen != 2) {
            PrintAndLogEx(WARNING, "Wrong ATQA length");
            return PM3_EINVARG;

        }
        flags |= FLAG_FORCED_ATQA;
    }
    if (saklen > 0) {
        if (saklen != 1) {
            PrintAndLogEx(WARNING, "Wrong SAK length");
            return PM3_EINVARG;

        }
        flags |= FLAG_FORCED_SAK;
    }

    // Use UID, SAK, ATQA from EMUL, if uid not defined
    if ((flags & (FLAG_4B_UID_IN_DATA | FLAG_7B_UID_IN_DATA | FLAG_10B_UID_IN_DATA)) == 0) {
        flags |= FLAG_UID_IN_EMUL;
    }

    uint8_t k_sectorsCount = 40;
    char csize[13] = { 0 };

    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    }

    if (m0) {
        flags |= FLAG_MF_MINI;
        snprintf(csize, sizeof(csize), "MINI");
        k_sectorsCount = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        flags |= FLAG_MF_1K;
        snprintf(csize, sizeof(csize), "1K");
        k_sectorsCount = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        flags |= FLAG_MF_2K;
        snprintf(csize, sizeof(csize), "2K with RATS");
        k_sectorsCount = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        flags |= FLAG_MF_4K;
        snprintf(csize, sizeof(csize), "4K");
        k_sectorsCount = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    PrintAndLogEx(INFO, _YELLOW_("MIFARE %s") " | %s UID  " _YELLOW_("%s") ""
                  , csize
                  , uidsize
                  , (uidlen == 0) ? "N/A" : sprint_hex(uid, uidlen)
                 );

    PrintAndLogEx(INFO, "Options [ numreads: %d, flags: %d (0x%02x) ]"
                  , exitAfterNReads
                  , flags
                  , flags);

    struct {
        uint16_t flags;
        uint8_t exitAfter;
        uint8_t uid[10];
        uint16_t atqa;
        uint8_t sak;
    } PACKED payload;

    payload.flags = flags;
    payload.exitAfter = exitAfterNReads;
    memcpy(payload.uid, uid, uidlen);
    payload.atqa = (atqa[1] << 8) | atqa[0];
    payload.sak = sak[0];

    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_SIMULATE, (uint8_t *)&payload, sizeof(payload));
    PacketResponseNG resp;

    if (flags & FLAG_INTERACTIVE) {
        PrintAndLogEx(INFO, "Press pm3-button or send another cmd to abort simulation");

        while (!kbd_enter_pressed()) {
            if (!WaitForResponseTimeout(CMD_ACK, &resp, 1500)) continue;
            if (!(flags & FLAG_NR_AR_ATTACK)) break;
            if ((resp.oldarg[0] & 0xffff) != CMD_HF_MIFARE_SIMULATE) break;

            memcpy(data, resp.data.asBytes, sizeof(data));
            readerAttack(k_sector, k_sectorsCount, data[0], setEmulatorMem, verbose);
        }
        showSectorTable(k_sector, k_sectorsCount);
    }
    return PM3_SUCCESS;
}

/*
static int CmdHF14AMfKeyBrute(const char *Cmd) {

    uint8_t blockNo = 0, keytype = 0;
    uint8_t key[6] = {0, 0, 0, 0, 0, 0};
    uint64_t foundkey = 0;

    char cmdp = tolower(param_getchar(Cmd, 0));
    if (cmdp == 'h') return usage_hf14_keybrute();

    // block number
    blockNo = param_get8(Cmd, 0);

    // keytype
    cmdp = tolower(param_getchar(Cmd, 1));
    if (cmdp == 'b') keytype = 1;

    // key
    if (param_gethex(Cmd, 2, key, 12)) return usage_hf14_keybrute();

    uint64_t t1 = msclock();

    if (mfKeyBrute(blockNo, keytype, key, &foundkey))
        PrintAndLogEx(SUCCESS, "found valid key: %012" PRIx64 " \n", foundkey);
    else
        PrintAndLogEx(FAILED, "key not found");

    t1 = msclock() - t1;
    PrintAndLogEx(SUCCESS, "\ntime in keybrute " _YELLOW_("%.0f") " seconds\n", (float)t1 / 1000.0);
    return PM3_SUCCESS;
}
*/

void printKeyTable(uint8_t sectorscnt, sector_t *e_sector) {
    return printKeyTableEx(sectorscnt, e_sector, 0);
}
void printKeyTableEx(uint8_t sectorscnt, sector_t *e_sector, uint8_t start_sector) {
    char strA[12 + 1] = {0};
    char strB[12 + 1] = {0};
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(SUCCESS, "|-----|----------------|---|----------------|---|");
    PrintAndLogEx(SUCCESS, "| Sec | key A          |res| key B          |res|");
    PrintAndLogEx(SUCCESS, "|-----|----------------|---|----------------|---|");
    for (uint8_t i = 0; i < sectorscnt; i++) {

        snprintf(strA, sizeof(strA), "------------");
        snprintf(strB, sizeof(strB), "------------");

        if (e_sector[i].foundKey[0])
            snprintf(strA, sizeof(strA), "%012" PRIx64, e_sector[i].Key[0]);

        if (e_sector[i].foundKey[1])
            snprintf(strB, sizeof(strB), "%012" PRIx64, e_sector[i].Key[1]);

        if (e_sector[i].foundKey[0] > 1) {
            PrintAndLogEx(SUCCESS, "| "_YELLOW_("%03d")" | " _GREEN_("%s")"   | " _YELLOW_("%c")" | " _GREEN_("%s")"   | " _YELLOW_("%c")" |"
                          , i
                          , strA, e_sector[i].foundKey[0]
                          , strB, e_sector[i].foundKey[1]
                         );
        } else {

            // keep track if we use start_sector or i...
            uint8_t s = start_sector;
            if (start_sector == 0)
                s = i;

            PrintAndLogEx(SUCCESS, "| "_YELLOW_("%03d")" | " _GREEN_("%s")"   | " _YELLOW_("%d")" | " _GREEN_("%s")"   | " _YELLOW_("%d")" |"
                          , s
                          , strA, e_sector[i].foundKey[0]
                          , strB, e_sector[i].foundKey[1]
                         );
        }
    }
    PrintAndLogEx(SUCCESS, "|-----|----------------|---|----------------|---|");

    if (e_sector[0].foundKey[0] > 1) {
        PrintAndLogEx(INFO, "( "
                      _YELLOW_("D") ":Dictionary / "
                      _YELLOW_("S") ":darkSide / "
                      _YELLOW_("U") ":User / "
                      _YELLOW_("R") ":Reused / "
                      _YELLOW_("N") ":Nested / "
                      _YELLOW_("H") ":Hardnested / "
                      _YELLOW_("C") ":statiCnested / "
                      _YELLOW_("A") ":keyA "
                      ")"
                     );
    } else {
        PrintAndLogEx(SUCCESS, "( " _YELLOW_("0") ":Failed / " _YELLOW_("1") ":Success)");
    }
    PrintAndLogEx(NORMAL, "");
}


// EMULATOR COMMANDS
static int CmdHF14AMfEGetBlk(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf egetblk",
                  "Get emulator memory block",
                  "hf mf egetblk --blk 0      -> get block 0 (manufacturer)\n"
                  "hf mf egetblk --blk 3 -v   -> get block 3, decode sector trailer\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("b",  "blk", "<dec>", "block number"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int b = arg_get_int_def(ctx, 1, 0);
    bool verbose = arg_get_lit(ctx, 2);
    CLIParserFree(ctx); 

    if (b > 255) {
        return PM3_EINVARG;
    }
    uint8_t blockno = (uint8_t)b;

    uint8_t data[16] = {0x00};
    if (mfEmlGetMem(data, blockno, 1) == PM3_SUCCESS) {

        uint8_t sector = GetSectorFromBlockNo(blockno);
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "  # | data  - sector %02d / 0x%02X                        | ascii", sector, sector);
        PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");
        mf_print_block(blockno, data);
    }
    if (verbose) {
        decode_print_st(blockno, data);
    } else {
        PrintAndLogEx(NORMAL, "");
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfEGetSc(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf egetsc",
                  "Get emulator memory sector",
                  "hf mf egetsc -s 0"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("s",  "sec", "<dec>", "sector number"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int s = arg_get_int_def(ctx, 1, 0);
    bool verbose = arg_get_lit(ctx, 2);
    CLIParserFree(ctx); 

    if (s > 39) {
        PrintAndLogEx(WARNING, "Sector number must be less then 40");
        return PM3_EINVARG;
    }

    uint8_t sector = (uint8_t)s;

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "  # | data  - sector %02d / 0x%02X                        | ascii", sector, sector);
    PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");

    uint8_t blocks = NumBlocksPerSector(sector);
    uint8_t start = FirstBlockOfSector(sector);

    uint8_t data[16] = {0};
    for (int i = 0; i < blocks; i++) {
        int res = mfEmlGetMem(data, start + i, 1);
        if (res == PM3_SUCCESS) {
            mf_print_block(start + i, data);
        }
    }
    if (verbose) {
        decode_print_st(start + blocks - 1, data);
    } else {
        PrintAndLogEx(NORMAL, "");
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfEClear(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf eclr",
                  "It set card emulator memory to empty data blocks and key A/B FFFFFFFFFFFF",
                  "hf mf eclr"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx); 
    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_EML_MEMCLR, NULL, 0);
    return PM3_SUCCESS;
}

static int CmdHF14AMfESet(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf esetblk",
                  "Set emulator memory block",
                  "hf mf esetblk --blk 1 -d 000102030405060708090a0b0c0d0e0f"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("b", "blk", "<dec>", "block number"),
        arg_str0("d", "data", "<hex>", "bytes to write, 16 hex bytes"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int b = arg_get_int_def(ctx, 1, 0);

    uint8_t data[16] = {0x00};
    int datalen = 0;
    int res = CLIParamHexToBuf(arg_get_str(ctx, 2), data, sizeof(data), &datalen);
    CLIParserFree(ctx); 
    if (res) {
        PrintAndLogEx(FAILED, "Error parsing bytes");
        return PM3_EINVARG;
    }

    if (b > 255) {
        return PM3_EINVARG;
    }

    if (datalen != sizeof(data)) {
        PrintAndLogEx(WARNING, "block data must include 16 HEX bytes. Got %i", datalen);
        return PM3_EINVARG;
    }

    //  1 - blocks count
    return mfEmlSetMem(data, b, 1);
}

int CmdHF14AMfELoad(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf eload",
                  "Load emulator memory with data from (bin/eml/json) dump file",
                  "hf mf eload -f hf-mf-01020304.bin\n"
                  "hf mf eload --4k -f hf-mf-01020304.eml\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str1("f", "file", "<fn>", "filename of dump"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0(NULL, "ul", "MIFARE Ultralight family"),
        arg_int0("q", "qty", "<dec>", "manually set number of blocks (overrides)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    
    int fnlen = 0;
    char filename[FILE_PATH_SIZE];
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);
    bool mu = arg_get_lit(ctx, 6);

    int numblks = arg_get_int_def(ctx, 7, -1);

    CLIParserFree(ctx); 

    // validations
    if ((m0 + m1 + m2 + m4 + mu) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4 + mu) == 0) {
        m1 = true;
    }

    uint8_t block_width = 16;
    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;

    if (m0) {
        block_cnt = MIFARE_MINI_MAXBLOCK;
    } else if (m1) {
        block_cnt = MIFARE_1K_MAXBLOCK;
    } else if (m2) {
        block_cnt = MIFARE_2K_MAXBLOCK;
    } else if (m4) {
        block_cnt = MIFARE_4K_MAXBLOCK;
    } else if (mu) {
        block_cnt = 255;
        block_width = 4;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    PrintAndLogEx(INFO, "%d blocks ( %u bytes ) to upload", block_cnt, block_cnt * block_width);

    if (numblks > 0) {
        block_cnt = MIN(numblks, block_cnt);
        PrintAndLogEx(INFO, "overriding number of blocks, will use %d blocks ( %u bytes )", block_cnt, block_cnt * block_width);
    }

    uint8_t *data = calloc(MFBLOCK_SIZE * MIFARE_4K_MAXBLOCK, sizeof(uint8_t));
    if (data == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }

    size_t datalen = 0;
    int res = PM3_SUCCESS;
    DumpFileType_t dftype = getfiletype(filename);
    switch (dftype) {
        case BIN: {
            res = loadFile_safe(filename, ".bin", (void **)&data, &datalen);
            break;
        }
        case EML: {
            res = loadFileEML_safe(filename, (void **)&data, &datalen);
            break;
        }
        case JSON: {
            res = loadFileJSON(filename, data, MIFARE_4K_MAXBLOCK * MFBLOCK_SIZE, &datalen, NULL);
            break;
        }
        case DICTIONARY: {
            PrintAndLogEx(ERR, "Error: Only BIN/JSON/EML formats allowed");
            return PM3_EINVARG;
        }
    }

    if (res != PM3_SUCCESS) {
        free(data);
        return PM3_EFILE;
    }

    // 64 or 256 blocks.
    if ((datalen % block_width) != 0) {
        PrintAndLogEx(FAILED, "File content error. Size doesn't match blockwidth ");
        free(data);
        return PM3_ESOFT;
    }

    // convert plain or old mfu format to new format
    if (block_width == 4) {
        res = convert_mfu_dump_format(&data, &datalen, true);
        if (res != PM3_SUCCESS) {
            PrintAndLogEx(FAILED, "Failed convert on load to new Ultralight/NTAG format");
            free(data);
            return res;
        }

        mfu_dump_t *mfu_dump = (mfu_dump_t *)data;
        printMFUdumpEx(mfu_dump, mfu_dump->pages + 1, 0);

        // update expected blocks to match converted data.
        block_cnt = datalen / 4;
        PrintAndLogEx(INFO, "MIFARE Ultralight override, will use %d blocks ( %u bytes )", block_cnt, block_cnt * block_width);
    }

    PrintAndLogEx(INFO, "Uploading to emulator memory");
    PrintAndLogEx(INFO, "." NOLF);

    // fast push mode
    conn.block_after_ACK = true;

    size_t offset = 0;
    int cnt = 0;

    while (datalen && cnt < block_cnt) {
        if (datalen == block_width) {
            // Disable fast mode on last packet
            conn.block_after_ACK = false;
        }

        if (mfEmlSetMem_xt(data + offset, cnt, 1, block_width) != PM3_SUCCESS) {
            PrintAndLogEx(FAILED, "Can't set emulator mem at block: %3d", cnt);
            free(data);
            return PM3_ESOFT;
        }
        PrintAndLogEx(NORMAL, "." NOLF);
        fflush(stdout);

        cnt++;
        offset += block_width;
        datalen -= block_width;
    }
    free(data);
    PrintAndLogEx(NORMAL, "\n");

    if (block_width == 4) {
        PrintAndLogEx(HINT, "You are ready to simulate. See " _YELLOW_("`hf mfu sim h`"));
        // MFU / NTAG
        if ((cnt != block_cnt)) {
            PrintAndLogEx(WARNING, "Warning, Ultralight/Ntag file content, Loaded %d blocks of expected %d blocks into emulator memory", cnt, block_cnt);
            return PM3_SUCCESS;
        }
    } else {
        PrintAndLogEx(HINT, "You are ready to simulate. See " _YELLOW_("`hf mf sim -h`"));
        // MFC
        if ((cnt != block_cnt)) {
            PrintAndLogEx(WARNING, "Error, file content, Only loaded %d blocks, must be %d blocks into emulator memory", cnt, block_cnt);
            return PM3_SUCCESS;
        }
    }
    PrintAndLogEx(INFO, "Done!");
    return PM3_SUCCESS;
}

static int CmdHF14AMfESave(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf esave",
                  "Save emulator memory into three files (BIN/EML/JSON) ",
                  "hf mf esave\n"
                  "hf mf esave --4k\n"
                  "hf mf esave --4k -f hf-mf-01020304.eml"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("f", "file", "<fn>", "filename of dump"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    
    int fnlen = 0;
    char filename[FILE_PATH_SIZE];
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);
    CLIParserFree(ctx); 

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

   uint16_t block_cnt = MIFARE_1K_MAXBLOCK;

    if (m0) {
        block_cnt = MIFARE_MINI_MAXBLOCK;
    } else if (m1) {
        block_cnt = MIFARE_1K_MAXBLOCK;
    } else if (m2) {
        block_cnt = MIFARE_2K_MAXBLOCK;
    } else if (m4) {
        block_cnt = MIFARE_4K_MAXBLOCK;
    }

    int bytes = block_cnt * MFBLOCK_SIZE;

    // reserv memory
    uint8_t *dump = calloc(bytes, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }
    memset(dump, 0, bytes);

    PrintAndLogEx(INFO, "downloading %u bytes from emulator memory", bytes);
    if (!GetFromDevice(BIG_BUF_EML, dump, bytes, 0, NULL, 0, NULL, 2500, false)) {
        PrintAndLogEx(WARNING, "Fail, transfer from device time-out");
        free(dump);
        return PM3_ETIMEOUT;
    }

    // user supplied filename?
    if (fnlen < 1) {
        char *fptr = filename;
        fptr += snprintf(fptr, sizeof(filename), "hf-mf-");
        FillFileNameByUID(fptr, dump, "-dump", 4);
    }

    saveFile(filename, ".bin", dump, bytes);
    saveFileEML(filename, dump, bytes, MFBLOCK_SIZE);
    saveFileJSON(filename, jsfCardMemory, dump, bytes, NULL);
    free(dump);
    return PM3_SUCCESS;
}

static int CmdHF14AMfEView(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf eview",
                  "It displays emulator memory",
                  "hf mf eview\n"
                  "hf mf eview --4k"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool m0 = arg_get_lit(ctx, 1);
    bool m1 = arg_get_lit(ctx, 2);
    bool m2 = arg_get_lit(ctx, 3);
    bool m4 = arg_get_lit(ctx, 4);
    CLIParserFree(ctx); 

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;

    if (m0) {
        block_cnt = MIFARE_MINI_MAXBLOCK;
    } else if (m1) {
        block_cnt = MIFARE_1K_MAXBLOCK;
    } else if (m2) {
        block_cnt = MIFARE_2K_MAXBLOCK;
    } else if (m4) {
        block_cnt = MIFARE_4K_MAXBLOCK;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    int bytes = block_cnt * MFBLOCK_SIZE;

    uint8_t *dump = calloc(bytes, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }
    memset(dump, 0, bytes);

    PrintAndLogEx(INFO, "downloading emulator memory");
    if (!GetFromDevice(BIG_BUF_EML, dump, bytes, 0, NULL, 0, NULL, 2500, false)) {
        PrintAndLogEx(WARNING, "Fail, transfer from device time-out");
        free(dump);
        return PM3_ETIMEOUT;
    }

    mf_print_blocks(block_cnt, dump);
    free(dump);
    return PM3_SUCCESS;
}

static int CmdHF14AMfECFill(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf ecfill",
                  "Dump card and transfer the data to emulator memory.\n"
                  "Keys must be laid in the emulator memory",
                  "hf mf ecfill          --> use key type A\n"
                  "hf mf ecfill --4k -b  --> target 4K card with key type B"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("a", NULL, "input key type is key A(def)"),
        arg_lit0("b", NULL, "input key type is key B"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    uint8_t keytype = 0;
    if (arg_get_lit(ctx, 1) && arg_get_lit(ctx, 2)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 2)) {
        keytype = 1;
    }

    bool m0 = arg_get_lit(ctx, 3);
    bool m1 = arg_get_lit(ctx, 4);
    bool m2 = arg_get_lit(ctx, 5);
    bool m4 = arg_get_lit(ctx, 6);
    CLIParserFree(ctx); 

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t sectors_cnt = MIFARE_1K_MAXSECTOR;

    if (m0) {
        sectors_cnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        sectors_cnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        sectors_cnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        sectors_cnt = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    mfc_eload_t payload = {
        .sectorcnt = sectors_cnt,
        .keytype = keytype
    };

    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_EML_LOAD, (uint8_t *)&payload, sizeof(payload));

    // 2021, iceman:  should get a response from device when its done.
    return PM3_SUCCESS;
}

static int CmdHF14AMfEKeyPrn(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf ekeyprn",
                  "Download and print the keys from emulator memory",
                  "hf mf ekeyprn --1k --> print MFC 1K keyset\n"
                  "hf mf ekeyprn -w   --> write keys to binary file"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("w", "write", "write keys to binary file `hf-mf-<UID>-key.bin`"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    bool create_dumpfile = arg_get_lit(ctx, 1);
    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);
    CLIParserFree(ctx); 

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t sectors_cnt = MIFARE_1K_MAXSECTOR;

    if (m0) {
        sectors_cnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        sectors_cnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        sectors_cnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        sectors_cnt = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    sector_t *e_sector = NULL;

    // create/initialize key storage structure
    int32_t res = initSectorTable(&e_sector, sectors_cnt);
    if (res != sectors_cnt) {
        free(e_sector);
        return PM3_EMALLOC;
    }

    // read UID from EMUL
    uint8_t data[16];
    if (mfEmlGetMem(data, 0, 1) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "error get block 0");
        free(e_sector);
        return PM3_ESOFT;
    }

    // assuming 4byte UID.
    uint8_t uid[4];
    memcpy(uid, data, sizeof(uid));

    // download keys from EMUL
    for (int i = 0; i < sectors_cnt; i++) {

        if (mfEmlGetMem(data, FirstBlockOfSector(i) + NumBlocksPerSector(i) - 1, 1) != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, "error get block %d", FirstBlockOfSector(i) + NumBlocksPerSector(i) - 1);
            e_sector[i].foundKey[0] = false;
            e_sector[i].foundKey[1] = false;
        } else {
            e_sector[i].foundKey[0] = true;
            e_sector[i].Key[0] = bytes_to_num(data, 6);
            e_sector[i].foundKey[1] = true;
            e_sector[i].Key[1] = bytes_to_num(data + 10, 6);
        }
    }

    // print keys
    printKeyTable(sectors_cnt, e_sector);

    // dump the keys
    if (create_dumpfile) {

        char filename[FILE_PATH_SIZE] = {0};
        char *fptr = filename;
        fptr += snprintf(fptr, sizeof(filename), "hf-mf-");
        FillFileNameByUID(fptr + strlen(fptr), uid, "-key", sizeof(uid));
        createMfcKeyDump(filename, sectors_cnt, e_sector);
    }

    free(e_sector);
    return PM3_SUCCESS;
}

// CHINESE MAGIC COMMANDS
static int CmdHF14AMfCSetUID(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf csetuid",
                  "Set UID, ATQA, and SAK for magic gen1a card",
                  "hf mf csetuid -u 01020304\n"
                  "hf mf csetuid -w -u 01020304 --atqa 0004 --sak 08"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("w", "wipe", "wipes card with backdoor cmd`"),
        arg_str0("u", "uid",  "<hex>", "UID, 4/7 hex bytes"),
        arg_str0("a", "atqa", "<hex>", "ATQA, 2 hex bytes"),
        arg_str0("s", "sak",  "<hex>", "SAK, 1 hex byte"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    uint8_t wipe_card = arg_get_lit(ctx, 1);

    int uidlen = 0;
    uint8_t uid[7] = {0x00};
    CLIGetHexWithReturn(ctx, 2, uid, &uidlen);

    int alen = 0;
    uint8_t atqa[2] = {0x00};
    CLIGetHexWithReturn(ctx, 3, atqa, &alen);

    int slen = 0;
    uint8_t sak[1] = {0x00};
    CLIGetHexWithReturn(ctx, 4, sak, &slen);
    CLIParserFree(ctx); 

    // sanity checks
    if (uidlen != 4 && uidlen != 7) {
        PrintAndLogEx(FAILED, "UID must be 4 or 7 hex bytes. Got %d", uidlen);
        return PM3_EINVARG;
    }
    if (alen && alen != 2) {
        PrintAndLogEx(FAILED, "ATQA must be 2 hex bytes. Got %d", alen);
        return PM3_EINVARG;
    }
    if (slen && slen != 1) {
        PrintAndLogEx(FAILED, "SAK must be 1 hex byte. Got %d", slen);
        return PM3_EINVARG;
    }

    uint8_t old_uid[7] = {0};
    uint8_t verify_uid[7] = {0};

    int res = mfCSetUID(
            uid,
            uidlen,
            (alen) ? atqa : NULL,
            (slen) ? sak : NULL,
            old_uid,
            verify_uid,
            wipe_card
        );

    if (res) {
        PrintAndLogEx(ERR, "Can't set UID. error %d", res);
        return PM3_ESOFT;
    }

    res = memcmp(uid, verify_uid, uidlen);

    PrintAndLogEx(SUCCESS, "Old UID... %s", sprint_hex(old_uid, uidlen));
    PrintAndLogEx(SUCCESS, "New UID... %s ( %s )",
        sprint_hex(verify_uid, uidlen),
        (res == 0) ? _GREEN_("verified") : _RED_("fail")
        );
    return PM3_SUCCESS;
}

static int CmdHF14AMfCWipe(const char *cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf cwipe",
                  "Wipe gen1 magic chinese card.\n"
                  "Set UID / ATQA / SAK / Data / Keys / Access to default values",
                  "hf mf cwipe\n"
                  "hf mf cwipe -u 09080706 -a 0004 -s 18 --> set UID, ATQA and SAK and wipe card");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("u", "uid",  "<hex>", "UID, 4 hex bytes"),
        arg_str0("a", "atqa", "<hex>", "ATQA, 2 hex bytes"),
        arg_str0("s", "sak",  "<hex>", "SAK, 1 hex byte"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, cmd, argtable, true);

    int uidlen = 0;
    uint8_t uid[8] = {0x00};
    CLIGetHexWithReturn(ctx, 1, uid, &uidlen);
    
    int alen = 0;
    uint8_t atqa[2] = {0x00};
    CLIGetHexWithReturn(ctx, 2, atqa, &alen);
    
    int slen = 0;
    uint8_t sak[1] = {0x00};
    CLIGetHexWithReturn(ctx, 3, sak, &slen);
    CLIParserFree(ctx);

    if (uidlen && uidlen != 4) {
        PrintAndLogEx(ERR, "UID length must be 4 bytes, got %d", uidlen);
        return PM3_EINVARG;
    }
    if (alen && alen != 2) {
        PrintAndLogEx(ERR, "ATQA length must be 2 bytes, got %d", alen);
        return PM3_EINVARG;
    }
    if (slen && slen != 1) {
        PrintAndLogEx(ERR, "SAK length must be 1 byte, got %d", slen);
        return PM3_EINVARG;
    }

    int res = mfCWipe((uidlen) ? uid : NULL, (alen) ? atqa : NULL, (slen) ? sak : NULL);
    if (res) {
        PrintAndLogEx(ERR, "Can't wipe card. error %d", res);
        return PM3_ESOFT;
    }

    PrintAndLogEx(SUCCESS, "Card wiped successfully");
    return PM3_SUCCESS;
}

static int CmdHF14AMfCSetBlk(const char *Cmd) {
    uint8_t block[16] = {0x00};
    uint8_t blockNo = 0;
    uint8_t params = MAGIC_SINGLE;
    int res;
    char ctmp = tolower(param_getchar(Cmd, 0));

    if (strlen(Cmd) < 1 || ctmp == 'h') return usage_hf14_csetblk();

    blockNo = param_get8(Cmd, 0);

    if (param_gethex(Cmd, 1, block, 32)) return usage_hf14_csetblk();

    ctmp = tolower(param_getchar(Cmd, 2));
    if (ctmp == 'w')
        params |= MAGIC_WIPE;

    PrintAndLogEx(NORMAL, "--block number:%2d data:%s", blockNo, sprint_hex(block, 16));

    res = mfCSetBlock(blockNo, block, NULL, params);
    if (res) {
        PrintAndLogEx(ERR, "Can't write block. error=%d", res);
        return PM3_ESOFT;
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfCLoad(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf cload",
                  "Load magic gen1a card with data from (bin/eml/json) dump file\n"
                  "or from emulator memory.",
                  "hf mf cload --emu\n"
                  "hf mf cload -f hf-mf-01020304.eml\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str1("f", "file", "<fn>", "filename of dump"),
        arg_lit0(NULL, "emu", "from emulator memory"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    
    int fnlen = 0;
    char filename[FILE_PATH_SIZE];
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    bool fill_from_emulator = arg_get_lit(ctx, 2);
    CLIParserFree(ctx); 

    if (fill_from_emulator) {

        PrintAndLogEx(INFO, "Start upload to emulator memory");
        PrintAndLogEx(INFO, "." NOLF);

        for (int b = 0; b < MIFARE_1K_MAXBLOCK; b++) {
            int flags = 0;
            uint8_t buf8[MFBLOCK_SIZE] = {0x00};

            // read from emul memory
            if (mfEmlGetMem(buf8, b, 1)) {
                PrintAndLogEx(WARNING, "Can't read from emul block: %d", b);
                return PM3_ESOFT;
            }

            // switch on field and send magic sequence
            if (b == 0) {
                flags = MAGIC_INIT + MAGIC_WUPC;
            }

            // just write
            if (b == 1) {
                flags = 0;
            }

            // Done. Magic Halt and switch off field.
            if (b == ((MFBLOCK_SIZE * 4) - 1)) {
                flags = MAGIC_HALT + MAGIC_OFF;
            }

            // write to card
            if (mfCSetBlock(b, buf8, NULL, flags)) {
                PrintAndLogEx(WARNING, "Can't set magic card block: %d", b);
                return PM3_ESOFT;
            }
            PrintAndLogEx(NORMAL, "." NOLF);
            fflush(stdout);
        }
        PrintAndLogEx(NORMAL, "");
        return PM3_SUCCESS;
    }

    uint8_t *data = calloc(MFBLOCK_SIZE * MIFARE_4K_MAXBLOCK, sizeof(uint8_t));
    if (data == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }

    size_t bytes_read = 0;
    int res = 0;
    DumpFileType_t dftype = getfiletype(filename);
    switch (dftype) {
        case BIN: {
            res = loadFile_safe(filename, ".bin", (void **)&data, &bytes_read);
            break;
        }
        case EML: {
            res = loadFileEML_safe(filename, (void **)&data, &bytes_read);
            break;
        }
        case JSON: {
            res = loadFileJSON(filename, data, MIFARE_4K_MAXBLOCK * MFBLOCK_SIZE, &bytes_read, NULL);
            break;
        }
        case DICTIONARY: {
            PrintAndLogEx(ERR, "Error: Only BIN/JSON/EML formats allowed");
            return PM3_EINVARG;
        }
    }

    if (res != PM3_SUCCESS) {
        free(data);
        return PM3_EFILE;
    }

    // 64 or 256blocks.
    if (bytes_read != (MIFARE_1K_MAXBLOCK * MFBLOCK_SIZE) &&
        bytes_read != (MIFARE_4K_MAXBLOCK * MFBLOCK_SIZE)) {
        PrintAndLogEx(ERR, "File content error. Read %zu bytes", bytes_read);
        free(data);
        return PM3_EFILE;
    }

    PrintAndLogEx(INFO, "Copying to magic gen1a card");
    PrintAndLogEx(INFO, "." NOLF);

    int blockno = 0;
    int flags = 0;
    while (bytes_read) {

        // switch on field and send magic sequence
        if (blockno == 0) {
            flags = MAGIC_INIT + MAGIC_WUPC;
        }

        // write
        if (blockno == 1) {
            flags = 0;
        }

        // switch off field
        if (blockno == MFBLOCK_SIZE * 4 - 1) {
            flags = MAGIC_HALT + MAGIC_OFF;
        }

        if (mfCSetBlock(blockno, data + (MFBLOCK_SIZE * blockno), NULL, flags)) {
            PrintAndLogEx(WARNING, "Can't set magic card block: %d", blockno);
            free(data);
            return PM3_ESOFT;
        }

        bytes_read -= MFBLOCK_SIZE;

        PrintAndLogEx(NORMAL, "." NOLF);
        fflush(stdout);

        blockno++;

        // magic card type - mifare 1K
        if (blockno >= MIFARE_1K_MAXBLOCK) break;
    }
    PrintAndLogEx(NORMAL, "\n");

    free(data);

    // confirm number written blocks. Must be 64 or 256 blocks
    if (blockno != MIFARE_1K_MAXBLOCK) {
         if (blockno != MIFARE_4K_MAXBLOCK) {
            PrintAndLogEx(ERR, "File content error. There must be %u blocks", MIFARE_4K_MAXBLOCK);
            return PM3_EFILE;
        }
        PrintAndLogEx(ERR, "File content error. There must be %d blocks", MIFARE_1K_MAXBLOCK);
        return PM3_EFILE;
    }

    PrintAndLogEx(SUCCESS, "Card loaded %d blocks from file", blockno);
    PrintAndLogEx(INFO, "Done!");
    return PM3_SUCCESS;
}

static int CmdHF14AMfCGetBlk(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf cgetblk",
                  "Get block data from magic Chinese card.\n"
                  "Only works with magic gen1a cards",
                  "hf mf cgetblk --blk 0      --> get block 0 (manufacturer)\n"
                  "hf mf cgetblk --blk 3 -v   --> get block 3, decode sector trailer\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("b",  "blk", "<dec>", "block number"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int b = arg_get_int_def(ctx, 1, 0);
    bool verbose = arg_get_lit(ctx, 2);
    CLIParserFree(ctx); 

    if (b > 255) {
        return PM3_EINVARG;
    }

    uint8_t blockno = (uint8_t)b;
    uint8_t data[16] = {0};
    int res = mfCGetBlock(blockno, data, MAGIC_SINGLE);
    if (res) {
        PrintAndLogEx(ERR, "Can't read block. error=%d", res);
        return PM3_ESOFT;
    }

    uint8_t sector = GetSectorFromBlockNo(blockno);
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "  # | data  - sector %02d / 0x%02X                        | ascii", sector, sector);
    PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");
    mf_print_block(blockno, data);
   
    if (verbose) {
        decode_print_st(blockno, data);
    } else {
        PrintAndLogEx(NORMAL, "");
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfCGetSc(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf cgetsc",
                  "Get sector data from magic Chinese card.\n"
                  "Only works with magic gen1a cards",
                  "hf mf cgetsc -s 0"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("s",  "sec", "<dec>", "sector number"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int s = arg_get_int_def(ctx, 1, 0);
    bool verbose = arg_get_lit(ctx, 2);
    CLIParserFree(ctx); 
    if (s > 39) {
        PrintAndLogEx(WARNING, "Sector number must be less then 40");
        return PM3_EINVARG;
    }

    uint8_t sector = (uint8_t)s;

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "  # | data  - sector %02d / 0x%02X                        | ascii", sector, sector);
    PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");

    uint8_t blocks = 4;
    uint8_t start = sector * 4;
    if (sector >= 32) {
        blocks = 16;
        start = 128 + (sector - 32) * 16;
    }

    int flags = MAGIC_INIT + MAGIC_WUPC;
    uint8_t data[16] = {0};
    for (int i = 0; i < blocks; i++) {
        if (i == 1) flags = 0;
        if (i == blocks - 1) flags = MAGIC_HALT + MAGIC_OFF;

        int res = mfCGetBlock(start + i, data, flags);
        if (res) {
            PrintAndLogEx(ERR, "Can't read block. %d error=%d", start + i, res);
            return PM3_ESOFT;
        }
        mf_print_block(start + i, data);
    }
    if (verbose) {
        decode_print_st(start + blocks - 1, data);
    } else {
        PrintAndLogEx(NORMAL, "");
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfCSave(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf csave",
                  "Save magic gen1a card memory into three files (BIN/EML/JSON)"
                  "or into emulator memory",
                  "hf mf csave\n"
                  "hf mf csave --4k"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("f", "file", "<fn>", "filename of dump"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0(NULL, "emu", "from emulator memory"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    
    int fnlen = 0;
    char filename[FILE_PATH_SIZE];
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);
    bool fill_emulator = arg_get_lit(ctx, 6);
    CLIParserFree(ctx); 

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    char s[6];
    memset(s, 0, sizeof(s));
    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;
    if (m0) {
        block_cnt = MIFARE_MINI_MAXBLOCK;
        strncpy(s, "Mini", 5);
    } else if (m1) {
        block_cnt = MIFARE_1K_MAXBLOCK;
        strncpy(s, "1K", 3);
    } else if (m2) {
        block_cnt = MIFARE_2K_MAXBLOCK;
        strncpy(s, "2K", 3);
    } else if (m4) {
        block_cnt = MIFARE_4K_MAXBLOCK;
        strncpy(s, "4K", 3);
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    PrintAndLogEx(SUCCESS, "Dumping magic Gen1a MIFARE Classic " _GREEN_("%s") " card memory", s);
    PrintAndLogEx(INFO, "." NOLF);

    // Select card to get UID/UIDLEN information
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500) == false) {
        PrintAndLogEx(WARNING, "iso14443a card select timeout");
        return PM3_ETIMEOUT;
    }

    /*
        0: couldn't read
        1: OK, with ATS
        2: OK, no ATS
        3: proprietary Anticollision
    */
    uint64_t select_status = resp.oldarg[0];
    if (select_status == 0) {
        PrintAndLogEx(WARNING, "iso14443a card select failed");
        return select_status;
    }

    // store card info
    iso14a_card_select_t card;
    memcpy(&card, (iso14a_card_select_t *)resp.data.asBytes, sizeof(iso14a_card_select_t));

    // reserve memory
    uint16_t bytes = block_cnt * MFBLOCK_SIZE;
    uint8_t *dump = calloc(bytes, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }

    // switch on field and send magic sequence
    uint8_t flags = MAGIC_INIT + MAGIC_WUPC;
    for (uint16_t i = 0; i < block_cnt; i++) {

        // read
        if (i == 1) {
            flags = 0;
        }
        // switch off field
        if (i == block_cnt - 1) {
            flags = MAGIC_HALT + MAGIC_OFF;
        }

        if (mfCGetBlock(i, dump + (i * MFBLOCK_SIZE), flags)) {
            PrintAndLogEx(WARNING, "Can't get magic card block: %d", i);
            PrintAndLogEx(HINT, "Verify your card size, and try again or try another tag position");
            free(dump);
            return PM3_ESOFT;
        }
        PrintAndLogEx(NORMAL, "." NOLF);
        fflush(stdout);
    }
    PrintAndLogEx(NORMAL, "");

    if (fill_emulator) {
        PrintAndLogEx(INFO, "uploading to emulator memory");
        PrintAndLogEx(INFO, "." NOLF);
        // fast push mode
        conn.block_after_ACK = true;
        for (int i = 0; i < block_cnt; i += 5) {
            if (i == block_cnt - 1) {
                // Disable fast mode on last packet
                conn.block_after_ACK = false;
            }
            if (mfEmlSetMem(dump + (i * MFBLOCK_SIZE), i, 5) != PM3_SUCCESS) {
                PrintAndLogEx(WARNING, "Can't set emul block: %d", i);
            }
            PrintAndLogEx(NORMAL, "." NOLF);
            fflush(stdout);
        }
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(SUCCESS, "uploaded %d bytes to emulator memory", bytes);
    }

    // user supplied filename?
    if (fnlen < 1) {
        char *fptr = filename;
        fptr += snprintf(fptr, sizeof(filename), "hf-mf-");
        FillFileNameByUID(fptr, card.uid, "-dump", card.uidlen);
    }

    saveFile(filename, ".bin", dump, bytes);
    saveFileEML(filename, dump, bytes, MFBLOCK_SIZE);
    saveFileJSON(filename, jsfCardMemory, dump, bytes, NULL);
    free(dump);
    return PM3_SUCCESS;
}

static int CmdHF14AMfCView(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf cview",
                  "View `magic gen1a` card memory",
                  "hf mf cview\n"
                  "hf mf cview --4k"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool m0 = arg_get_lit(ctx, 1);
    bool m1 = arg_get_lit(ctx, 2);
    bool m2 = arg_get_lit(ctx, 3);
    bool m4 = arg_get_lit(ctx, 4);
    CLIParserFree(ctx); 

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    char s[6];
    memset(s, 0, sizeof(s));
    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;
    if (m0) {
        block_cnt = MIFARE_MINI_MAXBLOCK;
        strncpy(s, "Mini", 5);
    } else if (m1) {
        block_cnt = MIFARE_1K_MAXBLOCK;
        strncpy(s, "1K", 3);
    } else if (m2) {
        block_cnt = MIFARE_2K_MAXBLOCK;
        strncpy(s, "2K", 3);
    } else if (m4) {
        block_cnt = MIFARE_4K_MAXBLOCK;
        strncpy(s, "4K", 3);
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }
    PrintAndLogEx(SUCCESS, "View magic Gen1a MIFARE Classic " _GREEN_("%s"), s);
    PrintAndLogEx(INFO, "." NOLF);

    // Select card to get UID/UIDLEN information
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500) == false) {
        PrintAndLogEx(WARNING, "iso14443a card select timeout");
        return PM3_ETIMEOUT;
    }

    /*
        0: couldn't read
        1: OK, with ATS
        2: OK, no ATS
        3: proprietary Anticollision
    */
    uint64_t select_status = resp.oldarg[0];

    if (select_status == 0) {
        PrintAndLogEx(WARNING, "iso14443a card select failed");
        return select_status;
    }

    iso14a_card_select_t card;
    memcpy(&card, (iso14a_card_select_t *)resp.data.asBytes, sizeof(iso14a_card_select_t));

    // reserve memory
    uint16_t bytes = block_cnt * MFBLOCK_SIZE;
    uint8_t *dump = calloc(bytes, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }

    // switch on field and send magic sequence
    uint8_t flags = MAGIC_INIT + MAGIC_WUPC;
    for (uint16_t i = 0; i < block_cnt; i++) {
        // read
        if (i == 1) {
            flags = 0;
        }
        // switch off field
        if (i == block_cnt - 1) {
            flags = MAGIC_HALT + MAGIC_OFF;
        }

        if (mfCGetBlock(i, dump + (i * MFBLOCK_SIZE), flags)) {
            PrintAndLogEx(WARNING, "Can't get magic card block: %u", i);
            PrintAndLogEx(HINT, "Verify your card size, and try again or try another tag position");
            free(dump);
            return PM3_ESOFT;
        }
        PrintAndLogEx(NORMAL, "." NOLF);
        fflush(stdout);
    }

    PrintAndLogEx(NORMAL, "");
    mf_print_blocks(block_cnt, dump);
    free(dump);
    return PM3_SUCCESS;
}

//needs nt, ar, at, Data to decrypt
static int CmdHf14AMfDecryptBytes(const char *Cmd) {

    char ctmp = tolower(param_getchar(Cmd, 0));
    if (strlen(Cmd) < 1 || ctmp == 'h') return usage_hf14_decryptbytes();

    uint32_t nt     = param_get32ex(Cmd, 0, 0, 16);
    uint32_t ar_enc = param_get32ex(Cmd, 1, 0, 16);
    uint32_t at_enc = param_get32ex(Cmd, 2, 0, 16);

    int len = param_getlength(Cmd, 3);
    if (len & 1) {
        PrintAndLogEx(WARNING, "Uneven hex string length. LEN=%d", len);
        return PM3_ESOFT;
    }

    PrintAndLogEx(NORMAL, "nt\t%08X", nt);
    PrintAndLogEx(NORMAL, "ar enc\t%08X", ar_enc);
    PrintAndLogEx(NORMAL, "at enc\t%08X", at_enc);

    uint8_t *data = calloc(len, sizeof(uint8_t));
    if (!data) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }

    param_gethex_ex(Cmd, 3, data, &len);
    len >>= 1;
    tryDecryptWord(nt, ar_enc, at_enc, data, len);
    free(data);
    return PM3_SUCCESS;
}

static int CmdHf14AMfSetMod(const char *Cmd) {
    uint8_t key[6] = {0, 0, 0, 0, 0, 0};
    uint8_t mod = 2;

    char ctmp = param_getchar(Cmd, 0);
    if (ctmp == '0') {
        mod = 0;
    } else if (ctmp == '1') {
        mod = 1;
    }
    int gethexfail = param_gethex(Cmd, 1, key, 12);
    if (mod == 2 || gethexfail) {
        PrintAndLogEx(NORMAL, "Sets the load modulation strength of a MIFARE Classic EV1 card.");
        PrintAndLogEx(NORMAL, "Usage: hf mf setmod <0|1> <block 0 key A>");
        PrintAndLogEx(NORMAL, "       0 = normal modulation");
        PrintAndLogEx(NORMAL, "       1 = strong modulation (default)");
        return PM3_ESOFT;
    }

    uint8_t data[7];
    data[0] = mod;
    memcpy(data + 1, key, 6);

    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_SETMOD, data, sizeof(data));

    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_HF_MIFARE_SETMOD, &resp, 1500)) {

        if (resp.status == PM3_SUCCESS)
            PrintAndLogEx(SUCCESS, "Success");
        else
            PrintAndLogEx(FAILED, "Failed");

    } else {
        PrintAndLogEx(WARNING, "Command execute timeout");
    }
    return PM3_SUCCESS;
}

// MIFARE NACK bug detection
static int CmdHf14AMfNack(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf nack",
                  "Test a MIFARE Classic based card for the NACK bug",
                  "hf mf nack"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("v", "verbose", "verbose output`"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool verbose = arg_get_lit(ctx, 1);
    CLIParserFree(ctx); 

    if (verbose)
        PrintAndLogEx(INFO, "Started testing card for NACK bug. Press Enter to abort");

    detect_classic_nackbug(verbose);
    return PM3_SUCCESS;
}

static int CmdHF14AMfice(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf ice",
                  "Collect MIFARE Classic nonces to file",
                  "hf mf ice\n"
                  "hf mf ice -f nonces.bin");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("f", "file", "<filename>", "filename of nonce dump"),
        arg_u64_0(NULL, "limit", "<dec>", "nonces to be collected"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    uint32_t limit = arg_get_u32_def(ctx, 2, 50000);

    CLIParserFree(ctx);

    // Validations
    char *fptr;

    if (filename[0] == '\0') {
        fptr = GenerateFilename("hf-mf-", "-nonces.bin");
        if (fptr == NULL)
            return PM3_EFILE;
        strcpy(filename, fptr);
        free(fptr);
    }

    uint8_t blockNo = 0;
    uint8_t keyType = 0;
    uint8_t trgBlockNo = 0;
    uint8_t trgKeyType = 1;
    bool slow = false;
    bool initialize = true;
    bool acquisition_completed = false;
    uint32_t total_num_nonces = 0;
    PacketResponseNG resp;

    uint32_t part_limit = 3000;

    PrintAndLogEx(NORMAL, "Collecting "_YELLOW_("%u")" nonces \n", limit);

    FILE *fnonces = NULL;
    if ((fnonces = fopen(filename, "wb")) == NULL) {
        PrintAndLogEx(WARNING, "Could not create file " _YELLOW_("%s"), filename);
        return PM3_EFILE;
    }

    clearCommandBuffer();

    uint64_t t1 = msclock();

    do {
        if (kbd_enter_pressed()) {
            PrintAndLogEx(INFO, "\naborted via keyboard!\n");
            break;
        }

        uint32_t flags = 0;
        flags |= initialize ? 0x0001 : 0;
        flags |= slow ? 0x0002 : 0;
        clearCommandBuffer();
        SendCommandMIX(CMD_HF_MIFARE_ACQ_NONCES, blockNo + keyType * 0x100, trgBlockNo + trgKeyType * 0x100, flags, NULL, 0);

        if (!WaitForResponseTimeout(CMD_ACK, &resp, 3000)) goto out;
        if (resp.oldarg[0])  goto out;

        uint32_t items = resp.oldarg[2];
        fwrite(resp.data.asBytes, 1, items * 4, fnonces);
        fflush(fnonces);

        total_num_nonces += items;
        if (total_num_nonces > part_limit) {
            PrintAndLogEx(INFO, "Total nonces %u\n", total_num_nonces);
            part_limit += 3000;
        }

        acquisition_completed = (total_num_nonces > limit);

        initialize = false;

    } while (!acquisition_completed);

out:
    PrintAndLogEx(SUCCESS, "time: %" PRIu64 " seconds\n", (msclock() - t1) / 1000);

    if (fnonces) {
        fflush(fnonces);
        fclose(fnonces);
    }

    clearCommandBuffer();
    SendCommandMIX(CMD_HF_MIFARE_ACQ_NONCES, blockNo + keyType * 0x100, trgBlockNo + trgKeyType * 0x100, 4, NULL, 0);
    return PM3_SUCCESS;
}

static int CmdHF14AMfAuth4(const char *Cmd) {
    uint8_t keyn[20] = {0};
    int keynlen = 0;
    uint8_t key[16] = {0};
    int keylen = 0;

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf auth4",
                  "Executes AES authentication command in ISO14443-4",
                  "hf mf auth4 4000 000102030405060708090a0b0c0d0e0f -> executes authentication\n"
                  "hf mf auth4 9003 FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> executes authentication\n");

    void *argtable[] = {
        arg_param_begin,
        arg_str1(NULL,  NULL,     "<Key Num (HEX 2 bytes)>", NULL),
        arg_str1(NULL,  NULL,     "<Key Value (HEX 16 bytes)>", NULL),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    CLIGetHexWithReturn(ctx, 1, keyn, &keynlen);
    CLIGetHexWithReturn(ctx, 2, key, &keylen);
    CLIParserFree(ctx);

    if (keynlen != 2) {
        PrintAndLogEx(ERR, "<Key Num> must be 2 bytes long instead of: %d", keynlen);
        return PM3_ESOFT;
    }

    if (keylen != 16) {
        PrintAndLogEx(ERR, "<Key Value> must be 16 bytes long instead of: %d", keylen);
        return PM3_ESOFT;
    }

    return MifareAuth4(NULL, keyn, key, true, false, true, true, false);
}

// https://www.nxp.com/docs/en/application-note/AN10787.pdf
static int CmdHF14AMfMAD(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf mad",
                  "Checks and prints MIFARE Application Directory (MAD)",
                  "hf mf mad -> shows MAD if exists\n"
                  "hf mf mad --aid e103 -k ffffffffffff -b -> shows NDEF data if exists. read card with custom key and key B\n"
                  "hf mf mad --dch -k ffffffffffff -> decode CardHolder information\n");

    void *argtable[] = {
        arg_param_begin,
        arg_lit0("v",  "verbose",  "show technical data"),
        arg_str0(NULL, "aid",      "<aid>", "print all sectors with specified aid"),
        arg_str0("k",  "key",      "<key>", "key for printing sectors"),
        arg_lit0("b",  "keyb",     "use key B for access printing sectors (by default: key A)"),
        arg_lit0(NULL, "be",       "(optional, BigEndian)"),
        arg_lit0(NULL, "dch",      "decode Card Holder information"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool verbose = arg_get_lit(ctx, 1);
    uint8_t aid[2] = {0};
    int aidlen = 0;
    CLIGetHexWithReturn(ctx, 2, aid, &aidlen);
    uint8_t userkey[6] = {0};
    int keylen = 0;
    CLIGetHexWithReturn(ctx, 3, userkey, &keylen);
    bool keyB = arg_get_lit(ctx, 4);
    bool swapmad = arg_get_lit(ctx, 5);
    bool decodeholder = arg_get_lit(ctx, 6);

    CLIParserFree(ctx);

    uint8_t sector0[16 * 4] = {0};
    uint8_t sector10[16 * 4] = {0};

    bool got_first = true;
    if (mfReadSector(MF_MAD1_SECTOR, MF_KEY_A, (uint8_t *)g_mifare_mad_key, sector0) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "error, read sector 0. card don't have MAD or don't have MAD on default keys");
        got_first = false;
    } else {
        PrintAndLogEx(INFO, "Authentication ( " _GREEN_("OK") " )");
    }

    // User supplied key
    if (got_first == false && keylen == 6) {
        PrintAndLogEx(INFO, "Trying user specified key...");
        if (mfReadSector(MF_MAD1_SECTOR, MF_KEY_A, userkey, sector0) != PM3_SUCCESS) {
            PrintAndLogEx(ERR, "error, read sector 0. card don't have MAD or don't the custom key is wrong");
        } else {
            PrintAndLogEx(INFO, "Authentication ( " _GREEN_("OK") " )");
            got_first = true;
        }
    }

    // Both default and user supplied key failed
    if (got_first == false) {
        return PM3_ESOFT;
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("MIFARE App Directory Information") " ----------------");
    PrintAndLogEx(INFO, "-----------------------------------------------------");

    bool haveMAD2 = false;
    MAD1DecodeAndPrint(sector0, swapmad, verbose, &haveMAD2);

    if (haveMAD2) {
        if (mfReadSector(MF_MAD2_SECTOR, MF_KEY_A, (uint8_t *)g_mifare_mad_key, sector10)) {
            PrintAndLogEx(ERR, "error, read sector 0x10. card don't have MAD or don't have MAD on default keys");
            return PM3_ESOFT;
        }

        MAD2DecodeAndPrint(sector10, swapmad, verbose);
    }

    if (aidlen == 2 || decodeholder) {
        uint16_t mad[7 + 8 + 8 + 8 + 8] = {0};
        size_t madlen = 0;
        if (MADDecode(sector0, sector10, mad, &madlen, swapmad)) {
            PrintAndLogEx(ERR, "can't decode MAD");
            return PM3_ESOFT;
        }

        // copy default NDEF key
        uint8_t akey[6] = {0};
        memcpy(akey, g_mifare_ndef_key, 6);

        // user specified key
        if (keylen == 6) {
            memcpy(akey, userkey, 6);
        }

        uint16_t aaid = 0x0004;
        if (aidlen == 2) {

            aaid = (aid[0] << 8) + aid[1];

            PrintAndLogEx(NORMAL, "");
            PrintAndLogEx(INFO, "-------------- " _CYAN_("AID 0x%04x") " ---------------", aaid);

            for (int i = 0; i < madlen; i++) {
                if (aaid == mad[i]) {
                    uint8_t vsector[16 * 4] = {0};
                    if (mfReadSector(i + 1, keyB ? MF_KEY_B : MF_KEY_A, akey, vsector)) {
                        PrintAndLogEx(NORMAL, "");
                        PrintAndLogEx(ERR, "error, read sector %d", i + 1);
                        return PM3_ESOFT;
                    }

                    for (int j = 0; j < (verbose ? 4 : 3); j ++)
                        PrintAndLogEx(NORMAL, " [%03d] %s", (i + 1) * 4 + j, sprint_hex(&vsector[j * 16], 16));
                }
            }
        }

        if (decodeholder) {

            PrintAndLogEx(NORMAL, "");
            PrintAndLogEx(INFO, "-------- " _CYAN_("Card Holder Info 0x%04x") " --------", aaid);

            uint8_t data[4096] = {0};
            int datalen = 0;

            for (int i = 0; i < madlen; i++) {
                if (aaid == mad[i]) {

                    uint8_t vsector[16 * 4] = {0};
                    if (mfReadSector(i + 1, keyB ? MF_KEY_B : MF_KEY_A, akey, vsector)) {
                        PrintAndLogEx(NORMAL, "");
                        PrintAndLogEx(ERR, "error, read sector %d", i + 1);
                        return PM3_ESOFT;
                    }

                    memcpy(&data[datalen], vsector, 16 * 3);
                    datalen += 16 * 3;
                }
            }

            if (!datalen) {
                PrintAndLogEx(WARNING, "no Card Holder Info data");
                return PM3_SUCCESS;
            }
            MADCardHolderInfoDecode(data, datalen, verbose);
        }
    }

    if (verbose) {
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "------------ " _CYAN_("MAD sector raw") " -------------");
        for (int i = 0; i < 4; i ++)
            PrintAndLogEx(INFO, "[%d] %s", i, sprint_hex(&sector0[i * 16], 16));
    }

    return PM3_SUCCESS;
}

static int CmdHFMFNDEF(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf ndef",
                  "Prints NFC Data Exchange Format (NDEF)",
                  "hf mf ndef -> shows NDEF parsed data\n"
                  "hf mf ndef -vv -> shows NDEF parsed and raw data\n"
                  "hf mf ndef --aid e103 -k ffffffffffff -b -> shows NDEF data with custom AID, key and with key B\n");

    void *argtable[] = {
        arg_param_begin,
        arg_litn("v",  "verbose",  0, 2, "show technical data"),
        arg_str0(NULL, "aid",      "<aid>", "replace default aid for NDEF"),
        arg_str0("k",  "key",      "<key>", "replace default key for NDEF"),
        arg_lit0("b",  "keyb",     "use key B for access sectors (by default: key A)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    bool verbose = arg_get_lit(ctx, 1);
    bool verbose2 = arg_get_lit(ctx, 1) > 1;
    uint8_t aid[2] = {0};
    int aidlen;
    CLIGetHexWithReturn(ctx, 2, aid, &aidlen);
    uint8_t key[6] = {0};
    int keylen;
    CLIGetHexWithReturn(ctx, 3, key, &keylen);
    bool keyB = arg_get_lit(ctx, 4);

    CLIParserFree(ctx);

    uint16_t ndefAID = 0xe103;
    if (aidlen == 2)
        ndefAID = (aid[0] << 8) + aid[1];

    uint8_t ndefkey[6] = {0};
    memcpy(ndefkey, g_mifare_ndef_key, 6);
    if (keylen == 6) {
        memcpy(ndefkey, key, 6);
    }

    uint8_t sector0[16 * 4] = {0};
    uint8_t sector10[16 * 4] = {0};
    uint8_t data[4096] = {0};
    int datalen = 0;

    if (verbose)
        PrintAndLogEx(INFO, "reading MAD v1 sector");

    if (mfReadSector(MF_MAD1_SECTOR, MF_KEY_A, (uint8_t *)g_mifare_mad_key, sector0)) {
        PrintAndLogEx(ERR, "error, read sector 0. card don't have MAD or don't have MAD on default keys");
        PrintAndLogEx(HINT, "Try " _YELLOW_("`hf mf ndef -k `") " with your custom key");
        return PM3_ESOFT;
    }

    bool haveMAD2 = false;
    int res = MADCheck(sector0, NULL, verbose, &haveMAD2);
    if (res != PM3_SUCCESS) {
        PrintAndLogEx(ERR, "MAD error %d", res);
        return res;
    }

    if (haveMAD2) {
        if (verbose)
            PrintAndLogEx(INFO, "reading MAD v2 sector");

        if (mfReadSector(MF_MAD2_SECTOR, MF_KEY_A, (uint8_t *)g_mifare_mad_key, sector10)) {
            PrintAndLogEx(ERR, "error, read sector 0x10. card don't have MAD or don't have MAD on default keys");
            PrintAndLogEx(HINT, "Try " _YELLOW_("`hf mf ndef -k `") " with your custom key");
            return PM3_ESOFT;
        }
    }

    uint16_t mad[7 + 8 + 8 + 8 + 8] = {0};
    size_t madlen = 0;
    res = MADDecode(sector0, (haveMAD2 ? sector10 : NULL), mad, &madlen, false);
    if (res != PM3_SUCCESS) {
        PrintAndLogEx(ERR, "can't decode MAD");
        return res;
    }

    PrintAndLogEx(INFO, "reading data from tag");
    for (int i = 0; i < madlen; i++) {
        if (ndefAID == mad[i]) {
            uint8_t vsector[16 * 4] = {0};
            if (mfReadSector(i + 1, keyB ? MF_KEY_B : MF_KEY_A, ndefkey, vsector)) {
                PrintAndLogEx(ERR, "error, reading sector %d ", i + 1);
                return PM3_ESOFT;
            }

            memcpy(&data[datalen], vsector, 16 * 3);
            datalen += 16 * 3;

            PrintAndLogEx(INPLACE, "%d", i);
        }
    }
    PrintAndLogEx(NORMAL, "");

    if (!datalen) {
        PrintAndLogEx(WARNING, "no NDEF data");
        return PM3_SUCCESS;
    }

    if (verbose2) {
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "--- " _CYAN_("MFC NDEF raw") " ----------------");
        print_buffer(data, datalen, 1);
    }

    NDEFDecodeAndPrint(data, datalen, verbose);

    PrintAndLogEx(HINT, "Try " _YELLOW_("`hf mf ndef -vv`") " for more details");
    return PM3_SUCCESS;
}

static int CmdHFMFPersonalize(const char *cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf personalize",
                  "Personalize the UID of a MIFARE Classic EV1 card. This is only possible if it is a 7Byte UID card and if it is not already personalized.",
                  "hf mf personalize UIDF0 -> double size UID according to ISO/IEC14443-3\n"
                  "hf mf personalize UIDF1 -> double size UID according to ISO/IEC14443-3, optional usage of selection process shortcut\n"
                  "hf mf personalize UIDF2 -> single size random ID according to ISO/IEC14443-3\n"
                  "hf mf personalize UIDF3 -> single size NUID according to ISO/IEC14443-3\n"
                  "hf mf personalize -t B -k B0B1B2B3B4B5 UIDF3 -> use key B = 0xB0B1B2B3B4B5 instead of default key A");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("t",  "keytype", "<A|B>",                     "key type (A or B) to authenticate sector 0 (default: A)"),
        arg_str0("k",  "key",     "<key (hex 6 Bytes)>",       "key to authenticate sector 0 (default: FFFFFFFFFFFF)"),
        arg_str1(NULL,  NULL,      "<UIDF0|UIDF1|UIDF2|UIDF3>", "Personalization Option"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, cmd, argtable, true);

    char keytypestr[2] = "a";
    uint8_t keytype = 0x00;
    int keytypestr_len;
    int res = CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)keytypestr, 1, &keytypestr_len);
    str_lower(keytypestr);

    if (res || (keytypestr[0] != 'a' && keytypestr[0] != 'b')) {
        PrintAndLogEx(ERR, "ERROR: not a valid key type. Key type must be A or B");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if (keytypestr[0] == 'b') {
        keytype = 0x01;
    }

    uint8_t key[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    int key_len;
    res = CLIParamHexToBuf(arg_get_str(ctx, 2), key, 6, &key_len);
    if (res || (!res && key_len > 0 && key_len != 6)) {
        PrintAndLogEx(ERR, "ERROR: not a valid key. Key must be 12 hex digits");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }

    char pers_optionstr[6];
    int opt_len;
    uint8_t pers_option;
    res = CLIParamStrToBuf(arg_get_str(ctx, 3), (uint8_t *)pers_optionstr, 5, &opt_len);
    str_lower(pers_optionstr);

    if (res || (!res && opt_len > 0 && opt_len != 5)
            || (strncmp(pers_optionstr, "uidf0", 5) && strncmp(pers_optionstr, "uidf1", 5) && strncmp(pers_optionstr, "uidf2", 5) && strncmp(pers_optionstr, "uidf3", 5))) {
        PrintAndLogEx(ERR, "ERROR: invalid personalization option. Must be one of UIDF0, UIDF1, UIDF2, or UIDF3");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if (!strncmp(pers_optionstr, "uidf0", 5)) {
        pers_option = MIFARE_EV1_UIDF0;
    } else if (!strncmp(pers_optionstr, "uidf1", 5)) {
        pers_option = MIFARE_EV1_UIDF1;
    } else if (!strncmp(pers_optionstr, "uidf2", 5)) {
        pers_option = MIFARE_EV1_UIDF2;
    } else {
        pers_option = MIFARE_EV1_UIDF3;
    }

    CLIParserFree(ctx);

    clearCommandBuffer();

    struct {
        uint8_t keytype;
        uint8_t pers_option;
        uint8_t key[6];
    } PACKED payload;
    payload.keytype = keytype;
    payload.pers_option = pers_option;

    memcpy(payload.key, key, 6);

    SendCommandNG(CMD_HF_MIFARE_PERSONALIZE_UID, (uint8_t *)&payload, sizeof(payload));

    PacketResponseNG resp;
    if (!WaitForResponseTimeout(CMD_HF_MIFARE_PERSONALIZE_UID, &resp, 2500)) return PM3_ETIMEOUT;

    PrintAndLogEx(SUCCESS, "Personalization %s", resp.status == PM3_SUCCESS ? "SUCCEEDED" : "FAILED");

    return PM3_SUCCESS;
}

static int CmdHF14AMfList(const char *Cmd) {
    char args[128] = {0};
    if (strlen(Cmd) == 0) {
        snprintf(args, sizeof(args), "-t mf");
    } else {
        strncpy(args, Cmd, sizeof(args) - 1);
    }
    return CmdTraceList(args);
}

static int CmdHf14AGen3UID(const char *Cmd) {
    uint8_t uid[7] = {0x00};
    uint8_t oldUid[10] = {0x00};
    uint8_t uidlen;

    char ctmp = tolower(param_getchar(Cmd, 0));
    if (ctmp == 'h') return usage_hf14_gen3uid();

    if (param_gethex(Cmd, 0, uid, 8))
        if (param_gethex(Cmd, 0, uid, 14))
            return usage_hf14_gen3uid();
        else
            uidlen = 7;
    else
        uidlen = 4;

    int res = mfGen3UID(uid, uidlen, oldUid);
    if (res) {
        PrintAndLogEx(ERR, "Can't set UID. Error=%d", res);
        return PM3_ESOFT;
    }

    PrintAndLogEx(SUCCESS, "Old UID : %s", sprint_hex(oldUid, uidlen));
    PrintAndLogEx(SUCCESS, "New UID : %s", sprint_hex(uid, uidlen));
    return PM3_SUCCESS;
}

static int CmdHf14AGen3Block(const char *Cmd) {
    uint8_t block[16] = {0x00};
    int blocklen = 0;
    uint8_t newBlock[16] = {0x00};

    char ctmp = tolower(param_getchar(Cmd, 0));
    if (ctmp == 'h') return usage_hf14_gen3block();

    if (ctmp != '\0' && param_gethex_to_eol(Cmd, 0, block, sizeof(block), &blocklen))
        return usage_hf14_gen3block();

    int res = mfGen3Block(block, blocklen, newBlock);
    if (res) {
        PrintAndLogEx(ERR, "Can't change manufacturer block data. Error=%d", res);
        return PM3_ESOFT;
    }

    PrintAndLogEx(SUCCESS, "Current Block : %s", sprint_hex(newBlock, 16));
    return PM3_SUCCESS;
}

static int CmdHf14AGen3Freeze(const char *Cmd) {
    char ctmp = tolower(param_getchar(Cmd, 0));
    if (ctmp == 'h') return usage_hf14_gen3freeze();
    if (ctmp != 'y') return usage_hf14_gen3freeze();

    int res = mfGen3Freeze();
    if (res) {
        PrintAndLogEx(ERR, "Can't lock UID changes. Error=%d", res);
        return PM3_ESOFT;
    }
    PrintAndLogEx(SUCCESS, "MFC Gen3 UID permalocked");
    return PM3_SUCCESS;
}

static void des_decrypt(void *out, const void *in, const void *key) {
    mbedtls_des_context ctx;
    mbedtls_des_setkey_dec(&ctx, key);
    mbedtls_des_crypt_ecb(&ctx, in, out);
}

static int CmdHf14AMfSuperCard(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf supercard",
                  "Extract info from a `super card`",
                  "hf mf supercard");

    void *argtable[] = {
        arg_param_begin,
        arg_lit0("r",  "reset",  "reset card"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool reset_card = arg_get_lit(ctx, 1);
    CLIParserFree(ctx);

    bool activate_field = true;
    bool keep_field_on = true;
    int res = 0;

    if (reset_card)  {

        keep_field_on = false;
        uint8_t response[6];
        int resplen = 0;

        // --------------- RESET CARD ----------------
        uint8_t aRESET[] = { 0x00, 0xa6, 0xc0,  0x00 };
        res = ExchangeAPDU14a(aRESET, sizeof(aRESET), activate_field, keep_field_on, response, sizeof(response), &resplen);
        if (res) {
            PrintAndLogEx(FAILED, "Super card reset [ " _RED_("fail") " ]");
            DropField();
            return res;
        }
        PrintAndLogEx(SUCCESS, "Super card reset [ " _GREEN_("ok") " ]");
        return PM3_SUCCESS;
    }


    uint8_t responseA[22];
    uint8_t responseB[22];
    int respAlen = 0;
    int respBlen = 0;

    // --------------- First ----------------
    uint8_t aFIRST[] = { 0x00, 0xa6, 0xb0,  0x00,  0x10 };
    res = ExchangeAPDU14a(aFIRST, sizeof(aFIRST), activate_field, keep_field_on, responseA, sizeof(responseA), &respAlen);
    if (res) {
        DropField();
        return res;
    }

    // --------------- Second ----------------
    activate_field = false;
    keep_field_on = false;

    uint8_t aSECOND[] = { 0x00, 0xa6, 0xb0,  0x01,  0x10 };
    res = ExchangeAPDU14a(aSECOND, sizeof(aSECOND), activate_field, keep_field_on, responseB, sizeof(responseB), &respBlen);
    if (res) {
        DropField();
        return res;
    }

// uint8_t inA[] = { 0x72, 0xD7, 0xF4, 0x3E, 0xFD, 0xAB, 0xF2, 0x35, 0xFD, 0x49, 0xEE, 0xDC, 0x44, 0x95, 0x43, 0xC4};
// uint8_t inB[] = { 0xF0, 0xA2, 0x67, 0x6A, 0x04, 0x6A, 0x72, 0x12, 0x76, 0xA4, 0x1D, 0x02, 0x1F, 0xEA, 0x20, 0x85};

    uint8_t outA[16] = {0};
    uint8_t outB[16] = {0};

    uint8_t key[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    for (uint8_t i = 0; i < 16; i += 8) {
        des_decrypt(outA + i, responseA + i, key);
        des_decrypt(outB + i, responseB + i, key);
    }

    PrintAndLogEx(DEBUG, " in : %s", sprint_hex_inrow(responseA, respAlen));
    PrintAndLogEx(DEBUG, "out : %s", sprint_hex_inrow(outA, sizeof(outA)));
    PrintAndLogEx(DEBUG, " in : %s", sprint_hex_inrow(responseB, respAlen));
    PrintAndLogEx(DEBUG, "out : %s", sprint_hex_inrow(outB, sizeof(outB)));

    if (memcmp(outA, "\x01\x01\x01\x01\x01\x01\x01\x01", 8) == 0) {
        PrintAndLogEx(INFO, "No trace recorded");
        return PM3_SUCCESS;
    }

    // second trace?
    if (memcmp(outB, "\x01\x01\x01\x01\x01\x01\x01\x01", 8) == 0) {
        PrintAndLogEx(INFO, "Only one trace recorded");
        return PM3_SUCCESS;
    }

    nonces_t data;

    // first
    uint16_t NT0 = (outA[6] << 8) | outA[7];
    data.cuid = bytes_to_num(outA, 4);
    data.nonce = prng_successor(NT0, 31);
    data.nr = bytes_to_num(outA + 8, 4);
    data.ar = bytes_to_num(outA + 12, 4);
    data.at = 0;

    // second
    NT0 = (outB[6] << 8) | outB[7];
    data.nonce2 =  prng_successor(NT0, 31);;
    data.nr2 = bytes_to_num(outB + 8, 4);
    data.ar2 = bytes_to_num(outB + 12, 4);
    data.sector = GetSectorFromBlockNo(outA[5]);
    data.keytype = outA[4];
    data.state = FIRST;

    PrintAndLogEx(DEBUG, "A Sector %02x", data.sector);
    PrintAndLogEx(DEBUG, "A NT  %08x", data.nonce);
    PrintAndLogEx(DEBUG, "A NR  %08x", data.nr);
    PrintAndLogEx(DEBUG, "A AR  %08x", data.ar);
    PrintAndLogEx(DEBUG, "");
    PrintAndLogEx(DEBUG, "B NT  %08x", data.nonce2);
    PrintAndLogEx(DEBUG, "B NR  %08x", data.nr2);
    PrintAndLogEx(DEBUG, "B AR  %08x", data.ar2);

    uint64_t key64 = -1;
    res = mfkey32_moebius(&data, &key64);

    if (res) {
        PrintAndLogEx(SUCCESS, "UID: %s Sector %02x key %c [ " _GREEN_("%12" PRIX64) " ]"
                      , sprint_hex_inrow(outA, 4)
                      , data.sector
                      , (data.keytype == 0x60) ? 'A' : 'B'
                      , key64);
    } else {
        PrintAndLogEx(FAILED, "failed to recover any key");
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfWipe(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf wipe",
                  "Wipe card to zeros and default keys/acc.  This command taks a key file to wipe card\n"
                  "New A/B keys  FF FF FF FF FF FF\n"
                  "New acc       FF 07 80\n"
                  "New GDB       69",
                  "hf mf wipe"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("f",  "file", "<fn>", "key filename"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int keyfnlen = 0;
    char keyFilename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)keyFilename, FILE_PATH_SIZE, &keyfnlen);

    CLIParserFree(ctx);

    char *fptr;
    if (keyfnlen == 0) {
        fptr = GenerateFilename("hf-mf-", "-key.bin");
        if (fptr == NULL)
            return PM3_ESOFT;

        strcpy(keyFilename, fptr);
        free(fptr);
    }

    uint8_t *keys;
    size_t keyslen = 0;
    if (loadFile_safeEx(keyFilename, ".bin", (void **)&keys, (size_t *)&keyslen, false) != PM3_SUCCESS) {
        PrintAndLogEx(FAILED, "failed to load key file");
        return PM3_ESOFT;
    }

    uint8_t keyA[MIFARE_4K_MAXSECTOR * 6];
    uint8_t keyB[MIFARE_4K_MAXSECTOR * 6];
    uint8_t num_sectors = 0;

    switch (keyslen) {
        case (MIFARE_MINI_MAXSECTOR * 2 * 6): {
            PrintAndLogEx(INFO, "Loaded keys matching MIFARE Classic Mini 320b");
            memcpy(keyA, keys, (MIFARE_MINI_MAXSECTOR * 6));
            memcpy(keyB, keys + (MIFARE_MINI_MAXSECTOR * 6), (MIFARE_MINI_MAXSECTOR * 6));
            num_sectors = NumOfSectors('0');
            break;
        }
        case (MIFARE_1K_MAXSECTOR * 2 * 6): {
            PrintAndLogEx(INFO, "Loaded keys matching MIFARE Classic 1K");
            memcpy(keyA, keys, (MIFARE_1K_MAXSECTOR * 6));
            memcpy(keyB, keys + (MIFARE_1K_MAXSECTOR * 6), (MIFARE_1K_MAXSECTOR * 6));
            num_sectors = NumOfSectors('1');
            break;
        }
        case (MIFARE_4K_MAXSECTOR * 2 * 6): {
            PrintAndLogEx(INFO, "Loaded keys matching MIFARE Classic 4K");
            memcpy(keyA, keys, (MIFARE_4K_MAXSECTOR * 6));
            memcpy(keyB, keys + (MIFARE_4K_MAXSECTOR * 6), (MIFARE_4K_MAXSECTOR * 6));
            num_sectors = NumOfSectors('4');
            break;
        }
        default: {
            PrintAndLogEx(INFO, "wrong key file size");
            goto out;
        }
    }

    uint8_t zeros[MFBLOCK_SIZE] = {0};
    memset(zeros, 0x00, sizeof(zeros));
    uint8_t st[MFBLOCK_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // time to wipe card
    for (uint8_t s = 0; s < num_sectors; s++) {

        for (uint8_t b = 0; b < NumBlocksPerSector(s); b++) {

            uint8_t data[26];
            memset(data, 0, sizeof(data));
            if (mfIsSectorTrailer(b)) {
                memcpy(data + 10, st, sizeof(st));
            } else {
                memcpy(data + 10, zeros, sizeof(zeros));
            }

            // try both A/B keys, start with B key first
            for (int8_t kt = 1; kt > -1; kt--) {

                if (kt == 0)
                    memcpy(data, keyA + (s * 6), 6);
                else
                    memcpy(data, keyB + (s * 6), 6);

                PrintAndLogEx(INFO, "Writing to block %3d: %s", FirstBlockOfSector(s) + b, sprint_hex(data + 10, MFBLOCK_SIZE));
                clearCommandBuffer();
                SendCommandMIX(CMD_HF_MIFARE_WRITEBL, FirstBlockOfSector(s) + b, kt, 0, data, sizeof(data));
                PacketResponseNG resp;

                if (WaitForResponseTimeout(CMD_ACK, &resp, 1500)) {
                    uint8_t isOK  = resp.oldarg[0] & 0xff;
                    if (isOK == 0) {
                        PrintAndLogEx(FAILED, "isOk: %02x", isOK);
                    } else {
                        break;
                    }
                } else {
                    PrintAndLogEx(WARNING, "Command execute timeout");
                }
            }
        }
    }

    PrintAndLogEx(INFO, "Done!");
out:
    free(keys);
    return PM3_SUCCESS;
}

static int CmdHF14AMfView(const char *Cmd) {
    
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf view",
                  "Print a MIFARE Classic dump file (bin/eml/json)",
                  "hf mf view -f hf-mf-01020304-dump.bin"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str1("f", "file", "<fn>", "filename of dump"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);    
    int fnlen = 0;
    char filename[FILE_PATH_SIZE];
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);
    bool verbose = arg_get_lit(ctx, 2);
    CLIParserFree(ctx); 

    // reserve memory
    uint8_t *dump = calloc(MFBLOCK_SIZE * MIFARE_4K_MAXBLOCK, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }

    size_t bytes_read = 0;
    int res = 0;
    DumpFileType_t dftype = getfiletype(filename);
    switch (dftype) {
        case BIN: {
            res = loadFile_safe(filename, ".bin", (void **)&dump, &bytes_read);
            break;
        }
        case EML: {
            res = loadFileEML_safe(filename, (void **)&dump, &bytes_read);
            break;
        }
        case JSON: {
            res = loadFileJSON(filename, dump, MIFARE_4K_MAXBLOCK * MFBLOCK_SIZE, &bytes_read, NULL);
            break;
        }
        case DICTIONARY: {
            PrintAndLogEx(ERR, "Error: Only BIN/JSON/EML formats allowed");
            return PM3_EINVARG;
        }
    }

    if (res != PM3_SUCCESS) {
        PrintAndLogEx(FAILED, "File: " _YELLOW_("%s") ": not found or locked.", filename);
        free(dump);
        return PM3_EFILE;
    }

    uint16_t block_cnt = MIN(MIFARE_1K_MAXBLOCK, (bytes_read / MFBLOCK_SIZE));
    if (bytes_read == 320)
        block_cnt = MIFARE_MINI_MAXBLOCK;
    else if (bytes_read == 2048)
        block_cnt = MIFARE_2K_MAXBLOCK;
    else if (bytes_read == 4096)
        block_cnt = MIFARE_4K_MAXBLOCK;

    if (verbose) {
        PrintAndLogEx(INFO, "File: " _YELLOW_("%s"), filename);
        PrintAndLogEx(INFO, "File size %zu bytes, file blocks %d (0x%x)", bytes_read, block_cnt, block_cnt);
    }

    mf_print_blocks(block_cnt, dump);
    free(dump);
    return PM3_SUCCESS;
}

static command_t CommandTable[] = {
    {"help",        CmdHelp,                AlwaysAvailable, "This help"},
    {"list",        CmdHF14AMfList,         AlwaysAvailable,  "List MIFARE history"},
    {"-----------", CmdHelp,                IfPm3Iso14443a,  "----------------------- " _CYAN_("recovery") " -----------------------"},
    {"darkside",    CmdHF14AMfDarkside,     IfPm3Iso14443a,  "Darkside attack"},
    {"nested",      CmdHF14AMfNested,       IfPm3Iso14443a,  "Nested attack"},
    {"hardnested",  CmdHF14AMfNestedHard,   AlwaysAvailable, "Nested attack for hardened MIFARE Classic cards"},
    {"staticnested", CmdHF14AMfNestedStatic, IfPm3Iso14443a,  "Nested attack against static nonce MIFARE Classic cards"},
    {"autopwn",     CmdHF14AMfAutoPWN,      IfPm3Iso14443a,  "Automatic key recovery tool for MIFARE Classic"},
//    {"keybrute",    CmdHF14AMfKeyBrute,     IfPm3Iso14443a,  "J_Run's 2nd phase of multiple sector nested authentication key recovery"},
    {"nack",        CmdHf14AMfNack,         IfPm3Iso14443a,  "Test for MIFARE NACK bug"},
    {"chk",         CmdHF14AMfChk,          IfPm3Iso14443a,  "Check keys"},
    {"fchk",        CmdHF14AMfChk_fast,     IfPm3Iso14443a,  "Check keys fast, targets all keys on card"},
    {"decrypt",     CmdHf14AMfDecryptBytes, AlwaysAvailable, "[nt] [ar_enc] [at_enc] [data] - to decrypt sniff or trace"},
    {"supercard",   CmdHf14AMfSuperCard,    IfPm3Iso14443a,  "Extract info from a `super card`"},
    {"-----------", CmdHelp,                IfPm3Iso14443a,  "----------------------- " _CYAN_("operations") " -----------------------"},
    {"auth4",       CmdHF14AMfAuth4,        IfPm3Iso14443a,  "ISO14443-4 AES authentication"},
    {"dump",        CmdHF14AMfDump,         IfPm3Iso14443a,  "Dump MIFARE Classic tag to binary file"},
    {"mad",         CmdHF14AMfMAD,          IfPm3Iso14443a,  "Checks and prints MAD"},
    {"ndef",        CmdHFMFNDEF,            IfPm3Iso14443a,  "Prints NDEF records from card"},
    {"personalize", CmdHFMFPersonalize,     IfPm3Iso14443a,  "Personalize UID (MIFARE Classic EV1 only)"},
    {"rdbl",        CmdHF14AMfRdBl,         IfPm3Iso14443a,  "Read MIFARE Classic block"},
    {"rdsc",        CmdHF14AMfRdSc,         IfPm3Iso14443a,  "Read MIFARE Classic sector"},
    {"restore",     CmdHF14AMfRestore,      IfPm3Iso14443a,  "Restore MIFARE Classic binary file to BLANK tag"},
    {"setmod",      CmdHf14AMfSetMod,       IfPm3Iso14443a,  "Set MIFARE Classic EV1 load modulation strength"},
    {"view",        CmdHF14AMfView,         AlwaysAvailable,  "Display content from tag dump file"},
    {"wipe",        CmdHF14AMfWipe,         IfPm3Iso14443a,  "Wipe card to zeros and default keys/acc"},
    {"wrbl",        CmdHF14AMfWrBl,         IfPm3Iso14443a,  "Write MIFARE Classic block"},

//    {"sniff",       CmdHF14AMfSniff,        0, "Sniff card-reader communication"},
    {"-----------", CmdHelp,                IfPm3Iso14443a,  "----------------------- " _CYAN_("simulation") " -----------------------"},
    {"sim",         CmdHF14AMfSim,          IfPm3Iso14443a,  "Simulate MIFARE card"},
    {"ecfill",      CmdHF14AMfECFill,       IfPm3Iso14443a,  "Fill emulator memory with help of keys from emulator"},
    {"eclr",        CmdHF14AMfEClear,       IfPm3Iso14443a,  "Clear emulator memory"},
    {"egetblk",     CmdHF14AMfEGetBlk,      IfPm3Iso14443a,  "Get emulator memory block"},
    {"egetsc",      CmdHF14AMfEGetSc,       IfPm3Iso14443a,  "Get emulator memory sector"},
    {"ekeyprn",     CmdHF14AMfEKeyPrn,      IfPm3Iso14443a,  "Print keys from emulator memory"},
    {"eload",       CmdHF14AMfELoad,        IfPm3Iso14443a,  "Load from file emul dump"},
    {"esave",       CmdHF14AMfESave,        IfPm3Iso14443a,  "Save to file emul dump"},
    {"esetblk",     CmdHF14AMfESet,         IfPm3Iso14443a,  "Set emulator memory block"},
    {"eview",       CmdHF14AMfEView,        IfPm3Iso14443a,  "View emulator memory"},
    {"-----------", CmdHelp,                IfPm3Iso14443a,  "----------------------- " _CYAN_("magic gen1") " -----------------------"},
    {"cgetblk",     CmdHF14AMfCGetBlk,      IfPm3Iso14443a,  "Read block from card"},
    {"cgetsc",      CmdHF14AMfCGetSc,       IfPm3Iso14443a,  "Read sector from card"},
    {"cload",       CmdHF14AMfCLoad,        IfPm3Iso14443a,  "Load dump to card"},
    {"csave",       CmdHF14AMfCSave,        IfPm3Iso14443a,  "Save dump from card into file or emulator"},
    {"csetblk",     CmdHF14AMfCSetBlk,      IfPm3Iso14443a,  "Write block to card"},
    {"csetuid",     CmdHF14AMfCSetUID,      IfPm3Iso14443a,  "Set UID on card"},
    {"cview",       CmdHF14AMfCView,        IfPm3Iso14443a,  "View card"},
    {"cwipe",       CmdHF14AMfCWipe,        IfPm3Iso14443a,  "Wipe card to default UID/Sectors/Keys"},
    {"-----------", CmdHelp,                IfPm3Iso14443a,  "----------------------- " _CYAN_("magic gen3") " -----------------------"},
    {"gen3uid",     CmdHf14AGen3UID,        IfPm3Iso14443a,  "Set UID without manufacturer block"},
    {"gen3blk",     CmdHf14AGen3Block,      IfPm3Iso14443a,  "Overwrite full manufacturer block"},
    {"gen3freeze",  CmdHf14AGen3Freeze,     IfPm3Iso14443a,  "Perma lock further UID changes"},

    {"-----------", CmdHelp,                IfPm3Iso14443a,  "----------------------- " _CYAN_("i") " -----------------------"},
    {"ice",         CmdHF14AMfice,          IfPm3Iso14443a,  "collect MIFARE Classic nonces to file"},
    {NULL, NULL, NULL, NULL}
};

static int CmdHelp(const char *Cmd) {
    (void)Cmd; // Cmd is not used so far
    CmdsHelp(CommandTable);
    return PM3_SUCCESS;
}

int CmdHFMF(const char *Cmd) {
    clearCommandBuffer();
    return CmdsParse(CommandTable, Cmd);
}
