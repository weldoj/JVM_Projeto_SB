// classfile.h

#ifndef CLASSFILE_H
#define CLASSFILE_H

#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <string>
#include <memory> // Para shared_ptr/unique_ptr, se for usar mais tarde

// =======================================================================
// 1. CONSTANTES DA JVM
// =======================================================================

// Tags do Constant Pool
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

// =======================================================================
// 2. ESTRUTURAS DO ARQUIVO .CLASS
// =======================================================================

// A estrutura para uma entrada no Constant Pool (CP)
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

// Estrutura para o atributo Code (essencial para o Interpretador)
struct CodeAttribute {
    uint16_t max_stack;
    uint16_t max_locals;
    uint32_t code_length;
    std::vector<uint8_t> code; // Os bytes do bytecode
    
    struct ExceptionTableEntry {
        uint16_t start_pc;
        uint16_t end_pc;
        uint16_t handler_pc;
        uint16_t catch_type;
    };
    
    uint16_t exception_table_length;
    std::vector<ExceptionTableEntry> exception_table;
};

// Estrutura para os Methods
struct MethodInfo {
    uint16_t access_flags;
    uint16_t name_index;        // Índice para CONSTANT_Utf8 (nome)
    uint16_t descriptor_index;  // Índice para CONSTANT_Utf8 (descritor)
    uint16_t attributes_count;
    // Opcional: Para facilitar, você pode guardar o CodeAttribute aqui
    CodeAttribute code_attribute; 
};

// Estrutura para os Fields
struct FieldInfo {
    uint16_t access_flags;
    uint16_t name_index;
    uint16_t descriptor_index;
    uint16_t attributes_count;
    // Poderia adicionar estruturas para atributos como ConstantValue
};

// Estrutura Principal: O ClassFile
struct ClassFile {
    uint32_t magic;
    uint16_t minor_version;
    uint16_t major_version;
    ConstantPool constant_pool;
    uint16_t access_flags;
    uint16_t this_class_idx;
    uint16_t super_class_idx;
    std::vector<uint16_t> interfaces;
    std::vector<FieldInfo> fields;
    std::vector<MethodInfo> methods;
    uint16_t attributes_count;
    // Poderia adicionar atributos de classe (SourceFile, InnerClasses)
};

// =======================================================================
// 3. PROTÓTIPOS DE FUNÇÕES AUXILIARES (Big-Endian e Leitura)
// =======================================================================

uint32_t read_u4(std::ifstream& file);
uint16_t read_u2(std::ifstream& file);
uint8_t read_u1(std::ifstream& file);

// =======================================================================
// 4. PROTÓTIPOS DE FUNÇÕES DE RESOLUÇÃO
// =======================================================================

// Funções para resolver referências no CP (usadas pelo Disassembler e Interpretador)
std::string get_utf8(const ConstantPool& pool, uint16_t index);
std::string get_class_name(const ConstantPool& pool, uint16_t class_index);
std::string resolver_indice_cp(const ConstantPool& pool, uint16_t index);

// =======================================================================
// 5. PROTÓTIPOS DE FUNÇÕES DE LEITURA (Principal)
// =======================================================================

// Lê todo o arquivo .class e preenche a estrutura ClassFile
void ler_class_file(const std::string& filename, ClassFile& class_data);

#endif // CLASSFILE_H