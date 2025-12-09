// interpreter.h

#ifndef INTERPRETER_H
#define INTERPRETER_H
#define T_BOOLEAN 4
#define T_CHAR    5
#define T_FLOAT   6
#define T_DOUBLE  7
#define T_BYTE    8
#define T_SHORT   9
#define T_INT     10 // O mais comum
#define T_LONG    11

#include "classfile.h" 
#include <vector>
#include <cstdint>
#include <stdexcept>

// =======================================================================
// 1. ESTRUTURAS DE DADOS DE EXECUÇÃO
// =======================================================================

typedef uint32_t jword;

// Endereço do objeto no Heap (Referência da JVM)
typedef jword jref; 

// Estrutura para simular um Objeto/Array no Heap
struct HeapObject {
    // 0: Objeto de Classe | 1: Array de Primitivos | 2: Array de Referências | 3: String
    int type; 
    size_t size; // Tamanho total em unidades de jword (campos ou elementos do array).
    std::vector<jword> data; // Os dados reais (campos de instância, elementos).
    std::string class_name;  // Nome resolvido da classe (essencial para polimorfismo)

};

// Estrutura do Frame de Pilha (Stack Frame)
struct Frame {
    std::vector<jword> local_variables;
    std::vector<jword> operand_stack;
    uint32_t pc;
    const std::vector<uint8_t>* code;
    const std::vector<CodeAttribute::ExceptionTableEntry>* exception_table;
    const ConstantPool* class_constant_pool;
    
    Frame(const MethodInfo& method, const ConstantPool& cp);
};

// A Pilha da JVM (global para thread principal)
extern std::vector<Frame> jvm_stack;

// O Heap Simulado: O índice no vetor é a referência (jref).
extern std::vector<HeapObject> heap; 

// Method Area: Mapa de Nome da Classe -> ClassFile carregado
#include <map>
#include <string>
extern std::map<std::string, ClassFile> method_area;

// Função para obter (ou carregar) uma classe
ClassFile* get_class_from_method_area(const std::string& class_name); 

// =======================================================================
// 2. PROTÓTIPOS DE FUNÇÕES DE MANIPULAÇÃO DE DADOS
// =======================================================================

// Funções de leitura que avançam o Program Counter (PC)
uint8_t fetch_u1(Frame& frame);
uint16_t fetch_u2(Frame& frame);
int16_t fetch_s2(Frame& frame); 

// Funções de manipulação da Pilha de Operandos (32 bits)
jword pop_jword(Frame& frame);
void push_jword(Frame& frame, jword value);

// Funções de manipulação da Pilha de Operandos (64 bits - Categoria 2)
void push_jlong(Frame& frame, int64_t value);
int64_t pop_jlong(Frame& frame);
void push_jdouble(Frame& frame, double value);
double pop_jdouble(Frame& frame);

// Funções de Gerenciamento de Heap
jref allocate_heap_object(int type, size_t size, std::string class_name);

// =======================================================================
// 3. PROTÓTIPOS DE EXECUÇÃO PRINCIPAL
// =======================================================================

void run_frame(Frame& frame);
void executar_jvm(ClassFile& class_data);

#endif // INTERPRETER_H