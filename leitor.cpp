#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>   // Para uint32_t, uint16_t, uint8_t
#include <stdexcept> // Para std::runtime_error
#include <string>
#include <cstring>   // Para std::memcpy
#include <iomanip>   // Para std::setw, std::left, std::right, std::hex, std::dec

/*
 * =======================================================================
 * FUNÇÕES AUXILIARES BIG-ENDIAN
 * =======================================================================
 */
uint32_t swap_uint32(uint32_t val) {
    return ((val << 24) & 0xFF000000) | ((val <<  8) & 0x00FF0000) |
           ((val >>  8) & 0x0000FF00) | ((val >> 24) & 0x000000FF);
}
uint16_t swap_uint16(uint16_t val) {
    return ((val << 8) & 0xFF00) | ((val >> 8) & 0x00FF);
}

/*
 * =======================================================================
 * FUNÇÕES DE LEITURA DA JVM
 * =======================================================================
 */
uint32_t read_u4(std::ifstream& file) {
    uint32_t value;
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!file.good()) throw std::runtime_error("Erro ao ler u4.");
    return swap_uint32(value);
}
uint16_t read_u2(std::ifstream& file) {
    uint16_t value;
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!file.good()) throw std::runtime_error("Erro ao ler u2.");
    return swap_uint16(value);
}
// Função para ler s4 (4 bytes com sinal)
int32_t read_s4(std::ifstream& file) {
    int32_t value;
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!file.good()) throw std::runtime_error("Erro ao ler s4.");
    // Precisa converter para uint32_t antes de swapear os bytes
    return (int32_t)swap_uint32((uint32_t)value);
}
uint8_t read_u1(std::ifstream& file) {
    uint8_t value;
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!file.good()) throw std::runtime_error("Erro ao ler u1.");
    return value;
}

/*
 * =======================================================================
 * ESTRUTURAS DE DADOS DO CONSTANT POOL
 * =======================================================================
 */

const uint8_t CONSTANT_Utf8 = 1;
const uint8_t CONSTANT_Integer = 3;
const uint8_t CONSTANT_Float = 4;
const uint8_t CONSTANT_Long = 5;
const uint8_t CONSTANT_Double = 6;
const uint8_t CONSTANT_Class = 7;
const uint8_t CONSTANT_String = 8;
const uint8_t CONSTANT_Fieldref = 9;
const uint8_t CONSTANT_Methodref = 10;
const uint8_t CONSTANT_InterfaceMethodref = 11;
const uint8_t CONSTANT_NameAndType = 12;

struct ConstantInfo {
    uint8_t tag;
    uint16_t index1;
    uint16_t index2;
    std::string utf8_string;
    uint32_t bytes4;
    uint32_t high_bytes;
    uint32_t low_bytes;
    ConstantInfo() : tag(0), index1(0), index2(0), bytes4(0), high_bytes(0), low_bytes(0) {}
};

using ConstantPool = std::vector<ConstantInfo>;


/*
 * =======================================================================
 * LEITURA E EXIBIÇÃO DO CONSTANT POOL
 * =======================================================================
 */

// Declarações antecipadas (forward declarations)
std::string get_utf8(const ConstantPool& pool, uint16_t index);
std::string get_class_name(const ConstantPool& pool, uint16_t class_index);

void ler_constant_pool(std::ifstream& file, uint16_t count, ConstantPool& pool) {
    pool.resize(count);
    for (int i = 1; i < count; i++) {
        uint8_t tag = read_u1(file);
        pool[i].tag = tag;
        switch (tag) {
            case CONSTANT_Utf8: {
                uint16_t length = read_u2(file);
                std::vector<char> utf8_bytes(length);
                file.read(utf8_bytes.data(), length);
                pool[i].utf8_string = std::string(utf8_bytes.begin(), utf8_bytes.end());
                break;
            }
            case CONSTANT_Class: pool[i].index1 = read_u2(file); break;
            case CONSTANT_String: pool[i].index1 = read_u2(file); break;
            case CONSTANT_Fieldref:
            case CONSTANT_Methodref:
            case CONSTANT_InterfaceMethodref:
            case CONSTANT_NameAndType:
                pool[i].index1 = read_u2(file);
                pool[i].index2 = read_u2(file);
                break;
            case CONSTANT_Integer:
            case CONSTANT_Float:
                pool[i].bytes4 = read_u4(file);
                break;
            case CONSTANT_Long:
            case CONSTANT_Double:
                pool[i].high_bytes = read_u4(file);
                pool[i].low_bytes = read_u4(file);
                i++;
                break;
            default:
                throw std::runtime_error("Tag do Constant Pool desconhecida: " + std::to_string(tag));
        }
    }
}

std::string get_utf8(const ConstantPool& pool, uint16_t index) {
    try {
        if (index == 0 || index >= pool.size()) return "[Indice invalido]";
        return pool.at(index).utf8_string;
    } catch (const std::out_of_range& e) {
        return "[Erro OOR Utf8]";
    }
}

std::string get_class_name(const ConstantPool& pool, uint16_t class_index) {
     if (class_index == 0 || class_index >= pool.size() || pool[class_index].tag != CONSTANT_Class) {
        return "[Indice Class invalido]";
    }
    return get_utf8(pool, pool[class_index].index1);
}

// Helper para exibir o que um índice do CP significa
std::string resolver_indice_cp(const ConstantPool& pool, uint16_t index) {
    try {
        const ConstantInfo& c = pool.at(index);
        std::string s = "";
        switch (c.tag) {
            case CONSTANT_Fieldref:
            case CONSTANT_Methodref:
            case CONSTANT_InterfaceMethodref: {
                if(c.tag == CONSTANT_Fieldref) s += "Field ";
                else if(c.tag == CONSTANT_Methodref) s += "Method ";
                else s += "InterfaceMethod ";
                
                std::string class_name = get_class_name(pool, c.index1);
                const ConstantInfo& nat = pool.at(c.index2);
                std::string name = get_utf8(pool, nat.index1);
                std::string desc = get_utf8(pool, nat.index2);
                s += class_name + ".\"" + name + "\":" + desc;
                break;
            }
            case CONSTANT_Class:
                s += "Class " + get_class_name(pool, index);
                break;
            case CONSTANT_String:
                s += "String \"" + get_utf8(pool, c.index1) + "\"";
                break;
            case CONSTANT_Integer:
                s += "Int " + std::to_string((int32_t)c.bytes4);
                break;
             case CONSTANT_Float: {
                float f_val;
                //É necessário garantir que bytes4 realmente contenha os bits float
                std::memcpy(&f_val, &c.bytes4, sizeof(float));
                 s += "Float " + std::to_string(f_val) + "f";
                 break;
             }
            case CONSTANT_Long: {
                int64_t l_val = ((int64_t)c.high_bytes << 32) | c.low_bytes;
                s += "Long " + std::to_string(l_val) + "l";
                break;
            }
            case CONSTANT_Double: {
                uint64_t bits = ((uint64_t)c.high_bytes << 32) | c.low_bytes;
                double d_val;
                std::memcpy(&d_val, &bits, sizeof(double));
                s += "Double " + std::to_string(d_val) + "d";
                 break;
            }
            default:
                s += "[Tipo " + std::to_string(c.tag) + " nao resolvido]";
                break;
        }
        return s;
    } catch (const std::exception& e) {
        return "[Erro ao resolver indice]";
    }
}


void exibir_constant_pool(const ConstantPool& pool) {
    std::cout << "\nConstant pool:" << std::endl;
    for (int i = 1; i < pool.size(); i++) {
        std::cout << std::setw(4) << std::right << "#" << i << " = ";
        const ConstantInfo& c = pool[i];
        switch (c.tag) {
            case CONSTANT_Utf8: std::cout << std::setw(20) << std::left << "Utf8" << "\"" << c.utf8_string << "\"" << std::endl; break;
            case CONSTANT_Class: std::cout << std::setw(20) << std::left << "Class" << "#" << c.index1 << " \t\t// " << get_utf8(pool, c.index1) << std::endl; break;
            case CONSTANT_String: std::cout << std::setw(20) << std::left << "String" << "#" << c.index1 << " \t\t// " << get_utf8(pool, c.index1) << std::endl; break;
            case CONSTANT_Fieldref:
            case CONSTANT_Methodref:
            case CONSTANT_InterfaceMethodref: {
                if (c.tag == CONSTANT_Fieldref) std::cout << std::setw(20) << std::left << "Fieldref";
                else if (c.tag == CONSTANT_Methodref) std::cout << std::setw(20) << std::left << "Methodref";
                else std::cout << std::setw(20) << std::left << "InterfaceMethodref";
                std::cout << "#" << c.index1 << ".#" << c.index2;
                std::cout << " \t// " << resolver_indice_cp(pool, i) << std::endl;
                break;
            }
            case CONSTANT_NameAndType: std::cout << std::setw(20) << std::left << "NameAndType" << "#" << c.index1 << ".#" << c.index2 << " \t// \"" << get_utf8(pool, c.index1) << "\":" << get_utf8(pool, c.index2) << std::endl; break;
            case CONSTANT_Integer: std::cout << std::setw(20) << std::left << "Integer" << (int32_t)c.bytes4 << std::endl; break;
            case CONSTANT_Float: { float f_val; std::memcpy(&f_val, &c.bytes4, sizeof(float)); std::cout << std::setw(20) << std::left << "Float" << f_val << "f" << std::endl; break; }
            case CONSTANT_Long: { int64_t l_val = ((int64_t)c.high_bytes << 32) | c.low_bytes; std::cout << std::setw(20) << std::left << "Long" << l_val << "l" << std::endl; i++; break; }
            case CONSTANT_Double: { uint64_t bits = ((uint64_t)c.high_bytes << 32) | c.low_bytes; double d_val; std::memcpy(&d_val, &bits, sizeof(double)); std::cout << std::setw(20) << std::left << "Double" << d_val << "d" << std::endl; i++; break; }
            case 0: std::cout << "(Slot vazio)" << std::endl; break;
            default: std::cout << "Tag nao implementada: " << (int)c.tag << std::endl; break;
        }
    }
}

/*
 * =======================================================================
 * LEITURA DE INFORMAÇÕES DA CLASSE
 * =======================================================================
 */
void ler_class_info(std::ifstream& file, const ConstantPool& pool) {
    std::cout << "\n--- Informacoes da Classe ---" << std::endl;
    uint16_t access_flags = read_u2(file);
    std::cout << "flags: 0x" << std::hex << access_flags << std::dec << std::endl;
    uint16_t this_class_idx = read_u2(file);
    std::cout << "this_class: #" << this_class_idx << " \t\t// " << get_class_name(pool, this_class_idx) << std::endl;
    uint16_t super_class_idx = read_u2(file);
    if (super_class_idx > 0) std::cout << "super_class: #" << super_class_idx << " \t// " << get_class_name(pool, super_class_idx) << std::endl;
    else std::cout << "super_class: #" << super_class_idx << std::endl;
    uint16_t interfaces_count = read_u2(file);
    std::cout << "interfaces_count: " << interfaces_count << std::endl;
    for (int i = 0; i < interfaces_count; i++) {
        uint16_t interface_idx = read_u2(file);
        std::cout << "\tInterface #" << interface_idx << " \t// " << get_class_name(pool, interface_idx) << std::endl;
    }
}

/*
 * =======================================================================
 * FUNÇÃO GENÉRICA PARA PULAR ATRIBUTOS
 * =======================================================================
 */
void pular_lista_atributos(std::ifstream& file, uint16_t attributes_count, const ConstantPool& pool, const std::string& indent) {
    for (int i = 0; i < attributes_count; i++) {
        uint16_t attribute_name_index = read_u2(file);
        uint32_t attribute_length = read_u4(file);
        std::cout << indent << "Atributo #" << attribute_name_index << " (" << get_utf8(pool, attribute_name_index) << ")";
        std::cout << ", Comprimento: " << attribute_length << ". Pulando..." << std::endl;
        file.seekg(attribute_length, std::ios::cur);
        if (!file.good()) throw std::runtime_error("Erro ao pular atributo.");
    }
}


/*
 * =======================================================================
 * LEITURA DOS FIELDS
 * =======================================================================
 */
void ler_fields(std::ifstream& file, const ConstantPool& pool) {
    uint16_t fields_count = read_u2(file);
    std::cout << "\n--- Fields (Contagem: " << fields_count << ") ---" << std::endl;

    for (int i = 0; i < fields_count; i++) {
        uint16_t access_flags = read_u2(file);
        uint16_t name_index = read_u2(file);
        uint16_t descriptor_index = read_u2(file);
        
        std::cout << "Field #" << i << ":" << std::endl;
        std::cout << "\tNome: #" << name_index << " \t\t// " << get_utf8(pool, name_index) << std::endl;
        std::cout << "\tDescritor: #" << descriptor_index << " \t// " << get_utf8(pool, descriptor_index) << std::endl;
        std::cout << "\tflags: 0x" << std::hex << access_flags << std::dec << std::endl;
        
        uint16_t attributes_count = read_u2(file);
        std::cout << "\tattributes_count: " << attributes_count << std::endl;
        pular_lista_atributos(file, attributes_count, pool, "\t\t");
    }
}


/*
 * =======================================================================
 * LEITURA DOS METHODS (ATUALIZADO)
 * =======================================================================
 */

// Protótipos (Declarações antecipadas) das novas funções
void desmontar_bytecode(std::ifstream& file, uint32_t code_length, const ConstantPool& pool);
void ler_atributo_code(std::ifstream& file, const ConstantPool& pool);
void ler_atributos_do_metodo(std::ifstream& file, uint16_t attributes_count, const ConstantPool& pool);

// O "Desmontador" (Disassembler)
void desmontar_bytecode(std::ifstream& file, uint32_t code_length, const ConstantPool& pool) {
    uint32_t bytes_lidos = 0;
    while (bytes_lidos < code_length) {
        uint32_t offset = bytes_lidos;
        uint8_t opcode = read_u1(file);
        bytes_lidos++;

        std::cout << "\t\t\t\t" << std::setw(4) << offset << ": ";

        switch (opcode) {
            // --- Opcodes sem argumentos ---
            case 0x00: std::cout << "nop" << std::endl; break;
            case 0x01: std::cout << "aconst_null" << std::endl; break;
            case 0x02: std::cout << "iconst_m1" << std::endl; break;
            case 0x03: std::cout << "iconst_0" << std::endl; break;
            case 0x04: std::cout << "iconst_1" << std::endl; break;
            case 0x05: std::cout << "iconst_2" << std::endl; break;
            case 0x06: std::cout << "iconst_3" << std::endl; break;
            case 0x07: std::cout << "iconst_4" << std::endl; break;
            case 0x08: std::cout << "iconst_5" << std::endl; break;
            case 0x09: std::cout << "lconst_0" << std::endl; break;
            case 0x0a: std::cout << "lconst_1" << std::endl; break;
            case 0x0b: std::cout << "fconst_0" << std::endl; break;
            case 0x0c: std::cout << "fconst_1" << std::endl; break;
            case 0x0d: std::cout << "fconst_2" << std::endl; break; 
            case 0x0e: std::cout << "dconst_0" << std::endl; break;
            case 0x0f: std::cout << "dconst_1" << std::endl; break;
            case 0x1a: std::cout << "iload_0" << std::endl; break;
            case 0x1b: std::cout << "iload_1" << std::endl; break;
            case 0x1c: std::cout << "iload_2" << std::endl; break;
            case 0x1d: std::cout << "iload_3" << std::endl; break;
            case 0x1e: std::cout << "lload_0" << std::endl; break;
            case 0x1f: std::cout << "lload_1" << std::endl; break;
            case 0x20: std::cout << "lload_2" << std::endl; break;
            case 0x21: std::cout << "lload_3" << std::endl; break;
            case 0x24: std::cout << "fload_0" << std::endl; break;
            case 0x25: std::cout << "fload_1" << std::endl; break;
            case 0x26: std::cout << "dload_2" << std::endl; break; 
            case 0x27: std::cout << "dload_3" << std::endl; break; 
            case 0x28: std::cout << "dload_0" << std::endl; break;
            case 0x29: std::cout << "dload_1" << std::endl; break;
            case 0x2a: std::cout << "aload_0" << std::endl; break;
            case 0x2b: std::cout << "aload_1" << std::endl; break;
            case 0x2c: std::cout << "aload_2" << std::endl; break;
            case 0x2d: std::cout << "aload_3" << std::endl; break;
            case 0x2e: std::cout << "iaload" << std::endl; break;
            case 0x30: std::cout << "laload" << std::endl; break;
            case 0x31: std::cout << "faload" << std::endl; break; 
            case 0x32: std::cout << "aaload" << std::endl; break;
            case 0x33: std::cout << "baload" << std::endl; break; 
            case 0x34: std::cout << "caload" << std::endl; break; 
            case 0x35: std::cout << "saload" << std::endl; break; 
            case 0x3b: std::cout << "istore_0" << std::endl; break;
            case 0x3c: std::cout << "istore_1" << std::endl; break;
            case 0x3d: std::cout << "istore_2" << std::endl; break;
            case 0x3e: std::cout << "istore_3" << std::endl; break;
            case 0x3f: std::cout << "lstore_0" << std::endl; break;
            case 0x40: std::cout << "lstore_1" << std::endl; break;
            case 0x41: std::cout << "lstore_2" << std::endl; break;
            case 0x42: std::cout << "lstore_3" << std::endl; break;
            case 0x47: std::cout << "dstore_0" << std::endl; break;
            case 0x48: std::cout << "dstore_1" << std::endl; break;
            case 0x49: std::cout << "dstore_2" << std::endl; break;
            case 0x4a: std::cout << "dstore_3" << std::endl; break;
            case 0x4b: std::cout << "astore_0" << std::endl; break;
            case 0x4c: std::cout << "astore_1" << std::endl; break;
            case 0x4d: std::cout << "astore_2" << std::endl; break;
            case 0x4e: std::cout << "astore_3" << std::endl; break;
            case 0x4f: std::cout << "iastore" << std::endl; break;
            case 0x50: std::cout << "fastore" << std::endl; break; 
            case 0x51: std::cout << "lastore" << std::endl; break;
            case 0x52: std::cout << "dastore" << std::endl; break; 
            case 0x53: std::cout << "aastore" << std::endl; break;
            case 0x54: std::cout << "bastore" << std::endl; break; 
            case 0x55: std::cout << "castore" << std::endl; break; 
            case 0x56: std::cout << "sastore" << std::endl; break; 
            case 0x57: std::cout << "pop" << std::endl; break;
            case 0x58: std::cout << "pop2" << std::endl; break;
            case 0x59: std::cout << "dup" << std::endl; break;
            case 0x5a: std::cout << "dup_x1" << std::endl; break;
            case 0x5b: std::cout << "dup_x2" << std::endl; break;
            case 0x5c: std::cout << "dup2" << std::endl; break;
            case 0x60: std::cout << "iadd" << std::endl; break;
            case 0x61: std::cout << "ladd" << std::endl; break;
            case 0x63: std::cout << "dadd" << std::endl; break;
            case 0x64: std::cout << "isub" << std::endl; break;
            case 0x67: std::cout << "dsub" << std::endl; break;
            case 0x68: std::cout << "imul" << std::endl; break;
            case 0x6b: std::cout << "dmul" << std::endl; break;
            case 0x6c: std::cout << "idiv" << std::endl; break;
            case 0x6f: std::cout << "ddiv" << std::endl; break;
            case 0x70: std::cout << "irem" << std::endl; break;
            case 0x73: std::cout << "drem" << std::endl; break;
            case 0x77: std::cout << "dneg" << std::endl; break;
            case 0x65: std::cout << "lshl" << std::endl; break;
            case 0x85: std::cout << "i2l" << std::endl; break;
            case 0x86: std::cout << "i2f" << std::endl; break;
            case 0x87: std::cout << "i2d" << std::endl; break;
            case 0x88: std::cout << "l2i" << std::endl; break;
            case 0x8e: std::cout << "d2i" << std::endl; break;
            case 0x8f: std::cout << "d2l" << std::endl; break;
            case 0x90: std::cout << "d2f" << std::endl; break;
            case 0x94: std::cout << "lcmp" << std::endl; break;
            case 0x97: std::cout << "dcmpg" << std::endl; break;
            case 0x98: std::cout << "dcmpl" << std::endl; break;
            case 0xac: std::cout << "ireturn" << std::endl; break;
            case 0xad: std::cout << "lreturn" << std::endl; break;
            case 0xae: std::cout << "dreturn" << std::endl; break;
            case 0xaf: std::cout << "freturn" << std::endl; break;
            case 0xb0: std::cout << "areturn" << std::endl; break;
            case 0xb1: std::cout << "return" << std::endl; break;
            case 0xbe: std::cout << "arraylength" << std::endl; break;
            case 0xbf: std::cout << "athrow" << std::endl; break;

            // --- Opcodes com 1 argumento (u1) ---
            case 0x10: { /* bipush */ int8_t byte_val = (int8_t)read_u1(file); bytes_lidos++; std::cout << "bipush " << (int)byte_val << std::endl; break; }
            case 0x11: { /* sipush */ int16_t short_val = (int16_t)read_u2(file); bytes_lidos += 2; std::cout << "sipush " << short_val << std::endl; break; }
            case 0x15: { /* iload */ uint8_t index = read_u1(file); bytes_lidos++; std::cout << "iload " << (int)index << std::endl; break; }
            case 0x16: { /* lload */ uint8_t index = read_u1(file); bytes_lidos++; std::cout << "lload " << (int)index << std::endl; break; }
            case 0x17: { /* fload */ uint8_t index = read_u1(file); bytes_lidos++; std::cout << "fload " << (int)index << std::endl; break; }
            case 0x18: { /* dload */ uint8_t index = read_u1(file); bytes_lidos++; std::cout << "dload " << (int)index << std::endl; break; }
            case 0x19: { /* aload */ uint8_t index = read_u1(file); bytes_lidos++; std::cout << "aload " << (int)index << std::endl; break; }
            case 0x36: { /* istore */ uint8_t index = read_u1(file); bytes_lidos++; std::cout << "istore " << (int)index << std::endl; break; }
            case 0x37: { /* lstore */ uint8_t index = read_u1(file); bytes_lidos++; std::cout << "lstore " << (int)index << std::endl; break; }
            case 0x38: { /* fstore */ uint8_t index = read_u1(file); bytes_lidos++; std::cout << "fstore " << (int)index << std::endl; break; }
            case 0x39: { /* dstore */ uint8_t index = read_u1(file); bytes_lidos++; std::cout << "dstore " << (int)index << std::endl; break; }
            case 0x3a: { /* astore */ uint8_t index = read_u1(file); bytes_lidos++; std::cout << "astore " << (int)index << std::endl; break; }
            case 0x12: { /* ldc */ uint8_t index = read_u1(file); bytes_lidos++; std::cout << "ldc #" << (int)index << " \t// " << resolver_indice_cp(pool, index) << std::endl; break; }
            case 0xbc: { // newarray
                uint8_t atype = read_u1(file);
                bytes_lidos++;
                std::string s_type = "T_UNKNOWN";
                if(atype == 4) s_type = "T_BOOLEAN";
                else if(atype == 5) s_type = "T_CHAR";
                else if(atype == 6) s_type = "T_FLOAT";
                else if(atype == 7) s_type = "T_DOUBLE";
                else if(atype == 8) s_type = "T_BYTE";
                else if(atype == 9) s_type = "T_SHORT";
                else if(atype == 10) s_type = "T_INT";
                else if(atype == 11) s_type = "T_LONG";
                std::cout << "newarray " << s_type << " (" << (int)atype << ")" << std::endl;
                break;
            }


            // --- Opcodes com 2 argumentos (u2 - índice do CP ou s2 offset) ---
            case 0x13: { /* ldc_w */ uint16_t index = read_u2(file); bytes_lidos += 2; std::cout << "ldc_w #" << index << " \t// " << resolver_indice_cp(pool, index) << std::endl; break; }
            case 0x14: { /* ldc2_w */ uint16_t index = read_u2(file); bytes_lidos += 2; std::cout << "ldc2_w #" << index << " \t// " << resolver_indice_cp(pool, index) << std::endl; break; }
            case 0xb2: // getstatic
            case 0xb3: // putstatic
            case 0xb4: // getfield
            case 0xb5: // putfield
            case 0xb6: // invokevirtual
            case 0xb7: // invokespecial
            case 0xb8: // invokestatic
            case 0xb9: // invokeinterface
            case 0xbb: // new
            case 0xbd: // anewarray
            case 0xc0: // checkcast
            case 0xc1: // instanceof
            {
                std::string mnemonic;
                if(opcode == 0xb2) mnemonic = "getstatic";
                else if(opcode == 0xb3) mnemonic = "putstatic";
                else if(opcode == 0xb4) mnemonic = "getfield";
                else if(opcode == 0xb5) mnemonic = "putfield";
                else if(opcode == 0xb6) mnemonic = "invokevirtual";
                else if(opcode == 0xb7) mnemonic = "invokespecial";
                else if(opcode == 0xb8) mnemonic = "invokestatic";
                else if(opcode == 0xb9) mnemonic = "invokeinterface";
                else if(opcode == 0xbb) mnemonic = "new";
                else if(opcode == 0xbd) mnemonic = "anewarray";
                else if(opcode == 0xc0) mnemonic = "checkcast";
                else if(opcode == 0xc1) mnemonic = "instanceof";

                uint16_t index = read_u2(file);
                bytes_lidos += 2;
                
                // invokeinterface tem 2 bytes extras (count, 0)
                if(opcode == 0xb9) {
                    read_u1(file); // count
                    read_u1(file); // 0
                    bytes_lidos += 2;
                }

                std::cout << mnemonic << " #" << index << " \t// " << resolver_indice_cp(pool, index) << std::endl;
                break;
            }
            case 0x99: // ifeq
            case 0x9a: // ifne
            case 0x9b: // iflt
            case 0x9c: // ifge
            case 0x9d: // ifgt
            case 0x9e: // ifle
            case 0x9f: // if_icmpeq
            case 0xa0: // if_icmpne
            case 0xa1: // if_icmplt
            case 0xa2: // if_icmpge
            case 0xa3: // if_icmpgt
            case 0xa4: // if_icmple
            case 0xa5: // if_acmpeq
            case 0xa6: // if_acmpne
            case 0xa7: // goto
            case 0xc6: // ifnull
            case 0xc7: // ifnonnull
            {
                std::string mnemonic;
                if(opcode == 0x99) mnemonic = "ifeq";
                else if(opcode == 0x9a) mnemonic = "ifne";
                else if(opcode == 0xa7) mnemonic = "goto";
                else if(opcode == 0xc6) mnemonic = "ifnull";
                else if(opcode == 0xc7) mnemonic = "ifnonnull";
                else mnemonic = "branch";
                
                int16_t offset_s16 = (int16_t)read_u2(file);
                bytes_lidos += 2;
                std::cout << mnemonic << " " << (offset + offset_s16) << std::endl;
                break;
            }

            // --- Opcodes especiais ---
            case 0x84: { /* iinc */ uint8_t index = read_u1(file); int8_t const_val = (int8_t)read_u1(file); bytes_lidos += 2; std::cout << "iinc " << (int)index << ", " << (int)const_val << std::endl; break; }
            case 0xc5: { /* multianewarray */ uint16_t index = read_u2(file); uint8_t dimensions = read_u1(file); bytes_lidos += 3; std::cout << "multianewarray #" << index << " " << (int)dimensions << " \t// " << resolver_indice_cp(pool, index) << std::endl; break; }
            case 0xc8: { /* goto_w */ int32_t offset_s32 = read_s4(file); bytes_lidos += 4; std::cout << "goto_w " << (offset + offset_s32) << std::endl; break; }
            // ... (jsr_w) ...


            // --- Opcodes de switch (complexos de pular) ---
            case 0xaa: // tableswitch
            {
                std::cout << "tableswitch" << std::endl;
                uint32_t pre_padding_pos = bytes_lidos;
                uint32_t padding = (4 - (pre_padding_pos % 4)) % 4;
                file.seekg(padding, std::ios::cur);
                bytes_lidos += padding;
                uint32_t default_offset = read_u4(file);
                uint32_t low = read_u4(file);
                uint32_t high = read_u4(file);
                bytes_lidos += 12;
                uint32_t num_offsets = (high >= low) ? (high - low + 1) : 0;
                file.seekg(num_offsets * 4, std::ios::cur);
                bytes_lidos += num_offsets * 4;
                break;
            }
            case 0xab: // lookupswitch
            {
                std::cout << "lookupswitch" << std::endl;
                uint32_t pre_padding_pos = bytes_lidos;
                uint32_t padding = (4 - (pre_padding_pos % 4)) % 4;
                file.seekg(padding, std::ios::cur);
                bytes_lidos += padding;
                uint32_t default_offset = read_u4(file);
                uint32_t npairs = read_u4(file);
                bytes_lidos += 8;
                file.seekg(npairs * 8, std::ios::cur);
                bytes_lidos += npairs * 8;
                break;
            }
                
            default:
                std::cout << "Opcode desconhecido: 0x" << std::hex << (int)opcode << std::dec << std::endl;
                break;
        }
    }
}

// Lê o atributo "Code"
void ler_atributo_code(std::ifstream& file, const ConstantPool& pool) {
    uint16_t max_stack = read_u2(file);
    uint16_t max_locals = read_u2(file);
    uint32_t code_length = read_u4(file);

    std::cout << "\t\t\tmax_stack: " << max_stack << std::endl;
    std::cout << "\t\t\tmax_locals: " << max_locals << std::endl;
    std::cout << "\t\t\tcode_length: " << code_length << std::endl;
    std::cout << "\t\t\tBytecode:" << std::endl;

    desmontar_bytecode(file, code_length, pool);

    // Pular a tabela de exceções
    uint16_t exception_table_length = read_u2(file);
    std::cout << "\t\t\texception_table_length: " << exception_table_length << std::endl;
    // Cada entrada tem 8 bytes (start_pc, end_pc, handler_pc, catch_type)
    file.seekg(exception_table_length * 8, std::ios::cur);

    // Ler (e pular) os atributos do *próprio* atributo "Code"
    uint16_t code_attributes_count = read_u2(file);
    std::cout << "\t\t\tcode_attributes_count: " << code_attributes_count << std::endl;
    pular_lista_atributos(file, code_attributes_count, pool, "\t\t\t\t");
}

// Lê a lista de atributos de um método, procurando por "Code"
void ler_atributos_do_metodo(std::ifstream& file, uint16_t attributes_count, const ConstantPool& pool) {
    for (int i = 0; i < attributes_count; i++) {
        uint16_t attribute_name_index = read_u2(file);
        uint32_t attribute_length = read_u4(file);
        std::string attr_name = get_utf8(pool, attribute_name_index);

        std::cout << "\t\tAtributo: " << attr_name << " (Length: " << attribute_length << ")" << std::endl;

        if (attr_name == "Code") {
            // Se for "Code", nós o lemos
            ler_atributo_code(file, pool);
        } else {
            // Se for qualquer outro ("Exceptions", "Deprecated", etc.), nós o pulamos
            file.seekg(attribute_length, std::ios::cur);
            if (!file.good()) throw std::runtime_error("Erro ao pular atributo de metodo.");
        }
    }
}

// Função principal para ler a lista de métodos
void ler_methods(std::ifstream& file, const ConstantPool& pool) {
    uint16_t methods_count = read_u2(file);
    std::cout << "\n--- Methods (Contagem: " << methods_count << ") ---" << std::endl;

    for (int i = 0; i < methods_count; i++) {
        uint16_t access_flags = read_u2(file);
        uint16_t name_index = read_u2(file);
        uint16_t descriptor_index = read_u2(file);

        std::cout << "\nMethod #" << i << ":" << std::endl;
        std::cout << "\tNome: #" << name_index << " \t\t// " << get_utf8(pool, name_index) << std::endl;
        std::cout << "\tDescritor: #" << descriptor_index << " \t// " << get_utf8(pool, descriptor_index) << std::endl;
        std::cout << "\tflags: 0x" << std::hex << access_flags << std::dec << std::endl;

        // Cada método tem sua própria lista de atributos (ex: Code)
        uint16_t attributes_count = read_u2(file);
        std::cout << "\tattributes_count: " << attributes_count << std::endl;
        ler_atributos_do_metodo(file, attributes_count, pool);
    }
}


/*
 * =======================================================================
 * FUNÇÃO PRINCIPAL (Corrigida)
 * =======================================================================
 */
int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Uso: " << argv[0] << " <arquivo.class>" << std::endl;
        return 1;
    }

    std::string filename = argv[1];
    std::ifstream file(filename, std::ios::binary);

    if (!file) {
        std::cerr << "Erro: Nao foi possivel abrir o arquivo " << filename << std::endl;
        return 1;
    }

    ConstantPool constant_pool; // A variável se chama 'constant_pool'

    try {
        // --- 1. Cabeçalho ---
        uint32_t magic = read_u4(file);
        if (magic != 0xCAFEBABE) {
             std::cerr << "Erro: Arquivo nao e um .class valido!" << std::endl;
             return 1;
        }
        uint16_t minor_version = read_u2(file);
        uint16_t major_version = read_u2(file);
        std::cout << "Lendo arquivo: " << filename << std::endl;
        std::cout << "Magic: 0x" << std::hex << magic << std::dec << std::endl;
        std::cout << "Versao: " << major_version << "." << minor_version << std::endl;

        // --- 2. Constant Pool ---
        uint16_t cp_count = read_u2(file);
        ler_constant_pool(file, cp_count, constant_pool);
        exibir_constant_pool(constant_pool);

        // --- 3. Informações da Classe ---
        ler_class_info(file, constant_pool);

        // --- 4. Fields ---
        ler_fields(file, constant_pool);
        
        // --- 5. Methods ---
        ler_methods(file, constant_pool);

        // --- 6. Atributos da Classe (final) ---
        uint16_t class_attributes_count = read_u2(file);
        // Check if there are attributes before reading count (avoid reading past EOF)
        if (!file.eof()) {
             std::cout << "\n--- Atributos da Classe (Contagem: " << class_attributes_count << ") ---" << std::endl;
             pular_lista_atributos(file, class_attributes_count, constant_pool, "");
        } else if (file.fail() && !file.eof()) {
             // Handle potential read error if not EOF
             throw std::runtime_error("Erro ao tentar ler contagem de atributos da classe.");
        } else {
             std::cout << "\n--- Sem Atributos da Classe ---" << std::endl;
        }


        std::cout << "\nLeitura do arquivo .class concluida com sucesso." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "\nErro durante a leitura: " << e.what() << std::endl;
        return 1;
    }

    file.close();
    return 0;
}