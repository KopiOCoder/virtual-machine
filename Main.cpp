/*
 * Virtual Machine Implementation
 * Architecture: 8-bit signed registers (R0–R7), byte-addressable memory
 * Supported Instructions: MOV, ADD, SUB, MUL, DIV, INC, DEC, IN, OUT, SHL, SHR, PUSH, POP
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <climits>

// ─────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────
static const int NUM_REGISTERS  = 8;
static const int MEMORY_SIZE    = 256;   // bytes
static const int MAX_PROGRAM    = 512;   // max instructions
static const int STACK_START    = 56;    // stack base address
static const int STACK_SIZE     = 8;     // stack size in bytes

// ─────────────────────────────────────────────
//  Register  (base)
// ─────────────────────────────────────────────
class Register {
protected:
    signed char value;

public:
    Register() : value(0) {}
    explicit Register(signed char v) : value(v) {}
    virtual ~Register() {}

    virtual void     setValue(signed char v) { value = v; }
    virtual signed char getValue() const     { return value; }

    // Overflow check: would adding a + b overflow signed 8-bit?
    static bool addOverflows(signed char a, signed char b) {
        int result = static_cast<int>(a) + static_cast<int>(b);
        return result > 127 || result < -128;
    }
    // Underflow check (subtraction)
    static bool subUnderflows(signed char a, signed char b) {
        int result = static_cast<int>(a) - static_cast<int>(b);
        return result < -128;
    }
    // Carry check (unsigned perspective)
    static bool addCarries(signed char a, signed char b) {
        unsigned int ua = static_cast<unsigned char>(a);
        unsigned int ub = static_cast<unsigned char>(b);
        return (ua + ub) > 255;
    }
};

// ─────────────────────────────────────────────
//  GeneralRegister  (R0–R7)
// ─────────────────────────────────────────────
class GeneralRegister : public Register {
private:
    int index;   // 0–7

public:
    GeneralRegister() : Register(), index(0) {}

    void setIndex(int i) { index = i; }
    int  getIndex() const { return index; }

    void setValue(signed char v) override { value = v; }
    signed char getValue() const  override { return value; }
};

// ─────────────────────────────────────────────
//  FlagRegister
//  CF – carry flag
//  OF – overflow flag
//  UF – underflow flag
//  ZF – zero flag
// ─────────────────────────────────────────────
class FlagRegister {
private:
    bool CF, OF, UF, ZF;

public:
    FlagRegister() : CF(false), OF(false), UF(false), ZF(false) {}

    void setCF(bool v) { CF = v; }
    void setOF(bool v) { OF = v; }
    void setUF(bool v) { UF = v; }
    void setZF(bool v) { ZF = v; }

    bool getCF() const { return CF; }
    bool getOF() const { return OF; }
    bool getUF() const { return UF; }
    bool getZF() const { return ZF; }

    void updateAfterAdd(signed char a, signed char b, int result) {
        ZF = (result == 0);
        CF = Register::addCarries(a, b);
        OF = (result > 127);
        UF = (result < -128);
    }

    void updateAfterSub(signed char a, signed char b, int result) {
        ZF = (result == 0);
        CF = false;              // borrow — optional interpretation
        OF = (result > 127);
        UF = (result < -128);
    }

    void updateGeneric(int result) {
        ZF = (result == 0);
        OF = (result > 127);
        UF = (result < -128);
        CF = false;
    }

    void print() const {
        std::cout << "Flags: CF=" << CF << " OF=" << OF
                  << " UF=" << UF << " ZF=" << ZF << "\n";
    }
};

// ─────────────────────────────────────────────
//  Memory  (256-byte flat byte store)
// ─────────────────────────────────────────────
class Memory {
private:
    unsigned char data[MEMORY_SIZE];

public:
    Memory() { memset(data, 0, sizeof(data)); }

    // Write one byte
    void writeByte(int address, unsigned char val) {
        if (address < 0 || address >= MEMORY_SIZE) {
            std::cerr << "Memory write out of bounds: " << address << "\n";
            return;
        }
        data[address] = val;
    }

    // Read one byte
    unsigned char readByte(int address) const {
        if (address < 0 || address >= MEMORY_SIZE) {
            std::cerr << "Memory read out of bounds: " << address << "\n";
            return 0;
        }
        return data[address];
    }

    void dump(int from, int to) const {
        for (int i = from; i <= to && i < MEMORY_SIZE; ++i)
            std::cout << "[" << i << "]=" << (int)data[i] << "  ";
        std::cout << "\n";
    }
};

// ─────────────────────────────────────────────
//  Operand descriptor  (used by Instruction)
// ─────────────────────────────────────────────
enum class OperandType {
    NONE,
    REGISTER,         // Rn
    IMMEDIATE,        // literal integer
    MEMORY_REG,       // [Rn]  – indirect via register
};

struct Operand {
    OperandType type;
    int         regIndex;   // valid when REGISTER or MEMORY_REG
    int         immediate;  // valid when IMMEDIATE

    Operand() : type(OperandType::NONE), regIndex(0), immediate(0) {}
};

// ─────────────────────────────────────────────
//  Forward-declare CPU so Instruction can hold a reference
// ─────────────────────────────────────────────
class CPU;

// ─────────────────────────────────────────────
//  Instruction  (abstract base)
// ─────────────────────────────────────────────
class Instruction {
protected:
    Operand dst;
    Operand src;

public:
    virtual ~Instruction() {}

    void setDst(const Operand& o) { dst = o; }
    void setSrc(const Operand& o) { src = o; }

    // Every concrete instruction implements execute()
    virtual void execute(CPU& cpu) = 0;
    virtual const char* name() const = 0;
};

// ─────────────────────────────────────────────
//  CPU
// ─────────────────────────────────────────────
class CPU {
public:
    GeneralRegister reg[NUM_REGISTERS];
    FlagRegister    flags;
    Memory          memory;
    int             PC;     // program counter (instruction index)
    int             SI;     // stack index (register)

    CPU() : PC(0), SI(0) {
        for (int i = 0; i < NUM_REGISTERS; ++i)
            reg[i].setIndex(i);
    }

    // ── helpers used by instructions ──────────

    signed char getRegVal(int idx) const {
        return reg[idx].getValue();
    }
    void setRegVal(int idx, signed char v) {
        reg[idx].setValue(v);
    }

    unsigned char getMemVal(int addr) const {
        return memory.readByte(addr);
    }
    void setMemVal(int addr, unsigned char v) {
        memory.writeByte(addr, v);
    }

    int getSI() const { return SI; }
    void setSI(int s) { SI = s; }

    void dumpRegisters() const {
        for (int i = 0; i < NUM_REGISTERS; ++i)
            std::cout << "R" << i << "=" << (int)reg[i].getValue() << "  ";
        std::cout << "\n";
    }

    void dumpFlags() const { flags.print(); }
};

// ─────────────────────────────────────────────
//  MOV  instruction
//  Supports:
//    MOV Rn, imm       – immediate
//    MOV Rn, Rm        – register-to-register
//    MOV Rn, [Rm]      – memory-indirect via Rm
// ─────────────────────────────────────────────
class MOVInstruction : public Instruction {
public:
    void execute(CPU& cpu) override {
        signed char srcVal = 0;

        // Resolve source
        if (src.type == OperandType::IMMEDIATE) {
            srcVal = static_cast<signed char>(src.immediate);
        } else if (src.type == OperandType::REGISTER) {
            srcVal = cpu.getRegVal(src.regIndex);
        } else if (src.type == OperandType::MEMORY_REG) {
            int addr = static_cast<unsigned char>(cpu.getRegVal(src.regIndex));
            srcVal   = static_cast<signed char>(cpu.getMemVal(addr));
        }

        // Destination must be a register
        if (dst.type == OperandType::REGISTER) {
            cpu.setRegVal(dst.regIndex, srcVal);
        } else {
            std::cerr << "MOV: invalid destination operand type\n";
        }
    }
    const char* name() const override { return "MOV"; }
};

// ─────────────────────────────────────────────
//  ArithmeticInstruction  (ADD, SUB, MUL, DIV)
// ─────────────────────────────────────────────
enum class ArithOp { ADD, SUB, MUL, DIV };

class ArithmeticInstruction : public Instruction {
private:
    ArithOp op;

public:
    explicit ArithmeticInstruction(ArithOp o) : op(o) {}

    void execute(CPU& cpu) override {
        if (dst.type != OperandType::REGISTER) {
            std::cerr << "Arithmetic: destination must be a register\n";
            return;
        }
        signed char dstVal = cpu.getRegVal(dst.regIndex);
        signed char srcVal = 0;

        if (src.type == OperandType::IMMEDIATE)
            srcVal = static_cast<signed char>(src.immediate);
        else if (src.type == OperandType::REGISTER)
            srcVal = cpu.getRegVal(src.regIndex);
        else if (src.type == OperandType::MEMORY_REG) {
            int addr = static_cast<unsigned char>(cpu.getRegVal(src.regIndex));
            srcVal   = static_cast<signed char>(cpu.getMemVal(addr));
        }

        int result = 0;
        switch (op) {
            case ArithOp::ADD:
                result = static_cast<int>(dstVal) + static_cast<int>(srcVal);
                cpu.flags.updateAfterAdd(dstVal, srcVal, result);
                break;
            case ArithOp::SUB:
                result = static_cast<int>(dstVal) - static_cast<int>(srcVal);
                cpu.flags.updateAfterSub(dstVal, srcVal, result);
                break;
            case ArithOp::MUL:
                result = static_cast<int>(dstVal) * static_cast<int>(srcVal);
                cpu.flags.updateGeneric(result);
                break;
            case ArithOp::DIV:
                if (srcVal == 0) {
                    std::cerr << "Division by zero!\n";
                    return;
                }
                result = static_cast<int>(dstVal) / static_cast<int>(srcVal);
                cpu.flags.updateGeneric(result);
                break;
        }
        // clamp to 8-bit signed
        if (result > 127)  result = 127;
        if (result < -128) result = -128;
        cpu.setRegVal(dst.regIndex, static_cast<signed char>(result));
    }

    const char* name() const override {
        switch (op) {
            case ArithOp::ADD: return "ADD";
            case ArithOp::SUB: return "SUB";
            case ArithOp::MUL: return "MUL";
            case ArithOp::DIV: return "DIV";
        }
        return "ARITH";
    }
};

// ─────────────────────────────────────────────
//  IOInstruction  (IN, OUT)
//  IN  Rn        – read signed integer from stdin → Rn
//  OUT Rn        – print Rn to stdout
// ─────────────────────────────────────────────
enum class IOOp { IN, OUT };

class IOInstruction : public Instruction {
private:
    IOOp op;

public:
    explicit IOInstruction(IOOp o) : op(o) {}

    void execute(CPU& cpu) override {
        if (op == IOOp::IN) {
            if (dst.type != OperandType::REGISTER) {
                std::cerr << "IN: destination must be a register\n";
                return;
            }
            int v;
            std::cout << "IN> ";
            std::cin >> v;
            cpu.setRegVal(dst.regIndex, static_cast<signed char>(v));
        } else {  // OUT
            // OUT can take register or immediate as the operand stored in dst
            signed char val = 0;
            if (dst.type == OperandType::REGISTER)
                val = cpu.getRegVal(dst.regIndex);
            else if (dst.type == OperandType::IMMEDIATE)
                val = static_cast<signed char>(dst.immediate);
            std::cout << "OUT: " << (int)val << "\n";
        }
    }
    const char* name() const override { return (op == IOOp::IN) ? "IN" : "OUT"; }
};

// ─────────────────────────────────────────────
//  ShiftInstruction  (SHL, SHR)
//  SHL Rn, imm   – shift Rn left  by imm bits
//  SHR Rn, imm   – shift Rn right by imm bits (logical)
// ─────────────────────────────────────────────
enum class ShiftOp { SHL, SHR };

class ShiftInstruction : public Instruction {
private:
    ShiftOp op;

public:
    explicit ShiftInstruction(ShiftOp o) : op(o) {}

    void execute(CPU& cpu) override {
        if (dst.type != OperandType::REGISTER) {
            std::cerr << "SHIFT: destination must be a register\n";
            return;
        }
        int amount = 0;
        if (src.type == OperandType::IMMEDIATE)
            amount = src.immediate;
        else if (src.type == OperandType::REGISTER)
            amount = static_cast<int>(cpu.getRegVal(src.regIndex));

        unsigned char uval = static_cast<unsigned char>(cpu.getRegVal(dst.regIndex));
        if (op == ShiftOp::SHL)
            uval = static_cast<unsigned char>(uval << amount);
        else
            uval = static_cast<unsigned char>(uval >> amount);

        cpu.setRegVal(dst.regIndex, static_cast<signed char>(uval));
        cpu.flags.updateGeneric(static_cast<signed char>(uval));
    }
    const char* name() const override { return (op == ShiftOp::SHL) ? "SHL" : "SHR"; }
};

// ─────────────────────────────────────────────
//  IncDecInstruction  (INC, DEC) - CHONG SENG KIAT
// ─────────────────────────────────────────────
enum class IncDecOp { INC, DEC };

class IncDecInstruction : public Instruction {
private:
    IncDecOp op;

public:
    explicit IncDecInstruction(IncDecOp o) : op(o) {}

    void execute(CPU& cpu) override {
        if (dst.type != OperandType::REGISTER) {
            std::cerr << "INCDEC: destination must be a register\n";
            return;
        }
        signed char val = cpu.getRegVal(dst.regIndex);
        int result = static_cast<int>(val);

        if (op == IncDecOp::INC)
            result += 1;
        else
            result -= 1;

        // clamp to 8-bit signed
        if (result > 127)  result = 127;
        if (result < -128) result = -128;

        cpu.flags.updateGeneric(result);
        cpu.setRegVal(dst.regIndex, static_cast<signed char>(result));
    }

    const char* name() const override { return (op == IncDecOp::INC) ? "INC" : "DEC"; }
};

// ─────────────────────────────────────────────
//  PushInstruction  (PUSH) - CHONG SENG KIAT
// ─────────────────────────────────────────────
class PushInstruction : public Instruction {
public:
    void execute(CPU& cpu) override {
        if (dst.type != OperandType::REGISTER) {
            std::cerr << "PUSH: operand must be a register\n";
            return;
        }
        if (cpu.getSI() >= STACK_SIZE) {
            std::cerr << "Stack overflow!\n";
            return;
        }
        signed char val = cpu.getRegVal(dst.regIndex);
        int addr = STACK_START + cpu.getSI();
        cpu.setMemVal(addr, static_cast<unsigned char>(val));
        cpu.setSI(cpu.getSI() + 1);
    }

    const char* name() const override { return "PUSH"; }
};

// ─────────────────────────────────────────────
//  PopInstruction  (POP) - CHONG SENG KIAT
// ─────────────────────────────────────────────
class PopInstruction : public Instruction {
public:
    void execute(CPU& cpu) override {
        if (dst.type != OperandType::REGISTER) {
            std::cerr << "POP: operand must be a register\n";
            return;
        }
        if (cpu.getSI() <= 0) {
            std::cerr << "Stack underflow! Cannot pop from empty stack.\n";
            return;
        }
        cpu.setSI(cpu.getSI() - 1);
        int addr = STACK_START + cpu.getSI();
        unsigned char val = cpu.getMemVal(addr);
        cpu.setRegVal(dst.regIndex, static_cast<signed char>(val));
    }

    const char* name() const override { return "POP"; }
};

// Simple string helpers (no STL string functions used beyond basic ops)
static void strToUpper(char* s) {
    for (int i = 0; s[i]; ++i)
        s[i] = static_cast<char>(toupper(static_cast<unsigned char>(s[i])));
}

static void trim(char* s) {
    // left trim
    int start = 0;
    while (s[start] == ' ' || s[start] == '\t') ++start;
    int len = static_cast<int>(strlen(s + start));
    memmove(s, s + start, len + 1);
    // right trim
    int end = len - 1;
    while (end >= 0 && (s[end] == ' ' || s[end] == '\t' || s[end] == '\r' || s[end] == '\n'))
        s[end--] = '\0';
}

class Runner {
private:
    CPU          cpu;
    Instruction* program[MAX_PROGRAM];
    int          programSize;

    // ── operand parser ──────────────────────
    // Parses a token like "R3", "10", "[R2]", "-5"
    Operand parseOperand(const char* token) {
        Operand op;
        if (!token || token[0] == '\0') {
            op.type = OperandType::NONE;
            return op;
        }

        // Memory-indirect  [Rn]
        if (token[0] == '[') {
            op.type = OperandType::MEMORY_REG;
            // find the 'R' inside
            const char* p = token + 1;
            while (*p && *p != 'R' && *p != 'r') ++p;
            if (*p == 'R' || *p == 'r') {
                op.regIndex = atoi(p + 1);
            }
            return op;
        }

        // Register  Rn
        if (token[0] == 'R' || token[0] == 'r') {
            op.type     = OperandType::REGISTER;
            op.regIndex = atoi(token + 1);
            return op;
        }

        // Immediate (signed)
        op.type      = OperandType::IMMEDIATE;
        op.immediate = atoi(token);
        return op;
    }

    // ── line decoder ────────────────────────
    // Returns heap-allocated Instruction* or nullptr on blank/comment
    Instruction* decodeLine(char* line) {
        trim(line);
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#')
            return nullptr;

        // Tokenise by spaces/commas
        char tokens[4][64];
        int  nTokens = 0;
        char tmp[256];
        strncpy(tmp, line, 255);
        tmp[255] = '\0';

        char* tok = strtok(tmp, " ,\t");
        while (tok && nTokens < 4) {
            strncpy(tokens[nTokens], tok, 63);
            tokens[nTokens][63] = '\0';
            strToUpper(tokens[nTokens]);
            ++nTokens;
            tok = strtok(nullptr, " ,\t");
        }

        if (nTokens == 0) return nullptr;

        const char* mnemonic = tokens[0];

        // ── MOV ──────────────────────────────
        if (strcmp(mnemonic, "MOV") == 0) {
            if (nTokens < 3) {
                std::cerr << "MOV requires 2 operands\n";
                return nullptr;
            }
            MOVInstruction* instr = new MOVInstruction();
            instr->setDst(parseOperand(tokens[1]));
            // Re-parse src from original line to preserve '[' characters
            // (strtok may have split "[R1]" — re-read from raw line)
            // Find second operand in original line after the comma
            const char* comma = strchr(line, ',');
            Operand srcOp;
            if (comma) {
                char srcBuf[64];
                strncpy(srcBuf, comma + 1, 63);
                srcBuf[63] = '\0';
                // trim
                char* sb = srcBuf;
                while (*sb == ' ' || *sb == '\t') ++sb;
                // upper-case
                for (int i = 0; sb[i]; ++i)
                    sb[i] = static_cast<char>(toupper(static_cast<unsigned char>(sb[i])));
                srcOp = parseOperand(sb);
            } else {
                srcOp = parseOperand(tokens[2]);
            }
            instr->setSrc(srcOp);
            return instr;
        }

        // ── ADD ──────────────────────────────
        if (strcmp(mnemonic, "ADD") == 0) {
            ArithmeticInstruction* instr = new ArithmeticInstruction(ArithOp::ADD);
            instr->setDst(parseOperand(tokens[1]));
            instr->setSrc(parseOperand(nTokens > 2 ? tokens[2] : ""));
            return instr;
        }

        // ── SUB ──────────────────────────────
        if (strcmp(mnemonic, "SUB") == 0) {
            ArithmeticInstruction* instr = new ArithmeticInstruction(ArithOp::SUB);
            instr->setDst(parseOperand(tokens[1]));
            instr->setSrc(parseOperand(nTokens > 2 ? tokens[2] : ""));
            return instr;
        }

        // ── MUL ──────────────────────────────
        if (strcmp(mnemonic, "MUL") == 0) {
            ArithmeticInstruction* instr = new ArithmeticInstruction(ArithOp::MUL);
            instr->setDst(parseOperand(tokens[1]));
            instr->setSrc(parseOperand(nTokens > 2 ? tokens[2] : ""));
            return instr;
        }

        // ── DIV ──────────────────────────────
        if (strcmp(mnemonic, "DIV") == 0) {
            ArithmeticInstruction* instr = new ArithmeticInstruction(ArithOp::DIV);
            instr->setDst(parseOperand(tokens[1]));
            instr->setSrc(parseOperand(nTokens > 2 ? tokens[2] : ""));
            return instr;
        }

        // ── IN ───────────────────────────────
        if (strcmp(mnemonic, "IN") == 0) {
            IOInstruction* instr = new IOInstruction(IOOp::IN);
            instr->setDst(parseOperand(nTokens > 1 ? tokens[1] : ""));
            return instr;
        }

        // ── OUT ──────────────────────────────
        if (strcmp(mnemonic, "OUT") == 0) {
            IOInstruction* instr = new IOInstruction(IOOp::OUT);
            instr->setDst(parseOperand(nTokens > 1 ? tokens[1] : ""));
            return instr;
        }

        // ── SHL ──────────────────────────────
        if (strcmp(mnemonic, "SHL") == 0) {
            ShiftInstruction* instr = new ShiftInstruction(ShiftOp::SHL);
            instr->setDst(parseOperand(tokens[1]));
            instr->setSrc(parseOperand(nTokens > 2 ? tokens[2] : ""));
            return instr;
        }

        // ── SHR ──────────────────────────────
        if (strcmp(mnemonic, "SHR") == 0) {
            ShiftInstruction* instr = new ShiftInstruction(ShiftOp::SHR);
            instr->setDst(parseOperand(tokens[1]));
            instr->setSrc(parseOperand(nTokens > 2 ? tokens[2] : ""));
            return instr;
        }

        // ── INC ──────────────────────────────
        if (strcmp(mnemonic, "INC") == 0) {
            IncDecInstruction* instr = new IncDecInstruction(IncDecOp::INC);
            instr->setDst(parseOperand(tokens[1]));
            return instr;
        }

        // ── DEC ──────────────────────────────
        if (strcmp(mnemonic, "DEC") == 0) {
            IncDecInstruction* instr = new IncDecInstruction(IncDecOp::DEC);
            instr->setDst(parseOperand(tokens[1]));
            return instr;
        }

        // ── PUSH ─────────────────────────────
        if (strcmp(mnemonic, "PUSH") == 0) {
            PushInstruction* instr = new PushInstruction();
            instr->setDst(parseOperand(tokens[1]));
            return instr;
        }

        // ── POP ──────────────────────────────
        if (strcmp(mnemonic, "POP") == 0) {
            PopInstruction* instr = new PopInstruction();
            instr->setDst(parseOperand(tokens[1]));
            return instr;
        }

        std::cerr << "Unknown mnemonic: " << mnemonic << "\n";
        return nullptr;
    }

public:
    Runner() : programSize(0) {
        for (int i = 0; i < MAX_PROGRAM; ++i)
            program[i] = nullptr;
    }

    ~Runner() {
        for (int i = 0; i < programSize; ++i)
            delete program[i];
    }

    // Load from file
    bool loadFile(const char* filename) {
        std::ifstream f(filename);
        if (!f.is_open()) {
            std::cerr << "Cannot open file: " << filename << "\n";
            return false;
        }
        char line[256];
        while (f.getline(line, 256)) {
            if (programSize >= MAX_PROGRAM) {
                std::cerr << "Program too large\n";
                break;
            }
            Instruction* instr = decodeLine(line);
            if (instr)
                program[programSize++] = instr;
        }
        f.close();
        return true;
    }

    // Load from in-memory string (for demo)
    bool loadString(const char* src) {
        // Copy to mutable buffer
        int len = static_cast<int>(strlen(src));
        char* buf = new char[len + 2];
        memcpy(buf, src, len);
        buf[len]   = '\n';
        buf[len+1] = '\0';

        char line[256];
        int  li = 0;
        for (int i = 0; i <= len; ++i) {
            if (buf[i] == '\n' || buf[i] == '\0') {
                line[li] = '\0';
                if (programSize < MAX_PROGRAM) {
                    Instruction* instr = decodeLine(line);
                    if (instr)
                        program[programSize++] = instr;
                }
                li = 0;
            } else {
                if (li < 255) line[li++] = buf[i];
            }
        }
        delete[] buf;
        return true;
    }

    // Execute all instructions sequentially
    void run(bool verbose = false) {
        cpu.PC = 0;
        while (cpu.PC < programSize) {
            if (verbose) {
                std::cout << "[PC=" << cpu.PC << "] "
                          << program[cpu.PC]->name() << "\n";
            }
            program[cpu.PC]->execute(cpu);
            ++cpu.PC;
        }
    }

    void dumpRegisters() const { cpu.dumpRegisters(); }
    void dumpFlags()     const { cpu.dumpFlags(); }
    void dumpMemory(int from = 0, int to = 15) const {
        cpu.memory.dump(from, to);
    }

    // Pre-load a memory byte (useful for [Rn] tests)
    void preloadMemory(int addr, unsigned char val) {
        cpu.memory.writeByte(addr, val);
    }
};

// ─────────────────────────────────────────────
//  main  –  demo / test harness
// ─────────────────────────────────────────────
int main(int argc, char* argv[]) {

    // ── If a filename is given, run from file ──
    if (argc >= 2) {
        Runner runner;
        if (!runner.loadFile(argv[1])) return 1;
        runner.run(true);
        std::cout << "\n=== Final State ===\n";
        runner.dumpRegisters();
        runner.dumpFlags();
        return 0;
    }

    // ── Built-in demo ──────────────────────────
    std::cout << "==============================\n";
    std::cout << "  Virtual Machine – Demo\n";
    std::cout << "==============================\n\n";

    // ── Test 1: MOV immediate ──────────────────
    {
        std::cout << "-- Test 1: MOV Rn, immediate --\n";
        Runner r;
        r.loadString(
            "MOV R0, 10\n"
            "MOV R1, -5\n"
            "MOV R2, 100\n"
        );
        r.run();
        r.dumpRegisters();
        std::cout << "\n";
    }

    // ── Test 2: MOV register-to-register ───────
    {
        std::cout << "-- Test 2: MOV Rn, Rm --\n";
        Runner r;
        r.loadString(
            "MOV R0, 42\n"
            "MOV R1, R0\n"
            "MOV R2, R1\n"
        );
        r.run();
        r.dumpRegisters();
        std::cout << "\n";
    }

    // ── Test 3: MOV memory-indirect  [Rm] ──────
    {
        std::cout << "-- Test 3: MOV Rn, [Rm] --\n";
        Runner r;
        // Pre-load address 20 with value 77
        r.preloadMemory(20, 77);
        r.loadString(
            "MOV R1, 20\n"   // R1 = address 20
            "MOV R3, [R1]\n" // R3 = memory[20] = 77
        );
        r.run();
        r.dumpRegisters();
        std::cout << "\n";
    }

    // ── Test 4: Arithmetic ─────────────────────
    {
        std::cout << "-- Test 4: ADD / SUB / MUL / DIV --\n";
        Runner r;
        r.loadString(
            "MOV R0, 20\n"
            "MOV R1, 7\n"
            "ADD R0, R1\n"    // R0 = 27
            "SUB R1, 3\n"     // R1 = 4
            "MUL R0, R1\n"    // R0 = 108 (clamped to 107 due to signed 8-bit overflow guard? 108 fits)
            "DIV R0, 4\n"     // R0 = 27
            "OUT R0\n"
        );
        r.run();
        r.dumpRegisters();
        r.dumpFlags();
        std::cout << "\n";
    }

    // ── Test 5: Shift ──────────────────────────
    {
        std::cout << "-- Test 5: SHL / SHR --\n";
        Runner r;
        r.loadString(
            "MOV R0, 4\n"
            "SHL R0, 2\n"    // R0 = 16
            "OUT R0\n"
            "SHR R0, 1\n"    // R0 = 8
            "OUT R0\n"
        );
        r.run();
        r.dumpRegisters();
        std::cout << "\n";
    }

    // ── Test 6: Overflow flag ──────────────────
    {
        std::cout << "-- Test 6: Overflow flag --\n";
        Runner r;
        r.loadString(
            "MOV R0, 120\n"
            "ADD R0, 20\n"   // 140 > 127  → OF set, clamped to 127
            "OUT R0\n"
        );
        r.run();
        r.dumpRegisters();
        r.dumpFlags();
        std::cout << "\n";
    }

    // ── Test 7: INC / DEC ──────────────────────
    {
        std::cout << "-- Test 7: INC / DEC --\n";
        Runner r;
        r.loadString(
            "MOV R0, 5\n"
            "INC R0\n"       // R0 = 6
            "OUT R0\n"
            "DEC R0\n"       // R0 = 5
            "OUT R0\n"
            "DEC R0\n"       // R0 = 4
            "OUT R0\n"
        );
        r.run();
        r.dumpRegisters();
        r.dumpFlags();
        std::cout << "\n";
    }

    // ── Test 8: INC overflow ───────────────────
    {
        std::cout << "-- Test 8: INC overflow --\n";
        Runner r;
        r.loadString(
            "MOV R0, 127\n"
            "INC R0\n"       // 128 clamped to 127, OF set
            "OUT R0\n"
        );
        r.run();
        r.dumpRegisters();
        r.dumpFlags();
        std::cout << "\n";
    }

    // ── Test 9: DEC underflow ──────────────────
    {
        std::cout << "-- Test 9: DEC underflow --\n";
        Runner r;
        r.loadString(
            "MOV R0, -128\n"
            "DEC R0\n"       // -129 clamped to -128, UF set
            "OUT R0\n"
        );
        r.run();
        r.dumpRegisters();
        r.dumpFlags();
        std::cout << "\n";
    }

    // ── Test 10: PUSH single value ──────────────
    {
        std::cout << "-- Test 10: PUSH single value --\n";
        Runner r;
        r.loadString(
            "MOV R0, 42\n"
            "PUSH R0\n"
        );
        r.run();
        std::cout << "Stack after PUSH: ";
        r.dumpMemory(56, 63);
        std::cout << "\n";
    }

    // ── Test 11: PUSH and POP ──────────────────
    {
        std::cout << "-- Test 11: PUSH and POP --\n";
        Runner r;
        r.loadString(
            "MOV R0, 42\n"
            "PUSH R0\n"
            "MOV R1, 10\n"
            "POP R1\n"       // R1 should now be 42
            "OUT R1\n"
        );
        r.run();
        r.dumpRegisters();
        std::cout << "\n";
    }

    // ── Test 12: Multiple PUSH and POP ─────────
    {
        std::cout << "-- Test 12: Multiple PUSH and POP --\n";
        Runner r;
        r.loadString(
            "MOV R0, 10\n"
            "MOV R1, 20\n"
            "MOV R2, 30\n"
            "PUSH R0\n"
            "PUSH R1\n"
            "PUSH R2\n"
            "POP R3\n"       // R3 = 30
            "OUT R3\n"
            "POP R4\n"       // R4 = 20
            "OUT R4\n"
            "POP R5\n"       // R5 = 10
            "OUT R5\n"
        );
        r.run();
        r.dumpRegisters();
        std::cout << "\n";
    }

    // ── Test 13: Stack with INC/DEC ────────────
    {
        std::cout << "-- Test 13: Stack with INC/DEC --\n";
        Runner r;
        r.loadString(
            "MOV R0, 5\n"
            "INC R0\n"       // R0 = 6
            "PUSH R0\n"
            "MOV R0, 100\n"
            "POP R0\n"       // R0 = 6 (from stack)
            "OUT R0\n"
        );
        r.run();
        r.dumpRegisters();
        std::cout << "\n";
    }
}
