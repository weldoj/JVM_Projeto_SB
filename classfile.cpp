// classfile.cpp

#include "classfile.h" // Inclui as declarações e estruturas
#include <stdexcept>
#include <cstring>   // Para std::memcpy
#include <iomanip>
#include <iostream>

// =======================================================================
// 1. FUNÇÕES AUXILIARES BIG-ENDIAN (Implementação)
// =======================================================================

// Nota: Estas implementações deveriam idealmente estar aqui, mas foram declaradas como protótipos em .h.
// Para fins de compilação, é mais simples mantê-las aqui se não precisar de acesso externo.
// Vamos movê-las de volta para o .cpp, pois não precisam de acesso externo.

uint32_t swap_uint32(uint32_t val) {
    return ((val << 24) & 0xFF000000) | ((val <<  8) & 0x00FF0000) |
           ((val >>  8) & 0x0000FF00) | ((val >> 24) & 0x000000FF);
}
uint16_t swap_uint16(uint16_t val) {
    return ((val << 8) & 0xFF00) | ((val >> 8) & 0x00FF);
}

// =======================================================================
// 2. FUNÇÕES DE LEITURA BINÁRIA (Implementação)
// =======================================================================

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
uint8_t read_u1(std::ifstream& file) {
    uint8_t value;
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!file.good()) throw std::runtime_error("Erro ao ler u1.");
    return value;
}

// =======================================================================
// 3. FUNÇÕES DE RESOLUÇÃO (Implementação)
// =======================================================================

std::string get_utf8(const ConstantPool& pool, uint16_t index) {
    try {
        if (index == 0 || index >= pool.size() || pool.at(index).tag != CONSTANT_Utf8) return "[Indice Utf8 invalido]";
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

// Implementação da função para resolver e descrever o índice do CP (mantida para reuso no disassembler/leitor)
std::string resolver_indice_cp(const ConstantPool& pool, uint16_t index) {
    try {
        const ConstantInfo& c = pool.at(index);
        std::string s = "";
        // Simplificação: apenas descrevemos o tipo (a lógica completa pode ficar no disassembler)
        switch (c.tag) {
            case CONSTANT_Fieldref:
            case CONSTANT_Methodref:
            case CONSTANT_InterfaceMethodref: 
                s += get_class_name(pool, c.index1) + "." + get_utf8(pool, pool.at(c.index2).index1);
                break;
            case CONSTANT_Class: s += "Class " + get_class_name(pool, index); break;
            case CONSTANT_String: s += "String \"" + get_utf8(pool, c.index1) + "\""; break;
            case CONSTANT_Integer: s += "Int " + std::to_string((int32_t)c.bytes4); break;
            // ... (Implementação completa de Long/Double/Float é longa, será mantida resumida aqui)
            default: s += "[Tipo " + std::to_string(c.tag) + " nao resolvido]"; break;
        }
        return s;
    } catch (const std::exception& e) {
        return "[Erro ao resolver indice]";
    }
}

// =======================================================================
// 4. LEITURA DE SEÇÕES ESPECÍFICAS
// =======================================================================

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
                if (!file.good()) throw std::runtime_error("Erro ao ler dados Utf8.");
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
                i++; // Long e Double ocupam dois slots
                break;
            default:
                throw std::runtime_error("Tag do Constant Pool desconhecida: " + std::to_string(tag));
        }
    }
}

// --- Funções de Leitura de Atributos ---

// Função genérica para pular atributos (o disassembler cuidará de exibir)
void pular_lista_atributos(std::ifstream& file, uint16_t attributes_count) {
    for (int i = 0; i < attributes_count; i++) {
        read_u2(file); // attribute_name_index
        uint32_t attribute_length = read_u4(file);
        file.seekg(attribute_length, std::ios::cur);
        if (!file.good()) throw std::runtime_error("Erro ao pular atributo.");
    }
}

// Função MODIFICADA: Lê o CodeAttribute e ARMAZENA o bytecode
void ler_atributo_code(std::ifstream& file, MethodInfo& method) {
    CodeAttribute& code_attr = method.code_attribute;

    code_attr.max_stack = read_u2(file);
    code_attr.max_locals = read_u2(file);
    code_attr.code_length = read_u4(file);

    // 1. LER E ARMAZENAR O BYTECODE
    code_attr.code.resize(code_attr.code_length);
    file.read(reinterpret_cast<char*>(code_attr.code.data()), code_attr.code_length);
    if (!file.good()) throw std::runtime_error("Erro ao ler bytecode.");

    // 2. LER A TABELA DE EXCEÇÕES
    code_attr.exception_table_length = read_u2(file);
    code_attr.exception_table.resize(code_attr.exception_table_length);
    
    for (int i = 0; i < code_attr.exception_table_length; i++) {
        code_attr.exception_table[i].start_pc = read_u2(file);
        code_attr.exception_table[i].end_pc = read_u2(file);
        code_attr.exception_table[i].handler_pc = read_u2(file);
        code_attr.exception_table[i].catch_type = read_u2(file);
    }

    // 3. Pular atributos do próprio atributo "Code"
    uint16_t code_attributes_count = read_u2(file);
    pular_lista_atributos(file, code_attributes_count);
}

// Lê a lista de atributos de um método, procurando por "Code"
void ler_atributos_do_metodo(std::ifstream& file, MethodInfo& method, const ConstantPool& pool) {
    method.attributes_count = read_u2(file);
    
    for (int i = 0; i < method.attributes_count; i++) {
        uint16_t attribute_name_index = read_u2(file);
        uint32_t attribute_length = read_u4(file);
        std::string attr_name = get_utf8(pool, attribute_name_index);

        // Se for "Code", nós o lemos e armazenamos o bytecode
        if (attr_name == "Code") {
            ler_atributo_code(file, method);
        } else {
            // Se for qualquer outro, pulamos
            file.seekg(attribute_length, std::ios::cur);
            if (!file.good()) throw std::runtime_error("Erro ao pular atributo de metodo.");
        }
    }
}

void ler_methods(std::ifstream& file, ClassFile& class_data) {
    uint16_t methods_count = read_u2(file);
    class_data.methods.resize(methods_count);

    for (int i = 0; i < methods_count; i++) {
        MethodInfo& m = class_data.methods[i];
        m.access_flags = read_u2(file);
        m.name_index = read_u2(file);
        m.descriptor_index = read_u2(file);
        
        // Lê e armazena os atributos, incluindo o bytecode do atributo "Code"
        ler_atributos_do_metodo(file, m, class_data.constant_pool);
    }
}

void ler_fields(std::ifstream& file, ClassFile& class_data) {
    uint16_t fields_count = read_u2(file);
    class_data.fields.resize(fields_count);

    for (int i = 0; i < fields_count; i++) {
        FieldInfo& f = class_data.fields[i];
        f.access_flags = read_u2(file);
        f.name_index = read_u2(file);
        f.descriptor_index = read_u2(file);
        f.attributes_count = read_u2(file);
        
        pular_lista_atributos(file, f.attributes_count);
    }
}


// =======================================================================
// 5. FUNÇÃO PRINCIPAL DE LEITURA (Ponto de entrada para o JVM.cpp)
// =======================================================================

void ler_class_file(const std::string& filename, ClassFile& class_data) {
    std::ifstream file(filename, std::ios::binary);

    if (!file) {
        throw std::runtime_error("Nao foi possivel abrir o arquivo " + filename);
    }

    try {
        // 1. Cabeçalho
        class_data.magic = read_u4(file);
        if (class_data.magic != 0xCAFEBABE) {
             throw std::runtime_error("Arquivo nao e um .class valido!");
        }
        class_data.minor_version = read_u2(file);
        class_data.major_version = read_u2(file);

        // 2. Constant Pool
        uint16_t cp_count = read_u2(file);
        ler_constant_pool(file, cp_count, class_data.constant_pool);

        // 3. Informações da Classe
        class_data.access_flags = read_u2(file);
        class_data.this_class_idx = read_u2(file);
        class_data.super_class_idx = read_u2(file);
        
        // Interfaces
        uint16_t interfaces_count = read_u2(file);
        class_data.interfaces.resize(interfaces_count);
        for (int i = 0; i < interfaces_count; i++) {
            class_data.interfaces[i] = read_u2(file);
        }

        // 4. Fields
        ler_fields(file, class_data);
        
        // 5. Methods (Inclui a leitura e armazenamento do bytecode)
        ler_methods(file, class_data);

        // 6. Atributos da Classe (final)
        class_data.attributes_count = read_u2(file);
        pular_lista_atributos(file, class_data.attributes_count);
        
        // --- DETALHES DO LEITOR ---
        std::cout << "\n\t[LEITOR] Versao do ClassFile: " << class_data.major_version << "." << class_data.minor_version << std::endl;
        std::cout << "\t[LEITOR] Constant Pool Count: " << cp_count << std::endl;
        std::cout << "\t[LEITOR] Interfaces: " << interfaces_count << std::endl;
        std::cout << "\t[LEITOR] Fields: " << class_data.fields.size() << std::endl;
        std::cout << "\t[LEITOR] Methods: " << class_data.methods.size() << std::endl;
        std::cout << "\t[LEITOR] Attributes: " << class_data.attributes_count << std::endl;

        
    } catch (const std::exception& e) {
        file.close();
        throw; // Relança o erro para o jvm.cpp principal
    }

    file.close();
}