#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <map>
#include <set>
#include <vector>
#include <string>

#include <capstone/capstone.h>

#include "elf.h"

#ifndef TRACE
#define TRACE 0
#endif

#define LABELS_64_BIT 1

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define u32be(x) (uint32_t)(((x & 0xff) << 24) + ((x & 0xff00) << 8) + ((x & 0xff0000) >> 8) + ((uint32_t)(x) >> 24))
#define u16be(x) (uint16_t)(((x & 0xff) << 8) + ((x & 0xff00) >> 8))
#define read_u32_be(buf) (uint32_t)(((buf)[0] << 24) + ((buf)[1] << 16) + ((buf)[2] << 8) + ((buf)[3]))

using namespace std;

struct Insn {
    uint32_t id;
    uint8_t op_count;
    string mnemonic;
    string op_str;
    cs_mips_op operands[8];

    uint8_t is_jump: 1;
    uint8_t is_global_got_memop: 1;
    int linked_insn;
    union {
        uint32_t linked_value;
        float linked_float;
    };
    uint32_t jtbl_addr;
    uint32_t num_cases;
    uint32_t index_reg;
};

static csh handle;

static const uint8_t *text_section;
static uint32_t text_section_len;
static uint32_t text_vaddr;

static const uint8_t *rodata_section;
static uint32_t rodata_section_len;
static uint32_t rodata_vaddr;

static const uint8_t *data_section;
static uint32_t data_section_len;
static uint32_t data_vaddr;

static uint32_t bss_section_len;
static uint32_t bss_vaddr;

static vector<Insn> insns;
static set<uint32_t> label_addresses;
static vector<uint32_t> got_globals;
static vector<uint32_t> got_locals;
static uint32_t gp_value;
static uint32_t gp_value_adj;

static map<uint32_t, string> symbol_names;

#define FLAG_NO_MEM 1
#define FLAG_VARARG 2

static const struct {
    const char *name;
    const char *params;
    int flags;
} extern_functions[] = {
    {"sbrk", "pi"},
    {"malloc", "pu"},
    {"calloc", "puu"},
    {"realloc", "ppu"},
    {"free", "vp"},
    {"fscanf", "ipp", FLAG_VARARG},
    {"printf", "ip", FLAG_VARARG},
    {"sprintf", "ipp", FLAG_VARARG},
    {"fprintf", "ipp", FLAG_VARARG},
    {"_doprnt", "ippp"},
    {"strlen", "up"},
    {"open", "ipii"},
    {"creat", "ipi"},
    {"access", "ipi"},
    {"rename", "ipp"},
    {"utime", "ipp"},
    {"flock", "iii"},
    {"ecvt", "pdipp"},
    {"fcvt", "pdipp"},
    {"atoi", "ip"},
    {"atol", "ip"},
    {"atof", "dp"},
    {"strtoul", "uppi"},
    {"strtod", "dpp"},
    {"strchr", "ppi"},
    {"strrchr", "ppi"},
    {"fstat", "iip"},
    {"stat", "ipp"},
    {"ftruncate", "iii"},
    {"bcopy", "vppu"},
    {"memcpy", "pppu"},
    {"memccpy", "pppiu"},
    {"read", "iipu"},
    {"write", "iipu"},
    {"fopen", "ppp"},
    {"freopen", "pppp"},
    {"fclose", "ip"},
    {"ftell", "ip"},
    {"rewind", "ip"},
    {"fseek", "ipii"},
    {"lseek", "iiii"},
    {"fflush", "ip"},
    {"dup", "ii"},
    {"dup2", "iii"},
    {"pipe", "ip"},
    {"perror", "vp"},
    {"fdopen", "iip"},
    {"memset", "vpiu"},
    {"bcmp", "ippu"},
    {"memcmp", "ippu"},
    {"getpid", "i", FLAG_NO_MEM},
    {"getpgrp", "i"},
    {"unlink", "ip"},
    {"close", "ii"},
    {"strcmp", "ipp"},
    {"strncmp", "ippu"},
    {"strcpy", "ppp"},
    {"strncpy", "pppu"},
    {"strcat", "ppp"},
    {"strncat", "pppu"},
    {"strtok", "ppp"},
    {"strstr", "ppp"},
    {"strdup", "pp"},
    {"toupper", "ii", FLAG_NO_MEM},
    {"tolower", "ii", FLAG_NO_MEM},
    {"gethostname", "ipu"},
    {"isatty", "ii"},
    {"times", "ip"},
    {"ctime", "pp"},
    {"setvbuf", "ippiu"},
    {"__semgetc", "ip"},
    {"__semputc", "iip"},
    {"fgetc", "ip"},
    {"fgets", "ipip"},
    {"__filbuf", "ip"},
    {"__flsbuf", "iip"},
    {"ungetc", "iip"},
    {"gets", "pp"},
    {"fread", "upuup"},
    {"fwrite", "upuup"},
    {"fputs", "ipp"},
    {"puts", "ip"},
    {"getcwd", "ppu"},
    {"time", "ip"},
    {"bzero", "vpu"},
    {"fp_class_d", "id", FLAG_NO_MEM},
    {"ldexp", "ddi", FLAG_NO_MEM},
    {"__ll_mul", "lll", FLAG_NO_MEM},
    {"__ll_div", "lll", FLAG_NO_MEM},
    {"__ll_rem", "ljl", FLAG_NO_MEM},
    {"__ll_lshift", "llj", FLAG_NO_MEM},
    {"__ll_rshift", "llj", FLAG_NO_MEM},
    {"__ull_div", "jjj", FLAG_NO_MEM},
    {"__ull_rem", "jjj", FLAG_NO_MEM},
    {"__ull_rshift", "jjj", FLAG_NO_MEM},
    {"__d_to_ull", "jd", FLAG_NO_MEM},
    {"__d_to_ll", "ld", FLAG_NO_MEM},
    {"__f_to_ull", "jf", FLAG_NO_MEM},
    {"__f_to_ll", "lf", FLAG_NO_MEM},
    {"__ull_to_f", "fj", FLAG_NO_MEM},
    {"__ll_to_f", "fl", FLAG_NO_MEM},
    {"__ull_to_d", "dj", FLAG_NO_MEM},
    {"__ll_to_d", "dl", FLAG_NO_MEM},
    {"_exit", "vi"},
    {"_cleanup", "v"},
    {"_rld_new_interface", "pu", FLAG_VARARG},
    {"_exithandle", "v"},
    {"_prctl", "ii", FLAG_VARARG},
    {"_atod", "dpii"},
    {"pathconf", "ipi"},
    {"getenv", "pp"},
    {"gettxt", "ppp"},
    {"mmap", "ppuiiii"},
    {"munmap", "ipu"},
    {"mprotect", "ipui"},
    {"sysconf", "ii"},
    {"getpagesize", "i"},
    {"strerror", "pi"},
    {"ioctl", "iiu", FLAG_VARARG},
    {"fcntl", "iii", FLAG_VARARG},
    {"signal", "pip"},
    {"sigset", "pip"},
    {"get_fpc_csr", "i"},
    {"set_fpc_csr", "ii"},
    {"tempnam", "ppp"},
    {"tmpnam", "pp"},
    {"mktemp", "pp"},
    {"wait", "ip"},
    {"kill", "iii"},
    {"execlp", "ip", FLAG_VARARG},
    {"execv", "ipp"},
    {"execvp", "ipp"},
    {"fork", "i"},
    {"tsearch", "pppp"},
    {"tfind", "pppp"},
    {"__assert", "vppi"},
};

static void disassemble(void) {
    csh handle;
    cs_insn *disasm;
    static size_t disasm_size;
    assert(cs_open(CS_ARCH_MIPS, (cs_mode)(CS_MODE_MIPS64 | CS_MODE_BIG_ENDIAN), &handle) == CS_ERR_OK);
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    disasm_size = cs_disasm(handle, text_section, text_section_len, text_vaddr, 0, &disasm);
    for (size_t i = 0; i < disasm_size; i++) {
        insns.push_back(Insn());
        Insn& insn = insns.back();
        insn.id = disasm[i].id;
        insn.mnemonic = disasm[i].mnemonic;
        insn.op_str = disasm[i].op_str;
        if (disasm[i].detail != nullptr && disasm[i].detail->mips.op_count > 0) {
            insn.op_count = disasm[i].detail->mips.op_count;
            memcpy(insn.operands, disasm[i].detail->mips.operands, sizeof(insn.operands));
        }
        insn.is_jump = cs_insn_group(handle, &disasm[i], MIPS_GRP_JUMP) || insn.id == MIPS_INS_JAL || insn.id == MIPS_INS_BAL || insn.id == MIPS_INS_JALR;
        insn.linked_insn = -1;
    }
    cs_free(disasm, disasm_size);
    cs_close(&handle);
}

// try to find a matching LUI for a given register
static void link_with_lui(int offset, uint32_t reg, int mem_imm)
{
#define MAX_LOOKBACK 128
    // don't attempt to compute addresses for zero offset
    // end search after some sane max number of instructions
    int end_search = MAX(0, offset - MAX_LOOKBACK);
    for (int search = offset - 1; search >= end_search; search--) {
        // use an `if` instead of `case` block to allow breaking out of the `for` loop
        if (insns[search].id == MIPS_INS_LUI) {
            uint32_t rd = insns[search].operands[0].reg;
            if (reg == rd) {
                break;
            }
        } else if (insns[search].id == MIPS_INS_LW ||
                        insns[search].id == MIPS_INS_LD ||
                        insns[search].id == MIPS_INS_ADDIU ||
                        //insns[search].id == MIPS_INS_ADDU || used in jump tables for offset
                        insns[search].id == MIPS_INS_ADD ||
                        insns[search].id == MIPS_INS_SUB ||
                        insns[search].id == MIPS_INS_SUBU) {
            uint32_t rd = insns[search].operands[0].reg;
            if (reg == rd) {
                if (insns[search].id == MIPS_INS_LW && insns[search].operands[1].mem.base == MIPS_REG_GP) {
                    int mem_imm0 = (int)insns[search].operands[1].mem.disp;
                    uint32_t got_entry = (mem_imm0 + gp_value_adj) / sizeof(uint32_t);
                    if (got_entry < got_locals.size()) {
                        // used for static functions
                        char buf[32];
                        uint32_t addr = got_locals[got_entry] + mem_imm;
                        insns[search].linked_insn = offset;
                        insns[search].linked_value = addr;
                        insns[offset].linked_insn = search;
                        insns[offset].linked_value = addr;

                        //vaddr_references[addr].insert(text_vaddr + offset * 4);

                        insns[search].id = MIPS_INS_LI;
                        insns[search].mnemonic = "li";
                        sprintf(buf, "$%s, 0x%x", cs_reg_name(handle, rd), addr);
                        insns[search].op_str = buf;
                        insns[search].operands[1].type = MIPS_OP_IMM;
                        insns[search].operands[1].imm = addr;

                        switch (insns[offset].id) {
                            case MIPS_INS_ADDIU:
                                insns[offset].id = MIPS_INS_MOVE;
                                insns[offset].operands[1].type = MIPS_OP_REG;
                                insns[offset].mnemonic = "move";
                                sprintf(buf, "$%s, $%s", cs_reg_name(handle, insns[offset].operands[0].reg), cs_reg_name(handle, rd));
                                insns[offset].op_str = buf;
                                break;
                            case MIPS_INS_LB:
                            case MIPS_INS_LBU:
                            case MIPS_INS_SB:
                            case MIPS_INS_LH:
                            case MIPS_INS_LHU:
                            case MIPS_INS_SH:
                            case MIPS_INS_LW:
                            case MIPS_INS_SW:
                            case MIPS_INS_LDC1:
                            case MIPS_INS_LWC1:
                            case MIPS_INS_SWC1:
                                insns[offset].operands[1].mem.disp = 0;
                                sprintf(buf, "$%s, ($%s)", cs_reg_name(handle, insns[offset].operands[0].reg), cs_reg_name(handle, rd));
                                insns[offset].op_str = buf;
                                break;
                            default:
                                assert(0);
                        }
                    }
                    break;
                } else {
                    // ignore: reg is pointer, offset is probably struct data member
                    break;
                }
            }
        } else if (insns[search].id == MIPS_INS_JR &&
                insns[search].operands[0].reg == MIPS_REG_RA && offset - search >= 2) {
            // stop looking when previous `jr ra` is hit,
            // but ignore if `offset` is branch delay slot for this `jr ra`
            break;
        }
    }
}

// for a given `jalr t9`, find the matching t9 load
static void link_with_jalr(int offset)
{
    // end search after some sane max number of instructions
    int end_search = MAX(0, offset - MAX_LOOKBACK);
    for (int search = offset - 1; search >= end_search; search--) {
        if (insns[search].operands[0].reg == MIPS_REG_T9) {
            if (insns[search].id == MIPS_INS_LW || insns[search].id == MIPS_INS_LI) {
                if (insns[search].is_global_got_memop || insns[search].id == MIPS_INS_LI) {
                    char buf[32];
                    sprintf(buf, "0x%x", insns[search].linked_value);
                    insns[search].linked_insn = offset;
                    insns[offset].linked_insn = search;
                    insns[offset].linked_value = insns[search].linked_value;
                    //insns[offset].label = insns[search].label;
                    //function_entry_points.insert(insns[search].linked_value);
                    insns[offset].id = MIPS_INS_JAL;
                    insns[offset].mnemonic = "jal";
                    insns[offset].op_str = buf;
                    insns[offset].operands[0].type = MIPS_OP_IMM;
                    insns[offset].operands[0].imm = insns[search].linked_value;
                    insns[search].id = MIPS_INS_NOP;
                    insns[search].mnemonic = "nop";
                    insns[search].op_str = "";
                }
                break;
            } else if (insns[search].id == MIPS_INS_ADDIU) {
                if (insns[search].linked_insn != -1) {
                    //function_entry_points.insert(insns[search].linked_value);
                    uint32_t first = insns[search].linked_insn;
                    insns[search].linked_insn = offset;
                    insns[offset].linked_insn = first;
                    insns[offset].linked_value = insns[search].linked_value;
                }
                break;
            } else if (insns[search].id == MIPS_INS_LI) {
                if (insns[search].linked_insn != -1) {
                    //function_entry_points.insert(insns[search].linked_value);
                    uint32_t first = insns[search].linked_insn;
                    insns[search].linked_insn = offset;
                    insns[offset].linked_insn = first;
                    insns[offset].linked_value = insns[search].linked_value;
                    insns[search].id = MIPS_INS_NOP;
                    insns[search].mnemonic = "nop";
                    insns[search].op_str = "";
                }
                break;
            } else if (insns[search].id == MIPS_INS_LD ||
                       insns[search].id == MIPS_INS_ADDU ||
                       insns[search].id == MIPS_INS_ADD ||
                       insns[search].id == MIPS_INS_SUB ||
                       insns[search].id == MIPS_INS_SUBU) {
                break;
            }
        } else if (insns[search].id == MIPS_INS_JR &&
                   insns[search].operands[0].reg == MIPS_REG_RA)
        {
            // stop looking when previous `jr ra` is hit
            break;
        }
    }
}

static void pass1(void) {
    for (size_t i = 0; i < insns.size(); i++) {
        Insn& insn = insns[i];
        if (insn.id == MIPS_INS_BAL) {
            insn.id = MIPS_INS_JAL;
            insn.mnemonic = "jal";
        }
        if (insn.is_jump) {
            if (insn.id == MIPS_INS_JAL || insn.id == MIPS_INS_J) {
                uint32_t target  = (uint32_t)insn.operands[0].imm;
                label_addresses.insert(target);
            } else if (insn.id == MIPS_INS_JR) {
                // sltiu $at, $ty, z
                // sw    $reg, offset($sp)   (very seldom, one or more, usually in func entry)
                // lw    $gp, offset($sp)    (if PIC, and very seldom)
                // beqz  $at, .L
                // some other instruction    (not always)
                // lui   $at, %hi(jtbl)
                // sll   $tx, $ty, 2
                // addu  $at, $at, $tx
                // lw    $tx, %lo(jtbl)($at)
                // nop                       (code compiled with 5.3)
                // addu  $tx, $tx, $gp       (if PIC)
                // jr    $tx

                // IDO 7.1:
                //lw      at,offset(gp)
                //andi    t9,t8,0x3f
                //sll     t9,t9,0x2
                //addu    at,at,t9
                //lw      t9,offset(at)
                //addu    t9,t9,gp
                //jr      t9

                // IDO 5.3:
                //lw      at,offset(gp)
                //andi    t3,t2,0x3f
                //sll     t3,t3,0x2
                //addu    at,at,t3
                //something
                //lw      t3,offset(at)
                //something
                //addu    t3,t3,gp
                //jr      t3
                if (i >= 7 && rodata_section != NULL) {
                    bool is_pic = insns[i - 1].id == MIPS_INS_ADDU && insns[i - 1].operands[2].reg == MIPS_REG_GP;
                    bool has_nop = insns[i - is_pic - 1].id == MIPS_INS_NOP;
                    bool has_extra = insns[i - is_pic - has_nop - 5].id != MIPS_INS_BEQZ;
                    int lw = i - is_pic - has_nop - 1;
                    if (insns[lw].id != MIPS_INS_LW) {
                        --lw;
                    }
                    if (insns[lw].id == MIPS_INS_LW && insns[lw].linked_insn != -1) {
                        int sltiu_index = -1;
                        int andi_index = -1;
                        for (int j = 3; j <= 4; j++) {
                            if (insns[lw - j].id == MIPS_INS_ANDI) {
                                andi_index = lw - j;
                                break;
                            }
                        }
                        if (andi_index == -1) {
                            for (int j = 5; j <= 14; j++) {
                                if (insns[lw - has_extra - j].id == MIPS_INS_SLTIU) {
                                    sltiu_index = j;
                                    break;
                                }
                            }
                        }
                        uint32_t num_cases;
                        bool found = false;
                        bool and_variant = false;
                        if (sltiu_index != -1 && insns[lw - has_extra - sltiu_index].id == MIPS_INS_SLTIU) {
                            num_cases = insns[lw - has_extra - sltiu_index].operands[2].imm;
                            found = true;
                        } else if (andi_index != -1) {
                            num_cases = insns[andi_index].operands[2].imm + 1;
                            found = true;
                            and_variant = true;
                        }
                        if (found) {
                            uint32_t jtbl_addr = insns[lw].linked_value;
                            if (is_pic) {
                                insns[i - 1].id = MIPS_INS_NOP;
                            }
                            uint32_t addu_index = lw - 1;
                            if (insns[addu_index].id != MIPS_INS_ADDU) {
                                --addu_index;
                            }
                            uint32_t index_reg = insns[addu_index - 1].operands[1].reg;
                            assert(insns[addu_index].id == MIPS_INS_ADDU);
                            assert(insns[addu_index - 1].id == MIPS_INS_SLL);
                            //printf("jump table at %08x, size %u\n", jtbl_addr, num_cases);
                            insn.jtbl_addr = jtbl_addr;
                            insn.num_cases = num_cases;
                            insn.index_reg = index_reg;
                            insns[lw].id = MIPS_INS_NOP;
                            insns[addu_index].id = MIPS_INS_NOP;
                            insns[addu_index - 1].id = MIPS_INS_NOP;
                            if (!and_variant) {
                                insns[addu_index - 2].id = MIPS_INS_NOP;
                            }

                            if (jtbl_addr < rodata_vaddr || jtbl_addr + num_cases * sizeof(uint32_t) > rodata_vaddr + rodata_section_len) {
                                fprintf(stderr, "jump table outside rodata\n");
                                exit(EXIT_FAILURE);
                            }
                            for (uint32_t i = 0; i < num_cases; i++) {
                                uint32_t target_addr = read_u32_be(rodata_section + (jtbl_addr - rodata_vaddr) + i * sizeof(uint32_t));
                                target_addr += gp_value;
                                //printf("%08X\n", target_addr);
                                label_addresses.insert(target_addr);
                            }
                        }
                    }
                }
            } else {
                for (int j = 0; j < insn.op_count; j++) {
                    if (insn.operands[j].type == MIPS_OP_IMM) {
                        uint32_t target  = (uint32_t)insn.operands[j].imm;
                        label_addresses.insert(target);
                    }
                }
            }
        }
        switch (insns[i].id) {
            // find floating point LI
            case MIPS_INS_MTC1:
            {
                unsigned int rt = insns[i].operands[0].reg;
                for (int s = i - 1; s >= 0; s--) {
                    if (insns[s].id == MIPS_INS_LUI && insns[s].operands[0].reg == rt) {
                        float f;
                        uint32_t lui_imm = (uint32_t)(insns[s].operands[1].imm << 16);
                        memcpy(&f, &lui_imm, sizeof(f));
                        insns[s].operands[1].imm <<= 16;
                        // link up the LUI with this instruction and the float
                        insns[s].linked_insn = i;
                        insns[s].linked_float = f;
                        // rewrite LUI instruction to be LI
                        insns[s].id = MIPS_INS_LI;
                        insns[s].mnemonic = "li";
                        break;
                    } else if (insns[s].id == MIPS_INS_LW ||
                                    insns[s].id == MIPS_INS_LD ||
                                    insns[s].id == MIPS_INS_LH ||
                                    insns[s].id == MIPS_INS_LHU ||
                                    insns[s].id == MIPS_INS_LB ||
                                    insns[s].id == MIPS_INS_LBU ||
                                    insns[s].id == MIPS_INS_ADDIU ||
                                    insns[s].id == MIPS_INS_ADD ||
                                    insns[s].id == MIPS_INS_SUB ||
                                    insns[s].id == MIPS_INS_SUBU) {
                        unsigned int rd = insns[s].operands[0].reg;
                        if (rt == rd) {
                            break;
                        }
                    } else if (insns[s].id == MIPS_INS_JR &&
                                    insns[s].operands[0].reg == MIPS_REG_RA) {
                        // stop looking when previous `jr ra` is hit
                        break;
                    }
                }
                break;
            }
            case MIPS_INS_SD:
            case MIPS_INS_SW:
            case MIPS_INS_SH:
            case MIPS_INS_SB:
            case MIPS_INS_LB:
            case MIPS_INS_LBU:
            case MIPS_INS_LD:
            case MIPS_INS_LDL:
            case MIPS_INS_LDR:
            case MIPS_INS_LH:
            case MIPS_INS_LHU:
            case MIPS_INS_LW:
            case MIPS_INS_LWU:
            case MIPS_INS_LDC1:
            case MIPS_INS_LWC1:
            case MIPS_INS_LWC2:
            case MIPS_INS_LWC3:
            case MIPS_INS_SWC1:
            case MIPS_INS_SWC2:
            case MIPS_INS_SWC3:
            {
                unsigned int mem_rs = insns[i].operands[1].mem.base;
                int mem_imm = (int)insns[i].operands[1].mem.disp;
                if (mem_rs == MIPS_REG_GP) {
                    unsigned int got_entry = (mem_imm + gp_value_adj) / sizeof(unsigned int);
                    if (got_entry >= got_locals.size()) {
                        got_entry -= got_locals.size();
                        if (got_entry < got_globals.size()) {
                            assert(insn.id == MIPS_INS_LW);
                            //printf("gp 0x%08x %s\n", mem_imm, got_globals[got_entry].name);
                            unsigned int dest_vaddr = got_globals[got_entry];
                            insns[i].is_global_got_memop = true;
                            insns[i].linked_value = dest_vaddr;
                            //insns[i].label = got_globals[got_entry].name;

                            //vaddr_references[dest_vaddr].insert(vaddr + i * 4);
                            //disasm_add_data_addr(state, dest_vaddr);
                            insns[i].id = MIPS_INS_LI;
                            insns[i].operands[1].imm = dest_vaddr;
                            char buf[32];
                            sprintf(buf, "$%s, 0x%x", cs_reg_name(handle, insn.operands[0].reg), dest_vaddr);
                            insns[i].op_str = buf;
                        }
                    }
                } else {
                    link_with_lui(i, mem_rs, mem_imm);
                }
                break;
            }
            case MIPS_INS_ADDIU:
            case MIPS_INS_ORI:
            {
                unsigned int rd = insns[i].operands[0].reg;
                unsigned int rs = insns[i].operands[1].reg;
                int64_t imm = insns[i].operands[2].imm;
                if (rs == MIPS_REG_ZERO) { // becomes LI
                    char buf[32];
                    insns[i].id = MIPS_INS_LI;
                    insns[i].operands[1].imm = imm;
                    insns[i].mnemonic = "li";
                    sprintf(buf, "$%s, %" PRIi64, cs_reg_name(handle, rd), imm);
                    insns[i].op_str = buf;
                } else if (/*rd == rs &&*/ rd != MIPS_REG_GP) { // only look for LUI if rd and rs are the same
                    link_with_lui(i, rs, (int)imm);
                }
                break;
            }
            case MIPS_INS_JALR:
            {
                unsigned int r = insn.operands[0].reg;
                if (r == MIPS_REG_T9) {
                    link_with_jalr(i);
                    if (insn.linked_insn != -1) {
                        char buf[32];
                        sprintf(buf, "0x%x", insn.linked_value);
                        insn.id = MIPS_INS_JAL;
                        insn.mnemonic = "jal";
                        insn.op_str = buf;
                        insn.operands[0].type = MIPS_OP_IMM;
                        insn.operands[0].imm = insn.linked_value;
                        label_addresses.insert(insn.linked_value);
                    }
                }
                break;
            }
        }
        if (insn.id == MIPS_INS_ADDU && insn.operands[0].reg == MIPS_REG_GP && insn.operands[1].reg == MIPS_REG_GP && insn.operands[2].reg == MIPS_REG_T9 && i >= 2) {
            //state->function_entry_points.insert(vaddr + (i - 2) * 4);
            for (int j = i - 2; j <= i; j++) {
                insns[j].id = MIPS_INS_NOP;
                insns[j].mnemonic = "nop";
                insns[j].op_str = "";
            }
        }
    }
}

static void pass2(void) {
    for (size_t i = 0; i < insns.size(); i++) {
        Insn& insn = insns[i];
        if (insn.is_jump) {
            //if (insn.id == MIPS_INS_JAL || insn.id == MIPS_INS_J
        }
    }
}

static void dump(void) {
    for (size_t i = 0; i < insns.size(); i++) {
        Insn& insn = insns[i];
        uint32_t vaddr = text_vaddr + i * 4;
        if (label_addresses.count(vaddr)) {
            if (symbol_names.count(vaddr)) {
                printf("L%08x: //%s\n", vaddr, symbol_names[vaddr].c_str());
            } else {
                printf("L%08x:\n", vaddr);
            }
        }
        printf("\t%s %s\n", insn.mnemonic.c_str(), insn.op_str.c_str());
    }
}

static const char *r(uint32_t reg) {
    return cs_reg_name(handle, reg);
}

static const char *wr(uint32_t reg) {
    static const char *regs[] = {
        "f0.w[0]", "f0.w[1]",
        "f2.w[0]", "f2.w[1]",
        "f4.w[0]", "f4.w[1]",
        "f6.w[0]", "f6.w[1]",
        "f8.w[0]", "f8.w[1]",
        "f10.w[0]", "f10.w[1]",
        "f12.w[0]", "f12.w[1]",
        "f14.w[0]", "f14.w[1]",
        "f16.w[0]", "f16.w[1]",
        "f18.w[0]", "f18.w[1]",
        "f20.w[0]", "f20.w[1]",
        "f22.w[0]", "f22.w[1]",
        "f24.w[0]", "f24.w[1]",
        "f26.w[0]", "f26.w[1]",
        "f28.w[0]", "f28.w[1]",
        "f30.w[0]", "f30.w[1]"
    };
    assert(reg >= MIPS_REG_F0 && reg <= MIPS_REG_F31);
    return regs[reg - MIPS_REG_F0];
}

static const char *fr(uint32_t reg) {
    static const char *regs[] = {
        "f0.f[0]", "f0.f[1]",
        "f2.f[0]", "f2.f[1]",
        "f4.f[0]", "f4.f[1]",
        "f6.f[0]", "f6.f[1]",
        "f8.f[0]", "f8.f[1]",
        "f10.f[0]", "f10.f[1]",
        "f12.f[0]", "f12.f[1]",
        "f14.f[0]", "f14.f[1]",
        "f16.f[0]", "f16.f[1]",
        "f18.f[0]", "f18.f[1]",
        "f20.f[0]", "f20.f[1]",
        "f22.f[0]", "f22.f[1]",
        "f24.f[0]", "f24.f[1]",
        "f26.f[0]", "f26.f[1]",
        "f28.f[0]", "f28.f[1]",
        "f30.f[0]", "f30.f[1]"
    };
    assert(reg >= MIPS_REG_F0 && reg <= MIPS_REG_F31);
    return regs[reg - MIPS_REG_F0];
}

static const char *dr(uint32_t reg) {
    static const char *regs[] = {
        "f0.d",
        "f2.d",
        "f4.d",
        "f6.d",
        "f8.d",
        "f10.d",
        "f12.d",
        "f14.d",
        "f16.d",
        "f18.d",
        "f20.d",
        "f22.d",
        "f24.d",
        "f26.d",
        "f28.d",
        "f30.d"
    };
    assert(reg >= MIPS_REG_F0 && reg <= MIPS_REG_F31 && (reg - MIPS_REG_F0) % 2 == 0);
    return regs[(reg - MIPS_REG_F0) / 2];
}

static void dump_instr(int i);

static void dump_cond_branch(int i, const char *lhs, const char *op, const char *rhs) {
    Insn& insn = insns[i];
    const char *cast1 = "";
    const char *cast2 = "";
    if (strcmp(op, "==") && strcmp(op, "!=")) {
        cast1 = "(int)";
        if (strcmp(rhs, "0")) {
            cast2 = "(int)";
        }
    }
    printf("if (%s%s %s %s%s) {", cast1, lhs, op, cast2, rhs);
    dump_instr(i + 1);
    printf("goto L%x;}\n", (uint32_t)insn.operands[insn.op_count - 1].imm);
}

static void dump_cond_branch_likely(int i, const char *lhs, const char *op, const char *rhs) {
    uint32_t target = text_vaddr + (i + 2) * 4;
    dump_cond_branch(i, lhs, op, rhs);
    if (!TRACE) {
        printf("else goto L%x;\n", target);
    } else {
        printf("else {printf(\"pc=0x%08x (ignored)\\n\"); goto L%x;}\n", text_vaddr + (i + 1) * 4, target);
    }
    label_addresses.insert(target);
}

static void dump_instr(int i) {
    const char *symbol_name = NULL;
    if (symbol_names.count(text_vaddr + i * 4) != 0) {
        symbol_name = symbol_names[text_vaddr + i * 4].c_str();
        printf("//%s:\n", symbol_name);
    }
    if (TRACE) {
        printf("++cnt; printf(\"pc=0x%08x%s%s\\n\"); ", text_vaddr + i * 4, symbol_name ? " " : "", symbol_name ? symbol_name : "");
    }
    Insn& insn = insns[i];
    switch (insn.id) {
        case MIPS_INS_ADD:
        case MIPS_INS_ADDU:
            if (insn.mnemonic == "add.s") {
                printf("%s = %s + %s;\n", fr(insn.operands[0].reg), fr(insn.operands[1].reg), fr(insn.operands[2].reg));
            } else if (insn.mnemonic == "add.d") {
                printf("%s = %s + %s;\n", dr(insn.operands[0].reg), dr(insn.operands[1].reg), dr(insn.operands[2].reg));
            } else {
                printf("%s = %s + %s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), r(insn.operands[2].reg));
            }
            break;
        case MIPS_INS_ADDI:
        case MIPS_INS_ADDIU:
            printf("%s = %s + 0x%x;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (uint32_t)insn.operands[2].imm);
            break;
        case MIPS_INS_AND:
            printf("%s = %s & %s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), r(insn.operands[2].reg));
            break;
        case MIPS_INS_ANDI:
            printf("%s = %s & 0x%x;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (uint32_t)insn.operands[2].imm);
            break;
        case MIPS_INS_BEQ:
            dump_cond_branch(i, r(insn.operands[0].reg), "==", r(insn.operands[1].reg));
            break;
        case MIPS_INS_BEQL:
            dump_cond_branch_likely(i, r(insn.operands[0].reg), "==", r(insn.operands[1].reg));
            break;
        case MIPS_INS_BGEZ:
            dump_cond_branch(i, r(insn.operands[0].reg), ">=", "0");
            break;
        case MIPS_INS_BGEZL:
            dump_cond_branch_likely(i, r(insn.operands[0].reg), ">=", "0");
            break;
        case MIPS_INS_BGTZ:
            dump_cond_branch(i, r(insn.operands[0].reg), ">", "0");
            break;
        case MIPS_INS_BGTZL:
            dump_cond_branch_likely(i, r(insn.operands[0].reg), ">", "0");
            break;
        case MIPS_INS_BLEZ:
            dump_cond_branch(i, r(insn.operands[0].reg), "<=", "0");
            break;
        case MIPS_INS_BLEZL:
            dump_cond_branch_likely(i, r(insn.operands[0].reg), "<=", "0");
            break;
        case MIPS_INS_BLTZ:
            dump_cond_branch(i, r(insn.operands[0].reg), "<", "0");
            break;
        case MIPS_INS_BLTZL:
            dump_cond_branch_likely(i, r(insn.operands[0].reg), "<", "0");
            break;
        case MIPS_INS_BNE:
            dump_cond_branch(i, r(insn.operands[0].reg), "!=", r(insn.operands[1].reg));
            break;
        case MIPS_INS_BNEL:
            dump_cond_branch_likely(i, r(insn.operands[0].reg), "!=", insn.mnemonic == "bnezl" ? "0" : r(insn.operands[1].reg));
            break;
        case MIPS_INS_BREAK:
            printf("abort();\n");
            break;
        case MIPS_INS_BEQZ:
            dump_cond_branch(i, r(insn.operands[0].reg), "==", "0");
            break;
        /*case MIPS_INS_BEQZL:
            dump_cond_branch_likely(i, r(insn.operands[0].reg), "==", "0");
            break;*/
        case MIPS_INS_B:
            dump_instr(i + 1);
            printf("goto L%x;\n", (int32_t)insn.operands[0].imm);
            break;
        case MIPS_INS_BC1F:
        case MIPS_INS_BC1T:
            printf("if (%scf) {", insn.id == MIPS_INS_BC1F ? "!" : "");
            dump_instr(i + 1);
            printf("goto L%x;}\n", (int32_t)insn.operands[0].imm);
            break;
        case MIPS_INS_BC1FL:
        case MIPS_INS_BC1TL:
        {
            uint32_t target = text_vaddr + (i + 2) * 4;
            printf("if (%scf) {", insn.id == MIPS_INS_BC1FL ? "!" : "");
            dump_instr(i + 1);
            printf("goto L%x;}\n", (int32_t)insn.operands[0].imm);
            if (!TRACE) {
                printf("else goto L%x;\n", target);
            } else {
                printf("else {printf(\"pc=0x%08x (ignored)\\n\"); goto L%x;}\n", text_vaddr + (i + 1) * 4, target);
            }
            label_addresses.insert(target);
            break;
        }
        case MIPS_INS_BNEZ:
            dump_cond_branch(i, r(insn.operands[0].reg), "!=", "0");
            break;
        /*case MIPS_INS_BNEZL:
            dump_cond_branch_likely(i, r(insn.operands[0].reg), "!=", "0");
            break;*/
        case MIPS_INS_C:
            if (insn.mnemonic == "c.lt.s") {
                printf("cf = %s < %s;\n", fr(insn.operands[0].reg), fr(insn.operands[1].reg));
            } else if (insn.mnemonic == "c.le.s") {
                printf("cf = %s <= %s;\n", fr(insn.operands[0].reg), fr(insn.operands[1].reg));
            } else if (insn.mnemonic == "c.eq.s") {
                printf("cf = %s == %s;\n", fr(insn.operands[0].reg), fr(insn.operands[1].reg));
            } else if (insn.mnemonic == "c.lt.d") {
                printf("cf = %s < %s;\n", dr(insn.operands[0].reg), dr(insn.operands[1].reg));
            } else if (insn.mnemonic == "c.le.d") {
                printf("cf = %s <= %s;\n", dr(insn.operands[0].reg), dr(insn.operands[1].reg));
            } else if (insn.mnemonic == "c.eq.d") {
                printf("cf = %s == %s;\n", dr(insn.operands[0].reg), dr(insn.operands[1].reg));
            }
            break;
        case MIPS_INS_CVT:
            if (insn.mnemonic == "cvt.s.w") {
                printf("%s = (int)%s;\n", fr(insn.operands[0].reg), wr(insn.operands[1].reg));
            } else if (insn.mnemonic == "cvt.d.w") {
                printf("%s = (int)%s;\n", dr(insn.operands[0].reg), wr(insn.operands[1].reg));
            } else if (insn.mnemonic == "cvt.d.s") {
                printf("%s = %s;\n", dr(insn.operands[0].reg), fr(insn.operands[1].reg));
            } else if (insn.mnemonic == "cvt.s.d") {
                printf("%s = %s;\n", fr(insn.operands[0].reg), dr(insn.operands[1].reg));
            } else if (insn.mnemonic == "cvt.w.d") {
                printf("%s = cvt_w_d(%s);\n", wr(insn.operands[0].reg), dr(insn.operands[1].reg));
            } else if (insn.mnemonic == "cvt.w.s") {
                printf("%s = cvt_w_s(%s);\n", wr(insn.operands[0].reg), fr(insn.operands[1].reg));
            } else {
                goto unimplemented;
            }
            break;
        case MIPS_INS_CFC1:
            assert(insn.operands[1].reg == MIPS_REG_31);
            printf("%s = fcsr;\n", r(insn.operands[0].reg));
            break;
        case MIPS_INS_CTC1:
            assert(insn.operands[1].reg == MIPS_REG_31);
            printf("fcsr = %s;\n", r(insn.operands[0].reg));
            break;
        case MIPS_INS_DIV:
            if (insn.mnemonic == "div.s") {
                assert(insn.op_count == 3);
                printf("%s = %s / %s;\n", fr(insn.operands[0].reg), fr(insn.operands[1].reg), fr(insn.operands[2].reg));
            } else if (insn.mnemonic == "div.d") {
                assert(insn.op_count == 3);
                printf("%s = %s / %s;\n", dr(insn.operands[0].reg), dr(insn.operands[1].reg), dr(insn.operands[2].reg));
            } else {
                assert(insn.op_count == 2);
                printf("lo = (int)%s / (int)%s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
                printf("hi = (int)%s %% (int)%s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
            }
            break;
        case MIPS_INS_DIVU:
            assert(insn.op_count == 2);
            printf("lo = %s / %s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
            printf("hi = %s %% %s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
            break;
        case MIPS_INS_MOV:
            if (insn.mnemonic == "mov.s") {
                printf("%s = %s;\n", fr(insn.operands[0].reg), fr(insn.operands[1].reg));
            } else if (insn.mnemonic == "mov.d") {
                printf("%s = %s;\n", dr(insn.operands[0].reg), dr(insn.operands[1].reg));
            } else {
                goto unimplemented;
            }
            break;
        case MIPS_INS_MUL:
            if (insn.mnemonic == "mul.s") {
                printf("%s = %s * %s;\n", fr(insn.operands[0].reg), fr(insn.operands[1].reg), fr(insn.operands[2].reg));
            } else if (insn.mnemonic == "mul.d") {
                printf("%s = %s * %s;\n", dr(insn.operands[0].reg), dr(insn.operands[1].reg), dr(insn.operands[2].reg));
            } else {
                goto unimplemented;
            }
            break;
        case MIPS_INS_NEG:
            if (insn.mnemonic == "neg.s") {
                printf("%s = -%s;\n", fr(insn.operands[0].reg), fr(insn.operands[1].reg));
            } else if (insn.mnemonic == "neg.d") {
                printf("%s = -%s;\n", dr(insn.operands[0].reg), dr(insn.operands[1].reg));
            } else {
                printf("%s = -%s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
            }
            break;
        case MIPS_INS_SUB:
            if (insn.mnemonic == "sub.s") {
                printf("%s = %s - %s;\n", fr(insn.operands[0].reg), fr(insn.operands[1].reg), fr(insn.operands[2].reg));
            } else if (insn.mnemonic == "sub.d") {
                printf("%s = %s - %s;\n", dr(insn.operands[0].reg), dr(insn.operands[1].reg), dr(insn.operands[2].reg));
            } else {
                goto unimplemented;
            }
            break;
        case MIPS_INS_J:
            dump_instr(i + 1);
            printf("goto L%x;\n", (uint32_t)insn.operands[0].imm);
            break;
        case MIPS_INS_JAL:
        {
            string name;
            bool is_extern_function = false;
            size_t extern_function_id;
            auto it = symbol_names.find(insn.operands[0].imm);
            if (it != symbol_names.end()) {
                name = it->second;
                for (size_t i = 0; i < sizeof(extern_functions) / sizeof(extern_functions[0]); i++) {
                    if (name == extern_functions[i].name) {
                        is_extern_function = true;
                        extern_function_id = i;
                        break;
                    }
                }
            }
            if (name == "exit") {
                dump_instr(i + 1);
                printf("if ((uint8_t)%s == 255) {\nabort();\n}\n", r(MIPS_REG_A0));
                printf("return %s;\n", r(MIPS_REG_A0));
                break;
            }
            if (!is_extern_function) {
                if (LABELS_64_BIT) {
                    printf("ra = (uint32_t)((uintptr_t)&&L%x - (uintptr_t)&&Loffset);\n", text_vaddr + (i + 2) * 4);
                } else {
                    printf("ra = (uint32_t)(uintptr_t)&&L%x;\n", text_vaddr + (i + 2) * 4);
                }
            }
            dump_instr(i + 1);
            if (is_extern_function) {
                auto& fn = extern_functions[extern_function_id];
                if (fn.flags & FLAG_VARARG) {
                    for (int j = 0; j < 4; j++) {
                        printf("MEM_U32(sp + %d) = %s;\n", j * 4, r(MIPS_REG_A0 + j));
                    }
                }
                char ret_type = fn.params[0];
                if (ret_type != 'v') {
                    switch (ret_type) {
                        case 'i':
                        case 'u':
                        case 'p':
                            printf("%s = ", r(MIPS_REG_V0));
                            break;
                        case 'f':
                            printf("%s = ", fr(MIPS_REG_F0));
                            break;
                        case 'd':
                            printf("%s = ", dr(MIPS_REG_F0));
                            break;
                        case 'l':
                        case 'j':
                            printf("temp64 = ");
                            break;
                    }
                }
                printf("wrapper_%s(", name.c_str());
                bool first = true;
                if (!(fn.flags & FLAG_NO_MEM)) {
                    printf("mem");
                    first = false;
                }
                int pos = 0;
                int pos_float = 0;
                bool only_floats_so_far = true;
                for (const char *p = fn.params + 1; *p != '\0'; ++p) {
                    if (!first) {
                        printf(", ");
                    }
                    first = false;
                    switch (*p) {
                        case 'i':
                        case 'u':
                        case 'p':
                            only_floats_so_far = false;
                            if (pos < 4) {
                                printf("%s", r(MIPS_REG_A0 + pos));
                            } else {
                                printf("MEM_%c32(sp + %d)", *p == 'i' ? 'S' : 'U', pos * 4);
                            }
                            ++pos;
                            break;
                        case 'f':
                            if (only_floats_so_far && pos_float < 4) {
                                printf("%s", fr(MIPS_REG_F12 + pos_float));
                                pos_float += 2;
                            } else if (pos < 4) {
                                printf("BITCAST_U32_TO_F32(%s)", r(MIPS_REG_A0 + pos));
                            } else {
                                printf("BITCAST_U32_TO_F32(MEM_U32(sp + %d))", pos * 4);
                            }
                            ++pos;
                            break;
                        case 'd':
                            if (pos % 1 != 0) {
                                ++pos;
                            }
                            if (only_floats_so_far && pos_float < 4) {
                                printf("%s", dr(MIPS_REG_F12 + pos_float));
                                pos_float += 2;
                            } else if (pos < 4) {
                                printf("BITCAST_U64_TO_F64(((uint64_t)%s << 32) | (uint64_t)%s)", r(MIPS_REG_A0 + pos), r(MIPS_REG_A0 + pos + 1));
                            } else {
                                printf("BITCAST_U64_TO_F64(((uint64_t)MEM_U32(sp + %d) << 32) | (uint64_t)MEM_U32(sp + %d))", pos * 4, (pos + 1) * 4);
                            }
                            pos += 2;
                            break;
                        case 'l':
                        case 'j':
                            if (pos % 1 != 0) {
                                ++pos;
                            }
                            only_floats_so_far = false;
                            if (*p == 'l') {
                                printf("(int64_t)");
                            }
                            if (pos < 4) {
                                printf("(((uint64_t)%s << 32) | (uint64_t)%s)", r(MIPS_REG_A0 + pos), r(MIPS_REG_A0 + pos + 1));
                            } else {
                                printf("(((uint64_t)MEM_U32(sp + %d) << 32) | (uint64_t)MEM_U32(sp + %d))", pos * 4, (pos + 1) * 4);
                            }
                            pos += 2;
                            break;
                    }
                }
                if (fn.flags & FLAG_VARARG) {
                    printf("%s%s", first ? "" : ", ", r(MIPS_REG_SP));
                }
                printf(");\n");
                if (ret_type == 'l' || ret_type == 'j') {
                    printf("%s = (uint32_t)(temp64 >> 32);\n", r(MIPS_REG_V0));
                    printf("%s = (uint32_t)temp64;\n", r(MIPS_REG_V1));
                }
                if (!name.empty()) {
                    //printf("printf(\"%s %%x\\n\", %s);\n", name.c_str(), r(MIPS_REG_A0));
                }
                printf("goto L%x;\n", text_vaddr + (i + 2) * 4);
            } else {
                if (!name.empty()) {
                    //printf("printf(\"%s %%x\\n\", %s);\n", name.c_str(), r(MIPS_REG_A0));
                }
                printf("goto L%x;", (uint32_t)insn.operands[0].imm);
                if (!name.empty()) {
                    printf(" // %s\n", name.c_str());
                }
                printf("\n");
            }
            label_addresses.insert(text_vaddr + (i + 2) * 4);
            break;
        }
        case MIPS_INS_JALR:
            if (LABELS_64_BIT) {
                printf("dest = (void *)((uintptr_t)%s + (uintptr_t)&&Loffset);\n", r(insn.operands[0].reg));
                printf("ra = (uint32_t)((uintptr_t)&&L%x - (uintptr_t)&&Loffset);\n", text_vaddr + (i + 2) * 4);
            } else {
                printf("dest = (void *)(uintptr_t)%s;\n", r(insn.operands[0].reg));
                printf("ra = (uint32_t)(uintptr_t)&&L%x;\n", text_vaddr + (i + 2) * 4);
            }
            dump_instr(i + 1);
            printf("goto *dest;\n");
            label_addresses.insert(text_vaddr + (i + 2) * 4);
            break;
        case MIPS_INS_JR:
            if (insn.jtbl_addr != 0) {
                uint32_t jtbl_pos = insn.jtbl_addr - rodata_vaddr;
                assert(jtbl_pos < rodata_section_len && jtbl_pos + insn.num_cases * 4 <= rodata_section_len);
                printf(";static void *const Lswitch%x[] = {\n", insn.jtbl_addr);
                for (uint32_t i = 0; i < insn.num_cases; i++) {
                    uint32_t dest_addr = read_u32_be(rodata_section + jtbl_pos + i * 4) + gp_value;
                    printf("&&L%x,\n", dest_addr);
                    label_addresses.insert(dest_addr);
                }
                printf("};\n");
                printf("dest = Lswitch%x[%s];\n", insn.jtbl_addr, r(insn.index_reg));
                dump_instr(i + 1);
                printf("goto *dest;\n");
            } else {
                if (LABELS_64_BIT) {
                    printf("dest = (void *)((uintptr_t)%s + (uintptr_t)&&Loffset);\n", r(insn.operands[0].reg));
                } else {
                    printf("dest = (void *)(uintptr_t)%s;\n", r(insn.operands[0].reg));
                }
                dump_instr(i + 1);
                printf("goto *dest;\n");
            }
            break;
        case MIPS_INS_LB:
            printf("%s = MEM_S8(%s + %d);\n", r(insn.operands[0].reg), r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp);
            break;
        case MIPS_INS_LBU:
            printf("%s = MEM_U8(%s + %d);\n", r(insn.operands[0].reg), r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp);
            break;
        case MIPS_INS_LH:
            printf("%s = MEM_S16(%s + %d);\n", r(insn.operands[0].reg), r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp);
            break;
        case MIPS_INS_LHU:
            printf("%s = MEM_U16(%s + %d);\n", r(insn.operands[0].reg), r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp);
            break;
        case MIPS_INS_LUI:
            printf("%s = 0x%x;\n", r(insn.operands[0].reg), ((uint32_t)insn.operands[1].imm) << 16);
            break;
        case MIPS_INS_LW:
            printf("%s = MEM_U32(%s + %d);\n", r(insn.operands[0].reg), r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp);
            break;
        case MIPS_INS_LWC1:
            printf("%s = MEM_U32(%s + %d);\n", wr(insn.operands[0].reg), r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp);
            break;
        case MIPS_INS_LDC1:
            assert((insn.operands[0].reg - MIPS_REG_F0) % 2 == 0);
            printf("%s = MEM_U32(%s + %d);\n", wr(insn.operands[0].reg + 1), r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp);
            printf("%s = MEM_U32(%s + %d + 4);\n", wr(insn.operands[0].reg), r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp);
            break;
        case MIPS_INS_LWL:
        {
            const char *reg = r(insn.operands[0].reg);
            printf("%s = %s + %d; ", reg, r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp);
            printf("%s = (MEM_U8(%s) << 24) | (MEM_U8(%s + 1) << 16) | (MEM_U8(%s + 2) << 8) | MEM_U8(%s + 3);\n", reg, reg, reg, reg, reg);
            break;
        }
        case MIPS_INS_LWR:
            printf("//lwr %s\n", insn.op_str.c_str());
            break;
        case MIPS_INS_LI:
            if (insn.is_global_got_memop && text_vaddr <= insn.operands[1].imm && insn.operands[1].imm < text_vaddr + text_section_len) {
                if (LABELS_64_BIT) {
                    printf("%s = (uint32_t)((uintptr_t)&&L%x - (uintptr_t)&&Loffset);\n", r(insn.operands[0].reg), (uint32_t)insn.operands[1].imm);
                } else {
                    printf("%s = (uint32_t)(uintptr_t)&&L%x;\n", r(insn.operands[0].reg), (uint32_t)insn.operands[1].imm);
                }
                label_addresses.insert((uint32_t)insn.operands[1].imm);
            } else {
                printf("%s = 0x%x;\n", r(insn.operands[0].reg), (uint32_t)insn.operands[1].imm);
            }
            break;
        case MIPS_INS_MFC1:
            printf("%s = %s;\n", r(insn.operands[0].reg), wr(insn.operands[1].reg));
            break;
        case MIPS_INS_MFHI:
            printf("%s = hi;\n", r(insn.operands[0].reg));
            break;
        case MIPS_INS_MFLO:
            printf("%s = lo;\n", r(insn.operands[0].reg));
            break;
        case MIPS_INS_MOVE:
            printf("%s = %s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
            break;
        case MIPS_INS_MTC1:
            printf("%s = %s;\n", wr(insn.operands[1].reg), r(insn.operands[0].reg));
            break;
        case MIPS_INS_MULT:
            printf("lo = %s * %s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
            printf("hi = (uint32_t)((int64_t)(int)%s * (int64_t)(int)%s >> 32);\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
            break;
        case MIPS_INS_MULTU:
            printf("lo = %s * %s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
            printf("hi = (uint32_t)((uint64_t)%s * (uint64_t)%s >> 32);\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
            break;
        case MIPS_INS_NEGU:
            printf("%s = -%s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
            break;
        case MIPS_INS_NOR:
            printf("%s = ~(%s | %s);\n", r(insn.operands[0].reg), r(insn.operands[1].reg), r(insn.operands[2].reg));
            break;
        case MIPS_INS_NOT:
            printf("%s = ~%s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg));
            break;
        case MIPS_INS_OR:
            printf("%s = %s | %s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), r(insn.operands[2].reg));
            break;
        case MIPS_INS_ORI:
            printf("%s = %s | 0x%x;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (uint32_t)insn.operands[2].imm);
            break;
        case MIPS_INS_SB:
            printf("MEM_U8(%s + %d) = (uint8_t)%s;\n", r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp, r(insn.operands[0].reg));
            break;
        case MIPS_INS_SH:
            printf("MEM_U16(%s + %d) = (uint16_t)%s;\n", r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp, r(insn.operands[0].reg));
            break;
        case MIPS_INS_SLL:
            printf("%s = %s << %d;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (uint32_t)insn.operands[2].imm);
            break;
        case MIPS_INS_SLLV:
            printf("%s = %s << (%s & 0x1f);\n", r(insn.operands[0].reg), r(insn.operands[1].reg), r(insn.operands[2].reg));
            break;
        case MIPS_INS_SLT:
            printf("%s = (int)%s < (int)%s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), r(insn.operands[2].reg));
            break;
        case MIPS_INS_SLTI:
            printf("%s = (int)%s < (int)0x%x;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (uint32_t)insn.operands[2].imm);
            break;
        case MIPS_INS_SLTIU:
            printf("%s = %s < 0x%x;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (uint32_t)insn.operands[2].imm);
            break;
        case MIPS_INS_SLTU:
            printf("%s = %s < %s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), r(insn.operands[2].reg));
            break;
        case MIPS_INS_SRA:
            printf("%s = (int)%s >> %d;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (uint32_t)insn.operands[2].imm);
            break;
        case MIPS_INS_SRAV:
            printf("%s = (int)%s >> (%s & 0x1f);\n", r(insn.operands[0].reg), r(insn.operands[1].reg), r(insn.operands[2].reg));
            break;
        case MIPS_INS_SRL:
            printf("%s = %s >> %d;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (uint32_t)insn.operands[2].imm);
            break;
        case MIPS_INS_SRLV:
            printf("%s = %s >> (%s & 0x1f);\n", r(insn.operands[0].reg), r(insn.operands[1].reg), r(insn.operands[2].reg));
            break;
        case MIPS_INS_SUBU:
            printf("%s = %s - %s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), r(insn.operands[2].reg));
            break;
        case MIPS_INS_SW:
            printf("MEM_U32(%s + %d) = %s;\n", r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp, r(insn.operands[0].reg));
            break;
        case MIPS_INS_SWC1:
            printf("MEM_U32(%s + %d) = %s;\n", r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp, wr(insn.operands[0].reg));
            break;
        case MIPS_INS_SDC1:
            assert((insn.operands[0].reg - MIPS_REG_F0) % 2 == 0);
            printf("MEM_U32(%s + %d) = %s;\n", r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp, wr(insn.operands[0].reg + 1));
            printf("MEM_U32(%s + %d + 4) = %s;\n", r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp, wr(insn.operands[0].reg));
            break;
        case MIPS_INS_SWL:
            for (int i = 0; i < 4; i++) {
                printf("MEM_U8(%s + %d + %d) = (uint8_t)(%s >> %d);\n", r(insn.operands[1].mem.base), (int)insn.operands[1].mem.disp, i, r(insn.operands[0].reg), (3 - i) * 8);
            }
            break;
        case MIPS_INS_SWR:
            printf("//swr %s\n", insn.op_str.c_str());
            break;
        case MIPS_INS_TRUNC:
            if (insn.mnemonic == "trunc.w.s") {
                printf("%s = (int)%s;\n", wr(insn.operands[0].reg), fr(insn.operands[1].reg));
            } else if (insn.mnemonic == "trunc.w.d") {
                printf("%s = (int)%s;\n", wr(insn.operands[0].reg), dr(insn.operands[1].reg));
            } else {
                goto unimplemented;
            }
            break;
        case MIPS_INS_XOR:
            printf("%s = %s ^ %s;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), r(insn.operands[2].reg));
            break;
        case MIPS_INS_XORI:
            printf("%s = %s ^ 0x%x;\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (uint32_t)insn.operands[2].imm);
            break;
        case MIPS_INS_TNE:
            printf("assert(%s == %s && \"tne %d\");\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (int)insn.operands[2].imm);
            break;
        case MIPS_INS_TEQ:
            printf("assert(%s != %s && \"teq %d\");\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (int)insn.operands[2].imm);
            break;
        case MIPS_INS_TGE:
            printf("assert((int)%s < (int)%s && \"tge %d\");\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (int)insn.operands[2].imm);
            break;
        case MIPS_INS_TGEU:
            printf("assert(%s < %s && \"tgeu %d\");\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (int)insn.operands[2].imm);
            break;
        case MIPS_INS_TLT:
            printf("assert((int)%s >= (int)%s && \"tlt %d\");\n", r(insn.operands[0].reg), r(insn.operands[1].reg), (int)insn.operands[2].imm);
            break;
        case MIPS_INS_NOP:
            printf("//nop;\n");
            break;
        default:
            unimplemented:
            printf("UNIMPLEMENTED %s %s\n", insn.mnemonic.c_str(), insn.op_str.c_str());
            break;
    }
}

static void inspect_function_pointers(vector<pair<uint32_t, uint32_t>>& ret, const uint8_t *section, uint32_t section_vaddr, uint32_t len) {
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t addr = read_u32_be(section + i);
        if (addr == 0x430b00 || addr == 0x433b00) {
            // in as1, not function pointers (normal integers)
            continue;
        }
        if (addr == 0x4a0000) {
            // in copt
            continue;
        }
        if (addr >= text_vaddr && addr < text_vaddr + text_section_len && addr % 4 == 0) {
            fprintf(stderr, "assuming function pointer 0x%x at 0x%x\n", addr, section_vaddr + i);
            ret.push_back(make_pair(section_vaddr + i, addr));
            label_addresses.insert(addr);
        }
    }
}

static void dump_c(void) {
    map<string, uint32_t> symbol_names_inv;
    for (auto& it : symbol_names) {
        symbol_names_inv[it.second] = it.first;
    }
    
    uint32_t min_addr = data_vaddr;
    uint32_t max_addr = data_vaddr + data_section_len;

    min_addr = MIN(min_addr, rodata_vaddr);
    min_addr = MIN(min_addr, bss_vaddr);
    max_addr = MAX(max_addr, rodata_vaddr + rodata_section_len);
    max_addr = MAX(max_addr, bss_vaddr + bss_section_len);

    min_addr = min_addr & ~0xfff;
    max_addr = (max_addr + 0xfff) & ~0xfff;

    uint32_t stack_bottom = min_addr;
    min_addr -= 1 * 1024 * 1024; // 1 MB stack
    stack_bottom -= 16; // for main's stack frame

    printf("static const uint32_t rodata[] = {\n");
    for (size_t i = 0; i < rodata_section_len; i += 4) {
        printf("0x%x,%s", read_u32_be(rodata_section + i), i % 32 == 28 ? "\n" : "");
    }
    printf("};\n");
    printf("static const uint32_t data[] = {\n");
    for (size_t i = 0; i < data_section_len; i += 4) {
        printf("0x%x,%s", read_u32_be(data_section + i), i % 32 == 28 ? "\n" : "");
    }
    printf("};\n");

    vector<pair<uint32_t, uint32_t>> function_pointers;
    inspect_function_pointers(function_pointers, rodata_section, rodata_vaddr, rodata_section_len);
    inspect_function_pointers(function_pointers, data_section, data_vaddr, data_section_len);

    if (!function_pointers.empty()) {
        printf("static const struct { uint32_t orig_addr; void *recompiled_addr; } function_pointers[] = {\n");
        for (auto item : function_pointers) {
            printf("{0x%x, &&L%x},\n", item.first, item.second);
        }
        printf("};\n");
    }

    printf("mmap_initial_data_range(mem, 0x%x, 0x%x);\n", min_addr, max_addr);

    printf("memcpy(mem + 0x%x, rodata, 0x%x);\n", rodata_vaddr, rodata_section_len);
    printf("memcpy(mem + 0x%x, data, 0x%x);\n", data_vaddr, data_section_len);

    if (!function_pointers.empty()) {
        if (!LABELS_64_BIT) {
            printf("for (int i = 0; i < %d; i++) MEM_U32(function_pointers[i].orig_addr) = (uint32_t)(uintptr_t)function_pointers[i].recompiled_addr;\n", (int)function_pointers.size());
        } else {
            printf("for (int i = 0; i < %d; i++) MEM_U32(function_pointers[i].orig_addr) = (uint32_t)((uintptr_t)function_pointers[i].recompiled_addr - (uintptr_t)&&Loffset);\n", (int)function_pointers.size());
        }
    }

    printf("{\n");
    printf("MEM_S32(0x%x) = argc;\n", symbol_names_inv.at("__Argc"));
    printf("MEM_S32(0x%x) = argc;\n", stack_bottom);
    printf("%s = argc;\n", r(MIPS_REG_A0));
    printf("uint32_t al = argc * 4; for (int i = 0; i < argc; i++) al += strlen(argv[i]) + 1;\n");
    printf("uint32_t arg_addr = wrapper_malloc(mem, al);\n");
    printf("MEM_U32(0x%x) = arg_addr;\n", symbol_names_inv.at("__Argv"));
    printf("MEM_U32(0x%x) = arg_addr;\n", stack_bottom + 4);
    printf("%s = arg_addr;\n", r(MIPS_REG_A1));
    printf("uint32_t arg_strpos = arg_addr + argc * 4;\n");
    printf("for (int i = 0; i < argc; i++) {MEM_U32(arg_addr + i * 4) = arg_strpos; uint32_t p = 0; do { MEM_S8(arg_strpos) = argv[i][p]; ++arg_strpos; } while (argv[i][p++] != '\\0');}\n");
    printf("}\n");

    printf("setup_libc_data(mem);\n");
    
    if (TRACE) {
        printf("unsigned long long int cnt = 0;\n");
    }
    printf("Loffset:\n");
    printf("%s = 0x%x;\n", r(MIPS_REG_SP), stack_bottom);
    if (LABELS_64_BIT) {
        printf("%s = (uint32_t)((uintptr_t)&&end - (uintptr_t)&&Loffset);\n", r(MIPS_REG_RA));
    } else {
        printf("%s = (uint32_t)(uintptr_t)&&end;\n", r(MIPS_REG_RA));
    }
    printf("goto L%x;\n", symbol_names_inv.at("main"));
    if (!TRACE) {
        printf("end: return %s;\n", r(MIPS_REG_V0));
    } else {
        printf("end: fprintf(stderr, \"cnt: %%llu\\n\", cnt); return %s;\n", r(MIPS_REG_V0));
    }

    for (size_t i = 0; i < insns.size(); i++) {
        Insn& insn = insns[i];
        uint32_t vaddr = text_vaddr + i * 4;
        if (label_addresses.count(vaddr)) {
            printf("L%x:\n", vaddr);
        }
        dump_instr(i);
    }
}

static void parse_elf(const uint8_t *data, size_t file_len) {
    Elf32_Ehdr *ehdr;
    Elf32_Shdr *shdr, *str_shdr, *sym_shdr = NULL, *dynsym_shdr, *dynamic_shdr, *reginfo_shdr, *got_shdr, *sym_strtab, *sym_dynstr;
    int text_section_index = -1;
    int symtab_section_index = -1;
    int dynsym_section_index = -1;
    int reginfo_section_index = -1;
    int dynamic_section_index = -1;
    int got_section_index = -1;
    int rodata_section_index = -1;
    int data_section_index = -1;
    int bss_section_index = -1;
    uint32_t text_offset = 0;
    uint32_t vaddr_adj = 0;

    if (file_len < 4 || data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
        fprintf(stderr, "Not an ELF file.\n");
        exit(EXIT_FAILURE);
    }

    ehdr = (Elf32_Ehdr *) data;
    if (ehdr->e_ident[EI_DATA] != 2 || u16be(ehdr->e_machine) != 8) {
        fprintf(stderr, "Not big-endian MIPS.\n");
        exit(EXIT_FAILURE);
    }

    if (u16be(ehdr->e_shstrndx) == 0) {
        // (We could look at program headers instead in this case.)
        fprintf(stderr, "Missing section headers; stripped binaries are not yet supported.\n");
        exit(EXIT_FAILURE);
    }

#define SECTION(index) (Elf32_Shdr *)(data + u32be(ehdr->e_shoff) + (index) * u16be(ehdr->e_shentsize))
#define STR(strtab, offset) (const char *)(data + u32be(strtab->sh_offset) + offset)

    str_shdr = SECTION(u16be(ehdr->e_shstrndx));
    for (int i = 0; i < u16be(ehdr->e_shnum); i++) {
        shdr = SECTION(i);
        const char *name = STR(str_shdr, u32be(shdr->sh_name));
        if (strcmp(name, ".text") == 0) {
            text_offset = u32be(shdr->sh_offset);
            text_vaddr = u32be(shdr->sh_addr);
            vaddr_adj = text_vaddr - u32be(shdr->sh_addr);
            text_section_len = u32be(shdr->sh_size);
            text_section = data + text_offset;
            text_section_index = i;
        }
        if (u32be(shdr->sh_type) == SHT_SYMTAB) {
            symtab_section_index = i;
        }
        if (u32be(shdr->sh_type) == SHT_DYNSYM) {
            dynsym_section_index = i;
        }
        if (u32be(shdr->sh_type) == SHT_MIPS_REGINFO) {
            reginfo_section_index = i;
        }
        if (u32be(shdr->sh_type) == SHT_DYNAMIC) {
            dynamic_section_index = i;
        }
        if (strcmp(name, ".got") == 0) {
            got_section_index = i;
        }
        if (strcmp(name, ".rodata") == 0) {
            rodata_section_index = i;
        }
        if (strcmp(name, ".data") == 0) {
            data_section_index = i;
        }
        if (strcmp(name, ".bss") == 0) {
            bss_section_index = i;
        }
    }

    if (text_section_index == -1) {
        fprintf(stderr, "Missing .text section.\n");
        exit(EXIT_FAILURE);
    }

    if (symtab_section_index == -1 && dynsym_section_index == -1) {
        fprintf(stderr, "Missing .symtab or .dynsym section.\n");
        exit(EXIT_FAILURE);
    }

    if (dynsym_section_index != -1) {
        if (reginfo_section_index == -1) {
            fprintf(stderr, "Missing .reginfo section.\n");
            exit(EXIT_FAILURE);
        }
        if (dynamic_section_index == -1) {
            fprintf(stderr, "Missing .dynamic section.\n");
            exit(EXIT_FAILURE);
        }
        if (got_section_index == -1) {
            fprintf(stderr, "Missing .got section.\n");
            exit(EXIT_FAILURE);
        }
    }

    if (rodata_section_index != -1) {
        shdr = SECTION(rodata_section_index);
        uint32_t size = u32be(shdr->sh_size);
        rodata_section = data + u32be(shdr->sh_offset);
        rodata_section_len = size;
        rodata_vaddr = u32be(shdr->sh_addr);
    }

    if (data_section_index != -1) {
        shdr = SECTION(data_section_index);
        uint32_t size = u32be(shdr->sh_size);
        data_section = data + u32be(shdr->sh_offset);
        data_section_len = size;
        data_vaddr = u32be(shdr->sh_addr);
    }

    if (bss_section_index != -1) {
        shdr = SECTION(bss_section_index);
        uint32_t size = u32be(shdr->sh_size);
        bss_section_len = size;
        bss_vaddr = u32be(shdr->sh_addr);
    }


    // add symbols
    if (symtab_section_index != -1) {
        sym_shdr = SECTION(symtab_section_index);
        sym_strtab = SECTION(u32be(sym_shdr->sh_link));
        assert(0 && ".symtab not supported - use a program with .dynsym instead");

        assert(u32be(sym_shdr->sh_entsize) == sizeof(Elf32_Sym));
        for (uint32_t i = 0; i < u32be(sym_shdr->sh_size); i += sizeof(Elf32_Sym)) {
            Elf32_Sym *sym = (Elf32_Sym *)(data + u32be(sym_shdr->sh_offset) + i);
            const char *name = STR(sym_strtab, u32be(sym->st_name));
            uint32_t addr = u32be(sym->st_value);
            if (u16be(sym->st_shndx) != text_section_index || name[0] == '.') {
                continue;
            }
            addr += vaddr_adj;
            //disasm_label_add(state, name, addr, u32be(sym->st_size), true);
        }
    }

    if (dynsym_section_index != -1) {
        dynsym_shdr = SECTION(dynsym_section_index);
        sym_dynstr = SECTION(u32be(dynsym_shdr->sh_link));
        reginfo_shdr = SECTION(reginfo_section_index);
        dynamic_shdr = SECTION(dynamic_section_index);
        got_shdr = SECTION(got_section_index);

        Elf32_RegInfo *reg_info = (Elf32_RegInfo *)(data + u32be(reginfo_shdr->sh_offset));
        uint32_t gp_base = u32be(reg_info->ri_gp_value); // gp should have this value through the program run
        uint32_t got_start = 0;
        uint32_t local_got_no = 0;
        uint32_t first_got_sym = 0;
        uint32_t dynsym_no = 0; // section size can't be used due to alignment 16 padding

        assert(u32be(dynamic_shdr->sh_entsize) == sizeof(Elf32_Dyn));
        for (uint32_t i = 0; i < u32be(dynamic_shdr->sh_size); i += sizeof(Elf32_Dyn)) {
            Elf32_Dyn *dyn = (Elf32_Dyn *)(data + u32be(dynamic_shdr->sh_offset) + i);
            if (u32be(dyn->d_tag) == DT_PLTGOT) {
                got_start = u32be(dyn->d_un.d_ptr);
            }
            if (u32be(dyn->d_tag) == DT_MIPS_LOCAL_GOTNO) {
                local_got_no = u32be(dyn->d_un.d_val);
            }
            if (u32be(dyn->d_tag) == DT_MIPS_GOTSYM) {
                first_got_sym = u32be(dyn->d_un.d_val);
            }
            if (u32be(dyn->d_tag) == DT_MIPS_SYMTABNO) {
                dynsym_no = u32be(dyn->d_un.d_val);
            }
        }

        assert(got_start != 0);

        // value to add to asm gp offset, for example 32752, if -32752(gp) refers to the first entry in got.
        uint32_t gp_adj = gp_base - got_start;
        assert(gp_adj < 0x10000);

        assert(u32be(dynsym_shdr->sh_entsize) == sizeof(Elf32_Sym));
        uint32_t global_got_no = dynsym_no - first_got_sym;
        //global_got_entry *global_entries = (global_got_entry *)calloc(global_got_no, sizeof(global_got_entry));
        got_globals.resize(global_got_no);

        uint32_t common_start = ~0U;
        vector<string> common_order;

        for (uint32_t i = 0; i < dynsym_no; i++) {
            Elf32_Sym *sym = (Elf32_Sym *)(data + u32be(dynsym_shdr->sh_offset) + i * sizeof(Elf32_Sym));
            const char *name = STR(sym_dynstr, u32be(sym->st_name));
            uint32_t addr = u32be(sym->st_value);
            addr += vaddr_adj;
            uint8_t type = ELF32_ST_TYPE(sym->st_info);
            if ((u16be(sym->st_shndx) == SHN_MIPS_TEXT && type == STT_FUNC) ||
                 (type == STT_OBJECT && (u16be(sym->st_shndx) == SHN_MIPS_ACOMMON || u16be(sym->st_shndx) == SHN_MIPS_DATA)))
            {
                //disasm_label_add(state, name, addr, u32be(sym->st_size), true);
                if (type == STT_OBJECT) {
                    //disasm_add_data_addr(state, addr);
                }
                if (u16be(sym->st_shndx) == SHN_MIPS_ACOMMON) {
                    if (addr < common_start) {
                        common_start = addr;
                    }
                    common_order.push_back(name);
                }
            }
            if (i >= first_got_sym) {
                uint32_t got_value = u32be(*(uint32_t *)(data + u32be(got_shdr->sh_offset) + (local_got_no + (i - first_got_sym)) * sizeof(uint32_t)));
                if (u16be(sym->st_shndx) == SHN_MIPS_TEXT && type == STT_FUNC) {
                    //got_globals[i - first_got_sym] = got_value;
                    //label_addresses.insert(got_value);
                    got_globals[i - first_got_sym] = addr; // to include the 3 instr gp header thing
                    label_addresses.insert(addr);
                } else if (type == STT_OBJECT && (u16be(sym->st_shndx) == SHN_UNDEF || u16be(sym->st_shndx) == SHN_COMMON)) {
                    // symbol defined externally (for example in libc)
                    got_globals[i - first_got_sym] = got_value;
                } else {
                    got_globals[i - first_got_sym] = addr;
                }
                symbol_names[got_globals[i - first_got_sym]] = name;
            }
        }

        uint32_t *local_entries = (uint32_t *)calloc(local_got_no, sizeof(uint32_t));
        got_locals.resize(local_got_no);
        for (uint32_t i = 0; i < local_got_no; i++) {
            uint32_t *entry = (uint32_t *)(data + u32be(got_shdr->sh_offset) + i * sizeof(uint32_t));
            got_locals[i] = u32be(*entry);
        }

        gp_value = gp_base;
        gp_value_adj = gp_adj;
        //disasm_got_entries_set(state, gp_base, gp_adj, local_entries, local_got_no, global_entries, global_got_no);

        //out_range.common_start = common_start;
        //out_range.common_order.swap(common_order);
    }

    // add relocations
    for (int i = 0; i < u16be(ehdr->e_shnum); i++) {
        Elf32_Rel *prevHi = NULL;
        shdr = SECTION(i);
        if (u32be(shdr->sh_type) != SHT_REL || u32be(shdr->sh_info) != (uint32_t) text_section_index)
            continue;

        if (sym_shdr == NULL) {
            fprintf(stderr, "Relocations without .symtab section\n");
            exit(EXIT_FAILURE);
        }

        assert(u32be(shdr->sh_link) == (uint32_t) symtab_section_index);
        assert(u32be(shdr->sh_entsize) == sizeof(Elf32_Rel));
        for (uint32_t i = 0; i < u32be(shdr->sh_size); i += sizeof(Elf32_Rel)) {
            Elf32_Rel *rel = (Elf32_Rel *)(data + u32be(shdr->sh_offset) + i);
            uint32_t offset = text_offset + u32be(rel->r_offset);
            uint32_t symIndex = ELF32_R_SYM(u32be(rel->r_info));
            uint32_t rtype = ELF32_R_TYPE(u32be(rel->r_info));
            const char *symName = "0";
            if (symIndex != STN_UNDEF) {
                Elf32_Sym *sym = (Elf32_Sym *)(data + u32be(sym_shdr->sh_offset) + symIndex * sizeof(Elf32_Sym));
                symName = STR(sym_strtab, u32be(sym->st_name));
            }

            if (rtype == R_MIPS_HI16) {
                if (prevHi != NULL) {
                    fprintf(stderr, "Consecutive R_MIPS_HI16.\n");
                    exit(EXIT_FAILURE);
                }
                prevHi = rel;
                continue;
            }
            if (rtype == R_MIPS_LO16) {
                int32_t addend = (int16_t)((data[offset + 2] << 8) + data[offset + 3]);
                if (prevHi != NULL) {
                    uint32_t offset2 = text_offset + u32be(prevHi->r_offset);
                    addend += (uint32_t)((data[offset2 + 2] << 8) + data[offset2 + 3]) << 16;
                    //add_reloc(state, offset2, symName, addend, out_range.vaddr);
                }
                prevHi = NULL;
                //add_reloc(state, offset, symName, addend, out_range.vaddr);
            }
            else if (rtype == R_MIPS_26) {
                int32_t addend = (u32be(*(uint32_t*)(data + offset)) & ((1 << 26) - 1)) << 2;
                if (addend >= (1 << 27)) {
                    addend -= 1 << 28;
                }
                //add_reloc(state, offset, symName, addend, out_range.vaddr);
            }
            else {
                fprintf(stderr, "Bad relocation type %d.\n", rtype);
                exit(EXIT_FAILURE);
            }
        }
        if (prevHi != NULL) {
            fprintf(stderr, "R_MIPS_HI16 without matching R_MIPS_LO16.\n");
            exit(EXIT_FAILURE);
        }
    }
}
#undef SECTION
#undef STR

size_t read_file(const char *file_name, uint8_t **data) {
    FILE *in;
    uint8_t *in_buf = NULL;
    long file_size;
    long bytes_read;
    in = fopen(file_name, "rb");
    assert(in != nullptr);

    // allocate buffer to read from offset to end of file
    fseek(in, 0, SEEK_END);
    file_size = ftell(in);
    assert(file_size != -1L);

    in_buf = (uint8_t *)malloc(file_size);
    fseek(in, 0, SEEK_SET);

    // read bytes
    bytes_read = fread(in_buf, 1, file_size, in);
    assert(bytes_read == file_size);

    fclose(in);
    *data = in_buf;
    return bytes_read;
}

int main(int argc, char *argv[]) {
    uint8_t *data;
    size_t len = read_file(argv[1], &data);
    parse_elf(data, len);
    assert(cs_open(CS_ARCH_MIPS, (cs_mode)(CS_MODE_MIPS64 | CS_MODE_BIG_ENDIAN), &handle) == CS_ERR_OK);
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    disassemble();
    pass1();
    //dump();
    dump_c();
    free(data);
    cs_close(&handle);
}

