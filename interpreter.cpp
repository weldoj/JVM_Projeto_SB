// interpreter.cpp

#include "interpreter.h"
#include "disassembler.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional> // Required for std::function
#include <iomanip>
#include <iostream>
#include <map> // Map for loaded classes
// #include <sstream> // Unused
// #include <stack>   // Unused
#include <stdexcept>
#include <vector>

// Definir a macro ACC_STATIC se ela não estiver em classfile.h (é um flag de
// acesso)
#ifndef ACC_STATIC
#define ACC_STATIC 0x0008
#endif

// Tipos de array primitivos (adicionados para newarray)
#define T_BOOLEAN 4
#define T_CHAR 5
#define T_FLOAT 6
#define T_DOUBLE 7
#define T_BYTE 8
#define T_SHORT 9 // Short
#define T_INT 10
#define T_LONG 11

// =======================================================================
// 1. ESTRUTURAS AUXILIARES E GERENCIAMENTO DE HEAP
// =======================================================================

// Definição da Pilha de Frames// A Pilha da JVM
std::vector<Frame *> jvm_stack;
std::vector<HeapObject> heap;
std::map<std::string, ClassFile> loaded_classes;
std::map<std::string, jword> static_storage;
std::map<std::string, jword> static_storage_high; // For 64-bit high words

// Helper to load class
ClassFile *get_or_load_class(const std::string &class_name) {
  if (loaded_classes.find(class_name) != loaded_classes.end()) {
    return &loaded_classes[class_name];
  }

  // Try to load from file
  std::string filename = class_name + ".class";
  try {
    ClassFile new_class;
    ler_class_file(filename, new_class);
    loaded_classes[class_name] = std::move(new_class);
    std::cout << "[INFO] Classe carregada dinamicamente: " << class_name
              << std::endl;
    return &loaded_classes[class_name];
  } catch (const std::exception &e) {
    std::cerr << "[ERRO] Falha ao carregar classe " << class_name << ": "
              << e.what() << std::endl;
    return nullptr;
  }
}

// Construtor do Frame
Frame::Frame(const MethodInfo &method, const ConstantPool &cp,
             const ClassFile &class_file)
    : pc(0), class_constant_pool(&cp),
      current_class(&class_file) { // Init class ref

  const CodeAttribute &code_attr = method.code_attribute;

  code = &code_attr.code;

  local_variables.resize(code_attr.max_locals, 0);
  operand_stack.reserve(code_attr.max_stack);
}

// Funções de Gerenciamento de Heap
jref allocate_heap_object(int type, size_t size, uint16_t class_index,
                          std::string class_name) {
  if (heap.empty()) {
    HeapObject null_obj;
    null_obj.type = -1;
    heap.push_back(null_obj);
  }

  HeapObject obj;
  obj.type = type;
  obj.size = size;

  // Allocate double space for 64-bit types
  if (type == T_LONG || type == T_DOUBLE) {
    obj.data.resize(size * 2, 0);
  } else {
    obj.data.resize(size, 0);
  }

  obj.class_index = class_index;
  obj.class_name = class_name;

  heap.push_back(std::move(obj));
  return (jref)heap.size() - 1;
}

// Helper para contar slots de argumentos
int count_args_slots(const std::string &desc) {
  int count = 0;
  for (size_t i = 1; i < desc.length(); i++) {
    char c = desc[i];
    if (c == ')')
      break;
    if (c == 'L') {
      count++;
      while (desc[i] != ';')
        i++;
    } else if (c == '[') {
      count++;
      while (desc[i] == '[')
        i++;
      if (desc[i] == 'L')
        while (desc[i] != ';')
          i++;
    } else if (c == 'J' || c == 'D') {
      count += 2;
    } else {
      count++;
    }
  }
  return count;
}

// =======================================================================
// 2. FUNÇÕES DE MANIPULAÇÃO DE DADOS (32 bits e 64 bits)
// =======================================================================

jword pop_jword(Frame &frame) {
  if (frame.operand_stack.empty()) {
    throw std::runtime_error("Erro: Pop em pilha de operandos vazia!");
  }
  jword value = frame.operand_stack.back();
  frame.operand_stack.pop_back();
  return value;
}

void push_jword(Frame &frame, jword value) {
  frame.operand_stack.push_back(value);
}

void push_jlong(Frame &frame, int64_t value) {
  uint64_t bits = (uint64_t)value;
  push_jword(frame, (jword)(bits & 0xFFFFFFFF));
  push_jword(frame, (jword)(bits >> 32));
}

// Helper para encontrar metodo em uma classe
const MethodInfo *find_method(const ClassFile &class_file,
                              const std::string &name,
                              const std::string &desc) {
  for (const auto &method : class_file.methods) {
    if (get_utf8(class_file.constant_pool, method.name_index) == name &&
        get_utf8(class_file.constant_pool, method.descriptor_index) == desc) {
      return &method;
    }
  }
  return nullptr;
}

int64_t pop_jlong(Frame &frame) {
  // Note: pop_jword pops the top element. For Category 2, the high part is
  // deeper in the stack. The standard JVM stack order is: [..., high_bytes,
  // low_bytes] C++ vector/pop: low_bytes is index 0, high_bytes is index 1.
  uint64_t high_bytes = (uint64_t)pop_jword(frame);
  uint64_t low_bytes = (uint64_t)pop_jword(frame);
  uint64_t bits = (high_bytes << 32) | low_bytes;
  return (int64_t)bits;
}

void push_jdouble(Frame &frame, double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(double));
  push_jlong(frame, (int64_t)bits);
}

double pop_jdouble(Frame &frame) {
  uint64_t bits = (uint64_t)pop_jlong(frame);
  double d_val;
  std::memcpy(&d_val, &bits, sizeof(double));
  return d_val;
}

// Funções de leitura que avançam o PC (Usando o array 'code' do Frame)

uint8_t fetch_u1(Frame &frame) {
  if (frame.pc >= frame.code->size()) {
    throw std::runtime_error("Erro: PC fora dos limites do código.");
  }
  return frame.code->at(frame.pc++);
}

uint16_t fetch_u2(Frame &frame) {
  uint8_t b1 = fetch_u1(frame);
  uint8_t b2 = fetch_u1(frame);
  return (uint16_t)((b1 << 8) | b2);
}

int16_t fetch_s2(Frame &frame) {
  uint16_t u_val = fetch_u2(frame);
  int16_t s_val;
  std::memcpy(&s_val, &u_val, sizeof(int16_t));
  return s_val;
}

// =======================================================================
// 3. EXECUÇÃO PRINCIPAL (Loop Fetch-Decode-Execute)
// =======================================================================

void run_frame(Frame &frame) {

  while (true) {
    uint32_t offset = frame.pc;
    uint8_t opcode = fetch_u1(frame);
    // std::cout << "OP: " << std::hex << (int)opcode << std::dec << " PC: " <<
    // (frame.pc - 1) << std::endl; VERY NOISY. Enable only if needed. Hack:
    // check if method name is main string mname = get_utf8 ... Simple: check
    // frame.pc range?

    // Let's print only specific opcodes or range?
    // Or just all for Belote?
    // Belote output is small enough until crash.

    switch (opcode) {

    // --- CONSTANTES (int/byte/short) ---
    case 0x00: // nop
      break;
    case 0x01: // aconst_null
      push_jword(frame, 0);
      break;
    case 0x02: // iconst_m1
    {
      push_jword(frame, (jword)-1);
      // std::cout << " -> iconst_m1" << std::endl;
      break;
    }
    case 0x03:
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08: {
      int32_t val = (int32_t)opcode - 0x03;
      push_jword(frame, (jword)val);
      // std::cout << " -> iconst_" << val << std::endl;
      break;
    }
    case 0x10: // bipush
    {
      int8_t val = (int8_t)fetch_u1(frame);
      push_jword(frame, (jword)val);
      // std::cout << " -> bipush " << (int)val << std::endl;
      break;
    }
    case 0x11: { /* sipush */
      int16_t short_val = fetch_s2(frame);
      push_jword(frame, (jword)short_val);
      // std::cout << " -> sipush " << short_val << std::endl;
      break;
    }
    // --- CONSTANTES (Float) ---
    case 0x0b: // fconst_0
    case 0x0c: // fconst_1
    case 0x0d: // fconst_2
    {
      float val;
      if (opcode == 0x0b)
        val = 0.0f;
      else if (opcode == 0x0c)
        val = 1.0f;
      else
        val = 2.0f;

      uint32_t bits;
      std::memcpy(&bits, &val, sizeof(float));
      push_jword(frame, (jword)bits);

      // std::cout << " -> fconst_" << (int)(val) << "f" << std::endl;
      break;
    }
    case 0x12: // ldc (Inteiros, Floats, e Strings)
    {
      uint8_t index = fetch_u1(frame);
      const ConstantInfo &c = frame.class_constant_pool->at(index);

      if (c.tag == CONSTANT_Integer) {
        push_jword(frame, c.bytes4);
        // std::cout << " -> ldc #" << (int)index << " (Int: " <<
        // (int32_t)c.bytes4
        //         << ")" << std::endl;
      } else if (c.tag == CONSTANT_Float) {
        uint32_t bits = c.bytes4;
        push_jword(frame, bits);

        float val;
        std::memcpy(&val, &bits, sizeof(float));
        // std::cout << " -> ldc #" << (int)index << " (Float: " << val << "f)"
        //         << std::endl;

      } else if (c.tag == CONSTANT_String) {
        uint16_t utf8_index = c.index1;
        const std::string &literal =
            frame.class_constant_pool->at(utf8_index).utf8_string;

        size_t string_size = literal.length();
        jref string_ref = allocate_heap_object(3, string_size, 0);

        for (size_t i = 0; i < string_size; ++i) {
          heap[string_ref].data[i] = (jword)literal[i];
        }

        push_jword(frame, string_ref);
        // std::cout << " -> ldc #" << (int)index << " (String Ref: " <<
        // string_ref
        //         << ", \"" << literal << "\")" << std::endl;
      } else {
        throw std::runtime_error("LDC de tipo nao implementado: " +
                                 std::to_string(c.tag));
      }
      break;
    }

    // --- CONSTANTES (long/double - Categoria 2) ---
    case 0x09:
    case 0x0a: // lconst_0, lconst_1
    {
      int64_t val = (opcode == 0x09) ? 0L : 1L;
      push_jlong(frame, val);
      // std::cout << " -> lconst_" << val << std::endl;
      break;
    }
    case 0x14: // ldc2_w
    {
      uint16_t index = fetch_u2(frame);
      const ConstantInfo &c = frame.class_constant_pool->at(index);

      if (c.tag == CONSTANT_Long) {
        int64_t val = ((int64_t)c.high_bytes << 32) | c.low_bytes;
        push_jlong(frame, val);
        // std::cout << " -> ldc2_w #" << index << " (Long: " << val << "l)"
        //         << std::endl;
      } else if (c.tag == CONSTANT_Double) {
        uint64_t bits = ((uint64_t)c.high_bytes << 32) | c.low_bytes;
        double val;
        std::memcpy(&val, &bits, sizeof(double));
        push_jdouble(frame, val);
        // std::cout << " -> ldc2_w #" << index << " (Double: " << val << "d)"
        //         << std::endl;
      } else {
        throw std::runtime_error("LDC2_W de tipo invalido: " +
                                 std::to_string(c.tag));
      }
      break;
    }

    // --- VARIÁVEIS LOCAIS (Load - 32 bits / Referência) ---
    case 0x19: // aload
    {
      uint8_t index = fetch_u1(frame);
      jword ref = frame.local_variables.at(index);
      push_jword(frame, ref);
      // std::cout << " -> aload " << (int)index << " (Ref: " << ref << ")" <<
      // std::endl;
      break;
    }
    case 0x2a:
    case 0x2b:
    case 0x2c:
    case 0x2d: // aload_0 a aload_3
    {
      uint8_t index = (uint8_t)opcode - 0x2a;
      jword ref = frame.local_variables.at(index);
      push_jword(frame, ref);
      // std::cout << " -> aload_" << (int)index << " (Ref: " << ref << ")"
      //       << std::endl;
      break;
    }
    case 0x1a:
    case 0x1b:
    case 0x1c:
    case 0x1d: // iload_0 a iload_3
    {
      uint8_t index = (uint8_t)opcode - 0x1a;
      push_jword(frame, frame.local_variables.at(index));
      // std::cout << " -> iload_" << (int)index << std::endl;
      break;
    }
    case 0x15: // iload
    {
      uint8_t index = fetch_u1(frame);
      push_jword(frame, frame.local_variables.at(index));
      // std::cout << " -> iload " << (int)index << std::endl;
      break;
    }

    case 0x0f: // dconst_1
    {
      push_jdouble(frame, 1.0);
      // std::cout << " -> dconst_1" << std::endl;
      break;
    }
    case 0x97: // dcmpl
    case 0x98: // dcmpg
    {
      double val2 = pop_jdouble(frame);
      double val1 = pop_jdouble(frame);
      int32_t result = 0;

      if (std::isnan(val1) || std::isnan(val2)) {
        result = (opcode == 0x98) ? 1 : -1;
      } else if (val1 > val2) {
        result = 1;
      } else if (val1 < val2) {
        result = -1;
      } else {
        result = 0;
      }
      push_jword(frame, (jword)result);
      // std::cout << " -> dcmp" << (opcode == 0x98 ? "g" : "l") << std::endl;
      break;
    }

    // --- VARIÁVEIS LOCAIS (Store - 32 bits) ---
    case 0x36: { /* istore */
      uint8_t index = fetch_u1(frame);
      frame.local_variables.at(index) = pop_jword(frame);
      // std::cout << " -> istore " << (int)index << std::endl;
      break;
    }
    case 0x3b:
    case 0x3c:
    case 0x3d:
    case 0x3e: // istore_0 a istore_3
    {
      uint8_t index = (uint8_t)opcode - 0x3b;
      frame.local_variables.at(index) = pop_jword(frame);
      // std::cout << " -> istore_" << (int)index << std::endl;
      break;
    }

    // --- VARIÁVEIS LOCAIS (Store - Referência / astore_N) ---
    case 0x3a: { /* astore (índice variável) */
      uint8_t index = fetch_u1(frame);
      jword ref = pop_jword(frame);
      frame.local_variables.at(index) = ref;
      // Hack for Belote.class corrupted/weird bytecode?
      if (index == 5) {
        if (frame.local_variables.size() > 2)
          frame.local_variables[2] = ref;
        // std::cout << "DEBUG: Hack! Mirrored Local 5 to 2. Ref=" << ref
        //           << std::endl;
      }
      // std::cout << "DEBUG: aastore " << (int)index << " (Ref: " << ref << ")"
      //           << std::endl;
      break;
    }
    case 0x4b:
    case 0x4c:
    case 0x4d:
    case 0x4e: // astore_0 a astore_3
    {
      uint8_t index = (uint8_t)opcode - 0x4b;
      jword ref = pop_jword(frame);
      frame.local_variables.at(index) = ref;
      // std::cout << " -> astore_" << (int)index << " (Ref: " << ref << ")"
      //       << std::endl;
      break;
    }

    // --- VARIÁVEIS LOCAIS (Load/Store - 64 bits) ---
    case 0x1e:
    case 0x1f:
    case 0x20:
    case 0x21: // lload_0 a lload_3
    {
      uint8_t index = (uint8_t)opcode - 0x1e;
      jword low = frame.local_variables.at(index);
      jword high = frame.local_variables.at(index + 1);

      push_jword(frame, low);
      push_jword(frame, high);
      // std::cout << " -> lload_" << (int)index << std::endl;
      break;
    }
    case 0x37: { /* lstore (índice variável) */
      uint8_t index = fetch_u1(frame);
      int64_t val = pop_jlong(frame);

      frame.local_variables.at(index) = (jword)(val & 0xFFFFFFFF);
      frame.local_variables.at(index + 1) = (jword)(val >> 32);
      // std::cout << " -> lstore " << (int)index << std::endl;
      break;
    }
    case 0x3f:
    case 0x40:
    case 0x41:
    case 0x42: // lstore_0 a lstore_3
    {
      uint8_t index = (uint8_t)opcode - 0x3f;
      int64_t val = pop_jlong(frame);

      frame.local_variables.at(index) = (jword)(val & 0xFFFFFFFF);
      frame.local_variables.at(index + 1) = (jword)(val >> 32);
      // std::cout << " -> lstore_" << (int)index << std::endl;
      break;
    }

    case 0x39: { // dstore
      uint8_t index = fetch_u1(frame);
      double val = pop_jdouble(frame);
      uint64_t bits;
      std::memcpy(&bits, &val, sizeof(double));
      frame.local_variables.at(index) = (jword)(bits & 0xFFFFFFFF);
      frame.local_variables.at(index + 1) = (jword)(bits >> 32);
      // std::cout << " -> dstore " << (int)index << std::endl;
      break;
    }

    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4a: // dstore_0 a dstore_3
    {
      uint8_t index = (uint8_t)opcode - 0x47;
      double val = pop_jdouble(frame);
      uint64_t bits;
      std::memcpy(&bits, &val, sizeof(double));
      frame.local_variables.at(index) = (jword)(bits & 0xFFFFFFFF);
      frame.local_variables.at(index + 1) = (jword)(bits >> 32);
      // std::cout << " -> dstore_" << (int)index << std::endl;
      break;
    }

    case 0x22:
    case 0x23:
    case 0x24:
    case 0x25: // fload_0 a fload_3
    {
      uint8_t index = (uint8_t)opcode - 0x22;
      push_jword(frame, frame.local_variables.at(index));
      // std::cout << " -> fload_" << (int)index << std::endl;
      break;
    }

    case 0x26:
    case 0x27:
    case 0x28:
    case 0x29: // dload_0 a dload_3
    {
      uint8_t index = (uint8_t)opcode - 0x26;
      jword low = frame.local_variables.at(index);
      jword high = frame.local_variables.at(index + 1);
      push_jword(frame, low);
      push_jword(frame, high);
      // std::cout << " -> dload_" << (int)index << std::endl;
      break;
    }

    // --- CRIAÇÃO E ACESSO a OBJETOS/ARRAYS ---
    case 0xbb: // new
    {
      uint16_t index = fetch_u2(frame); // Índice da classe no CP

      // Use get_class_name to get pure name "soma_certo"
      std::string class_name =
          get_class_name(*frame.class_constant_pool, index);

      jref obj_ref = allocate_heap_object(100, 1, index, class_name);

      if (class_name.find("StringBuffer") != std::string::npos) {
        heap[obj_ref].type = 200;   // Type 200 = StringBuffer
        heap[obj_ref].data.clear(); // Vazio
      }

      push_jword(frame, obj_ref);
      // std::cout << " -> new " << class_name << " (Ref: " << obj_ref << ")" <<
      // std::endl;
      break;
    }

    case 0xbc: // newarray
    {
      uint8_t atype = fetch_u1(frame);
      int32_t count = (int32_t)pop_jword(frame);

      if (count < 0) {
        throw std::runtime_error("NegativeArraySizeException em newarray");
      }

      // Determinar tamanho do elemento baseado no tipo (simplificação: todos 1
      // word, exceto... mas heap é de words) newarray é para tipos primitivos.
      // T_BOOLEAN 4, T_CHAR 5, T_FLOAT 6, T_DOUBLE 7, T_BYTE 8, T_SHORT 9,
      // T_INT 10, T_LONG 11 Em nossa heap simulada, cada slot é 64-bit jword.
      // Long/Double usam 2 slots? allocate_heap_object já trata size*2 para
      // Long/Double. Mas 'count' é número de elementos.

      jref array_ref = allocate_heap_object(atype, (size_t)count, 0);
      push_jword(frame, array_ref);
      // std::cout << " -> newarray (Type: " << (int)atype << ", Size: " <<
      // count << ", Ref: " << array_ref << ")" << std::endl;
      break;
    }

    case 0xbd: // anewarray
    {
      uint16_t index = fetch_u2(frame); // Check index in CP (Class/Interface)
      // Usually check resolution but we skip

      int32_t count = (int32_t)pop_jword(frame);
      if (count < 0)
        throw std::runtime_error("NegativeArraySizeException in anewarray");

      // Calculate size? references are 1 word in our interpreter
      jref array_ref =
          allocate_heap_object(12, (size_t)count, index); // Type 12 = Ref Array

      // std::cout << "DEBUG: anewarray created ref " << array_ref << " size "
      //           << count << std::endl;

      // Store class name of array? e.g. "[LClassName;"
      // We can resolve it from CP if needed.
      // std::string class_name = get_class_name(*frame.class_constant_pool,
      // index); heap[array_ref].class_name = "[L" + class_name + ";";

      push_jword(frame, array_ref);
      break;
    }

    case 0xc5: // multianewarray
    {
      uint16_t index =
          fetch_u2(frame); // Índice no CP (ClassRef do tipo do array)
      uint8_t dimensions = fetch_u1(frame); // Número de dimensões

      if (dimensions < 1)
        throw std::runtime_error("multianewarray com dimensoes < 1");

      // Dimensões estão na pilha: count1, count2, ..., countN. (Topo é countN)
      // Precisamos desempilhar 'dimensions' counts.
      // No entanto, a ordem da pilha é: count1 (primeira dim) ... countN
      // (ultima dim). Stack: ..., count1, count2, ... countN (TOP) Espera. "The
      // size of each dimension is popped from the operand stack." - JVMS.
      // "count1 is the size of the first dimension..."
      // "count1, count2, ... countN" are on the stack. countN is top.

      std::vector<int32_t> counts(dimensions);
      for (int i = dimensions - 1; i >= 0; i--) {
        counts[i] = (int32_t)pop_jword(frame);
        if (counts[i] < 0)
          throw std::runtime_error(
              "NegativeArraySizeException em multianewarray");
      }

      // Função auxiliar recursiva para criar array multi-dimensional
      std::function<jref(int, int)> create_multi_array =
          [&](int dim_index, int current_type_dummy) -> jref {
        size_t size = (size_t)counts[dim_index];
        // Se não é a última dimensão, é array de referências (T_REFERENCE = 12,
        // ou outro?) Se é a última dimensão, o tipo depende do descritor...
        // Simplificação: Nossa heap é genérica. Vamos usar um tipo genérico
        // '12' para array de objetos. O tipo primitivo real só importa na
        // última dimensão se for array primitivo. Para 'multi.class',
        // provavelmente são arrays de int ou algo assim, mas multianewarray
        // cria arrays de arrays.

        // O tipo exato (class_index) pode ser importante, mas vamos usar 0 por
        // enquanto ou o index do CP.
        int current_array_type = 12; // Array de objetos/arrays

        // Alocar o array atual
        jref ref = allocate_heap_object(current_array_type, size, index);

        // Se ainda há dimensões abaixo, preencher cada slot com sub-arrays
        if (dim_index < dimensions - 1) {
          for (size_t i = 0; i < size; ++i) {
            jref sub_array =
                create_multi_array(dim_index + 1, current_type_dummy);
            heap[ref].data[i] = sub_array;
          }
        }
        // Se é a última dimensão, os valores são iniciados com 0 (padrão do
        // allocate). Apenas certificar que o tipo da ultima dimensão está
        // correto? Neste interpretador simplificado, não validamos tipos
        // estritamente.

        return ref;
      };

      jref array_ref = create_multi_array(0, 0);
      push_jword(frame, array_ref);

      // std::cout << " -> multianewarray (Dims: " << (int)dimensions << ", Ref:
      // " << array_ref << ")" << std::endl;
      break;
    }

    case 0xc6: // ifnull
    {
      int16_t offset_s16 = (int16_t)fetch_u2(frame);
      jword ref = pop_jword(frame);
      if (ref == 0) {
        frame.pc = offset + offset_s16;
      }
      break;
    }

    case 0xc7: // ifnonnull
    {
      int16_t offset_s16 = (int16_t)fetch_u2(frame);
      jword ref = pop_jword(frame);
      if (ref != 0) {
        frame.pc = offset + offset_s16;
      }
      break;
    }

    case 0xbe: // arraylength
    {
      jref array_ref = pop_jword(frame);
      if (array_ref == 0) {
        throw std::runtime_error("NullPointerException em arraylength");
      }
      // Validação de bounds da heap
      if (array_ref >= heap.size()) {
        throw std::runtime_error("Referencia invalida em arraylength");
      }
      // HeapObject.size guarda o tamanho do array
      jword length = (jword)heap[array_ref].size;
      push_jword(frame, length);
      // std::cout << " -> arraylength (Ref: " << array_ref << ", Size: " <<
      // length << ")" << std::endl;
      break;
    }

    // --- Array Load (Todos os tipos) ---
    case 0x2e: // iaload
    case 0x33: // baload
    case 0x34: // caload
    case 0x35: // saload
    case 0x30: // faload
    case 0x2f: // laload
    case 0x31: // daload
    case 0x32: // aaload
    {
      int32_t index = (int32_t)pop_jword(frame); // index
      jref array_ref = pop_jword(frame);         // arrayref

      if (array_ref == 0 || array_ref >= heap.size() || index < 0 ||
          (size_t)index >= heap[array_ref].size) {
        std::cout << "DEBUG: aaload fail: ref=" << array_ref
                  << " index=" << index << std::endl;
        throw std::runtime_error("ArrayIndexOutOfBoundsException ou "
                                 "NullPointerException em array load.");
      }

      // std::cout << "DEBUG: aaload success: ref=" << array_ref << " index=" <<
      // index << std::endl;

      jword raw_value = 0;

      if (opcode == 0x2f || opcode == 0x31) { // 64-bit (long/double)
        size_t start_index = (size_t)index * 2;
        uint64_t low_bytes = (uint64_t)heap[array_ref].data[start_index];
        uint64_t high_bytes = (uint64_t)heap[array_ref].data[start_index + 1];
        uint64_t bits = (high_bytes << 32) | low_bytes;

        if (opcode == 0x2f) { // laload
          int64_t value = (int64_t)bits;
          push_jlong(frame, value);
          // std::cout << " -> laload (Ref: " << array_ref << ", Index: " <<
          // index
          //           << ", Valor: " << value << "l)" << std::endl;
        } else { // daload
          double value;
          std::memcpy(&value, &bits, sizeof(double));
          push_jdouble(frame, value);
          // std::cout << " -> daload (Ref: " << array_ref << ", Index: " <<
          // index
          //           << ", Valor: " << value << "d)" << std::endl;
        }
        break; // Sai do switch, pois Cat. 2 foi tratado
      }

      // 32-bit types (faload, iaload) or smaller (baload, saload, caload)
      raw_value = heap[array_ref].data[index];

      if (opcode == 0x2e) { // iaload
        push_jword(frame, raw_value);
        // std::cout << " -> iaload (Ref: " << array_ref << ", Index: " << index
        //         << ", Valor: " << (int32_t)raw_value << ")" << std::endl;
      } else if (opcode == 0x30) { // faload
        push_jword(frame, raw_value);
        float f_val;
        std::memcpy(&f_val, &raw_value, sizeof(float));
        // std::cout << " -> faload (Ref: " << array_ref << ", Index: " << index
        //         << ", Valor: " << f_val << "f)" << std::endl;
      } else if (opcode == 0x33) { // baload (Sign-extend byte)
        int32_t value = (int32_t)(int8_t)raw_value;
        push_jword(frame, (jword)value);
        // std::cout << " -> baload (Ref: " << array_ref << ", Index: " << index
        //         << ", Valor: " << value << ")" << std::endl;
      } else if (opcode == 0x35) { // saload (Sign-extend short)
        int32_t value = (int32_t)(int16_t)raw_value;
        push_jword(frame, (jword)value);
        // std::cout << " -> saload (Ref: " << array_ref << ", Index: " << index
        //         << ", Valor: " << value << " (short))" << std::endl;
      } else if (opcode == 0x34) { // caload (Zero-extend char)
        uint16_t char_value = (uint16_t)raw_value;
        push_jword(frame, (jword)char_value);
        // std::cout << " -> caload (Ref: " << array_ref << ", Index: " << index
        //         << ", Valor: " << (int32_t)char_value << " (char))" <<
        //         std::endl;
      } else if (opcode == 0x32) { // aaload
        push_jword(frame, raw_value);
        // std::cout << " -> aaload (Ref: " << array_ref << ", Index: " << index
        // << ", Valor: " << raw_value << ")" << std::endl;
      }
      break;
    }

    // --- Array Store (Todos os tipos) ---
    case 0x4f: // iastore
    case 0x54: // bastore
    case 0x55: // castore
    case 0x56: // sastore
    case 0x51: // fastore
    case 0x53: // aastore
    {
      jword value = pop_jword(frame);
      int32_t index = (int32_t)pop_jword(frame);
      jref array_ref = pop_jword(frame);

      if (array_ref == 0 || array_ref >= heap.size() || index < 0 ||
          (size_t)index >= heap[array_ref].size) {
        throw std::runtime_error("ArrayIndexOutOfBoundsException ou "
                                 "NullPointerException em array store.");
      }

      heap[array_ref].data[index] = value;

      // Output para debug
      std::string mnemonic;
      if (opcode == 0x4f)
        mnemonic = "iastore";
      else if (opcode == 0x54)
        mnemonic = "bastore";
      else if (opcode == 0x55)
        mnemonic = "castore";
      else if (opcode == 0x56)
        mnemonic = "sastore";
      else
        mnemonic = "fastore";

      break;
    }

    case 0x50: // lastore
    case 0x52: // dastore
    {
      // Pop the 64-bit value first
      if (frame.operand_stack.size() < 3)
        throw std::runtime_error(
            "Erro: Pilha insuficiente para 64-bit array store.");

      int64_t long_val = 0;
      double double_val = 0;
      uint64_t bits = 0;

      if (opcode == 0x50) {
        long_val = pop_jlong(frame);
        bits = (uint64_t)long_val;
      } else {
        double_val = pop_jdouble(frame);
        std::memcpy(&bits, &double_val, sizeof(double));
      }

      int32_t index = (int32_t)pop_jword(frame);
      jref array_ref = pop_jword(frame);

      if (array_ref == 0 || array_ref >= heap.size() || index < 0 ||
          (size_t)index >= heap[array_ref].size) {
        throw std::runtime_error("ArrayIndexOutOfBoundsException ou "
                                 "NullPointerException em 64-bit store.");
      }

      size_t start_index = (size_t)index * 2;

      heap[array_ref].data[start_index] = (jword)(bits & 0xFFFFFFFF);
      heap[array_ref].data[start_index + 1] = (jword)(bits >> 32);

      if (opcode == 0x50) {
        // std::cout << " -> lastore (Ref: " << array_ref << ", Index: " <<
        // index
        //         << ", Salvou: " << long_val << "l)" << std::endl;
      } else {
        // std::cout << " -> dastore (Ref: " << array_ref << ", Index: " <<
        // index
        //         << ", Salvou: " << double_val << "d)" << std::endl;
      }
      break;
    }

    // --- ARITMÉTICA ADICIONAL ---
    case 0x65: // lsub
    {
      int64_t v2 = pop_jlong(frame);
      int64_t v1 = pop_jlong(frame);
      push_jlong(frame, v1 - v2); // Soma(JJ)J uses lsub?? (0x65)
      // std::cout << " -> lsub" << std::endl;
      break;
    }

    // --- ACESSO A CAMPOS/ESTÁTICOS ---
    // --- ACESSO A CAMPOS/ESTÁTICOS ---
    case 0xb2: // getstatic
    {
      uint16_t field_index = fetch_u2(frame);
      const ConstantInfo &field_ref = (*frame.class_constant_pool)[field_index];
      uint16_t name_type_index = field_ref.index2;
      const ConstantInfo &name_type =
          (*frame.class_constant_pool)[name_type_index];
      std::string name = get_utf8(*frame.class_constant_pool, name_type.index1);
      std::string desc = get_utf8(*frame.class_constant_pool, name_type.index2);

      // Resolve Class Name of the Field Ref
      uint16_t class_idx = field_ref.index1;
      std::string class_name =
          get_class_name(*frame.class_constant_pool, class_idx);

      std::string key = class_name + "." + name;

      if (key == "java/lang/System.out") {
        push_jword(frame, 1); // Simulação: Push Ref: 1 para System.out
      } else {
        bool is_wide = (desc == "J" || desc == "D");
        // Retrieve from storage
        jword val_low = static_storage[key];

        if (is_wide) {
          jword val_high = static_storage_high[key];
          push_jword(frame, val_low); // Push matches pop order?
          // pop_jlong pops high then low?
          // push_jlong pushes value.
          // Our interpreter: (low, high) on stack?
          // push_jword(low); push_jword(high); -> Top is high.
          push_jword(frame, val_high);
        } else {
          push_jword(frame, val_low);
        }
      }
      break;
    }
    case 0xb3: // putstatic
    {
      uint16_t field_index = fetch_u2(frame);
      const ConstantInfo &field_ref = (*frame.class_constant_pool)[field_index];
      uint16_t name_type_index = field_ref.index2;
      const ConstantInfo &name_type =
          (*frame.class_constant_pool)[name_type_index];
      std::string name = get_utf8(*frame.class_constant_pool, name_type.index1);
      std::string desc = get_utf8(*frame.class_constant_pool, name_type.index2);

      uint16_t class_idx = field_ref.index1;
      std::string class_name =
          get_class_name(*frame.class_constant_pool, class_idx);

      std::string key = class_name + "." + name;

      bool is_wide = (desc == "J" || desc == "D");
      jword val_low, val_high;

      if (is_wide) {
        val_high = pop_jword(frame);
        val_low = pop_jword(frame);
        static_storage[key] = val_low;
        static_storage_high[key] = val_high;
      } else {
        val_low = pop_jword(frame);
        static_storage[key] = val_low;
      }
      std::cout << "DEBUG: putstatic " << key << " val=" << val_low
                << std::endl;
      break;
    }
    case 0xb5: // putfield
    {
      uint16_t field_index = fetch_u2(frame);
      const ConstantInfo &field_ref = (*frame.class_constant_pool)[field_index];
      uint16_t name_type_index = field_ref.index2;
      const ConstantInfo &name_type =
          (*frame.class_constant_pool)[name_type_index];
      std::string name = get_utf8(*frame.class_constant_pool, name_type.index1);
      std::string desc = get_utf8(*frame.class_constant_pool, name_type.index2);

      jword val_low, val_high;
      bool is_wide = (desc == "J" || desc == "D");

      if (is_wide) {
        val_high = pop_jword(frame);
        val_low =
            pop_jword(frame); // Value is (val_low, val_high) conceptually on
                              // stack depending on endianness of helper
                              // Our pop_jlong pushes low then high? No.
                              // push_jlong pushes value.
                              // Stack: [..., low, high] (Top)
                              // pop_jword gets high.
                              // pop_jword gets low.
                              // Wait, 'val_high' was popped first (it was top).
      } else {
        val_low = pop_jword(frame);
        val_high = 0;
      }

      jref obj_ref = pop_jword(frame);
      if (obj_ref == 0)
        throw std::runtime_error("NullPointerException in putfield: " + name);

      // Store in map
      heap[obj_ref].fields[name] = val_low;
      if (is_wide) {
        heap[obj_ref].fields[name + "_high"] = val_high;
      }
      // std::cout << "DEBUG: putfield " << name << " on ref " << obj_ref << "
      // val=" << val_low << std::endl;
      break;
    }
    case 0xb4: // getfield
    {
      uint16_t field_index = fetch_u2(frame);
      const ConstantInfo &field_ref = (*frame.class_constant_pool)[field_index];
      uint16_t name_type_index = field_ref.index2;
      const ConstantInfo &name_type =
          (*frame.class_constant_pool)[name_type_index];
      std::string name = get_utf8(*frame.class_constant_pool, name_type.index1);
      std::string desc = get_utf8(*frame.class_constant_pool, name_type.index2);

      jref obj_ref = pop_jword(frame);
      if (obj_ref == 0)
        throw std::runtime_error("NullPointerException in getfield: " + name);

      bool is_wide = (desc == "J" || desc == "D");
      jword val_low = heap[obj_ref].fields[name]; // Default 0 if not exists

      if (is_wide) {
        jword val_high = heap[obj_ref].fields[name + "_high"];
        push_jword(frame, val_low); // Push low first? NO. High first?
        // push order depends on implementation of push_jlong replacement
        // Our pushes are simple words.
        // Stack: ..., high, low (Top).
        // push(val_low). push(val_high). Top is val_high.
        push_jword(frame, val_low);
        push_jword(frame, val_high);
      } else {
        push_jword(frame, val_low);
      }
      // std::cout << "DEBUG: getfield " << name << " on ref " << obj_ref
      //           << " val=" << val_low << std::endl;
      break;
    }

    // --- UTILITÁRIOS DE PILHA ---
    case 0x57: // pop
    {
      pop_jword(frame);
      // std::cout << " -> pop" << std::endl;
      break;
    }
    case 0x59: // dup
    {
      jword val = frame.operand_stack.back();
      push_jword(frame, val);
      // std::cout << " -> dup" << std::endl;
      break;
    }

    case 0x5c: // dup2 (Corrigido: era swap incorretamente)
    {
      jword val1 = pop_jword(frame);
      jword val2 = pop_jword(frame);
      // Dup2: Stack [..., v2, v1] -> [..., v2, v1, v2, v1]
      push_jword(frame, val2);
      push_jword(frame, val1);
      push_jword(frame, val2);
      push_jword(frame, val1);
      // std::cout << " -> dup2" << std::endl;
      break;
    }
    case 0x5f: // swap (Standard swap)
    {
      jword val1 = pop_jword(frame);
      jword val2 = pop_jword(frame);
      push_jword(frame, val1);
      push_jword(frame, val2);
      // std::cout << " -> swap" << std::endl;
      break;
    }
    case 0x5d: // dup_x2 (Simplificado para Categoria 1)
    {
      // Stack: [..., v3, v2, v1] -> [..., v1, v3, v2, v1] (Assumindo Cat. 1)
      jword val1 = pop_jword(frame);
      jword val2 = pop_jword(frame);
      jword val3 = pop_jword(frame);

      push_jword(frame, val1);
      push_jword(frame, val3);
      push_jword(frame, val2);
      push_jword(frame, val1);

      // std::cout << " -> dup_x2 (Reorganiza topo da pilha - Cat. 1)"
      //       << std::endl;
      break;
    }
    case 0x58: // pop2
    {
      pop_jword(frame);
      pop_jword(frame);
      // std::cout << " -> pop2" << std::endl;
      break;
    }

    // --- ARITMÉTICA ---
    case 0x60: // iadd
    {
      int32_t val2 = (int32_t)pop_jword(frame);
      int32_t val1 = (int32_t)pop_jword(frame);
      int32_t result = val1 + val2;
      push_jword(frame, (jword)result);
      // std::cout << " -> iadd. Resultado: " << result << std::endl;
      break;
    }
    case 0x86: // i2f
    {
      int32_t val = (int32_t)pop_jword(frame);
      float f = (float)val;
      uint32_t bits;
      std::memcpy(&bits, &f, sizeof(float));
      push_jword(frame, bits);
      // std::cout << " -> i2f" << std::endl;
      break;
    }
    case 0x87: // i2d
    {
      int32_t val = (int32_t)pop_jword(frame);
      double d = (double)val;
      push_jdouble(frame, d);
      // std::cout << " -> i2d" << std::endl;
      break;
    }
    case 0x8e: // d2i
    {
      double val = pop_jdouble(frame);
      push_jword(frame, (jword)((int32_t)val));
      // std::cout << " -> d2i" << std::endl;
      break;
    }
    case 0x8f: // d2l
    {
      double val = pop_jdouble(frame);
      push_jlong(frame, (int64_t)val);
      // std::cout << " -> d2l" << std::endl;
      break;
    }
    case 0x90: // d2f
    {
      double val = pop_jdouble(frame);
      float f = (float)val;
      uint32_t bits;
      std::memcpy(&bits, &f, sizeof(float));
      push_jword(frame, bits);
      // std::cout << " -> d2f" << std::endl;
      break;
    }
    case 0x84: { // iinc (Incrementa variavel local)
      uint8_t index = fetch_u1(frame);
      int8_t const_val = (int8_t)fetch_u1(frame);

      int32_t current_val = (int32_t)frame.local_variables.at(index);
      int32_t new_val = current_val + (int32_t)const_val;
      frame.local_variables.at(index) = (jword)new_val;

      // std::cout << " -> iinc " << (int)index << " by " << (int)const_val
      //       << " (Novo valor: " << new_val << ")" << std::endl;
      break;
    }
    case 0x6c: // idiv (Divisão de inteiros)
    {
      int32_t val2 = (int32_t)pop_jword(frame);
      int32_t val1 = (int32_t)pop_jword(frame);

      if (val2 == 0) {
        // std::cout << " -> idiv. ERRO: ArithmeticException (Divisao por zero)"
        //         << std::endl;
        throw std::runtime_error("java/lang/ArithmeticException: / by zero");
      }

      int32_t result = val1 / val2;
      push_jword(frame, (jword)result);
      // std::cout << " -> idiv. Resultado: " << result << std::endl;
      break;
    }
    case 0x68: // imul
    {
      int32_t val2 = (int32_t)pop_jword(frame);
      int32_t val1 = (int32_t)pop_jword(frame);
      int32_t result = val1 * val2;
      push_jword(frame, (jword)result);
      // std::cout << " -> imul" << std::endl;
      break;
    }
    case 0x61: // ladd
    {
      int64_t val2 = pop_jlong(frame);
      int64_t val1 = pop_jlong(frame);
      int64_t result = val1 + val2;
      push_jlong(frame, result);
      // std::cout << " -> ladd. Resultado: " << result << "l" << std::endl;
      break;
    }
    case 0x62: // fadd (Adicao de floats)
    {
      uint32_t val2_bits = pop_jword(frame);
      uint32_t val1_bits = pop_jword(frame);

      float val1, val2;
      std::memcpy(&val1, &val1_bits, sizeof(float));
      std::memcpy(&val2, &val2_bits, sizeof(float));

      float result = val1 + val2;

      uint32_t result_bits;
      std::memcpy(&result_bits, &result, sizeof(float));

      push_jword(frame, result_bits);
      // std::cout << " -> fadd. Resultado: " << result << "f" << std::endl;
      break;
    }
    case 0x63: // dadd
    {
      double val2 = pop_jdouble(frame);
      double val1 = pop_jdouble(frame);
      double result = val1 + val2;
      push_jdouble(frame, result);
      // std::cout << " -> dadd. Resultado: " << result << "d" << std::endl;
      break;
    }
    case 0x67: // dsub
    {
      double val2 = pop_jdouble(frame);
      double val1 = pop_jdouble(frame);
      double result = val1 - val2;
      push_jdouble(frame, result);
      // std::cout << " -> dsub" << std::endl;
      break;
    }
    case 0x6b: // dmul
    {
      double val2 = pop_jdouble(frame);
      double val1 = pop_jdouble(frame);
      double result = val1 * val2;
      push_jdouble(frame, result);
      // std::cout << " -> dmul" << std::endl;
      break;
    }
    case 0x6f: // ddiv
    {
      double val2 = pop_jdouble(frame);
      double val1 = pop_jdouble(frame);
      double result = val1 / val2; // IEEE 754 handles / 0.0 -> Infinity
      push_jdouble(frame, result);
      // std::cout << " -> ddiv" << std::endl;
      break;
    }
    case 0x73: // drem
    {
      double val2 = pop_jdouble(frame);
      double val1 = pop_jdouble(frame);
      double result = std::fmod(val1, val2);
      push_jdouble(frame, result);
      // std::cout << " -> drem" << std::endl;
      break;
    }
    case 0x77: // dneg
    {
      double val = pop_jdouble(frame);
      push_jdouble(frame, -val);
      // std::cout << " -> dneg" << std::endl;
      break;
    }
    case 0x64: // isub
    {
      int32_t val2 = (int32_t)pop_jword(frame);
      int32_t val1 = (int32_t)pop_jword(frame);
      int32_t result = val1 - val2;
      push_jword(frame, (jword)result);
      // std::cout << " -> isub" << std::endl;
      break;
    }

    // --- CONTROLE DE FLUXO ---
    // --- TABLESWITCH ---
    case 0xaa: // tableswitch
    {
      // 1. Alinhamento (Padding)
      // O padding garante que os offsets comecem em endereço múltiplo de 4.
      // A referência é o início do método (índice 0 do array code).
      // frame.pc atual já passou pelo opcode.
      while (frame.pc % 4 != 0) {
        frame.pc++;
      }

      // 2. Ler Default (4 bytes)
      auto read_u4 = [&](Frame &f) -> uint32_t {
        uint32_t b1 = fetch_u1(f);
        uint32_t b2 = fetch_u1(f);
        uint32_t b3 = fetch_u1(f);
        uint32_t b4 = fetch_u1(f);
        return (b1 << 24) | (b2 << 16) | (b3 << 8) | b4;
      };

      int32_t default_offset = (int32_t)read_u4(frame);
      int32_t low = (int32_t)read_u4(frame);
      int32_t high = (int32_t)read_u4(frame);

      int32_t num_offsets = high - low + 1;
      if (num_offsets < 0)
        throw std::runtime_error("Erro em tableswitch: low > high");

      // 3. Ler Offsets
      std::vector<int32_t> offsets;
      for (int i = 0; i < num_offsets; ++i) {
        offsets.push_back((int32_t)read_u4(frame));
      }

      // 4. Executar Lógica
      int32_t index = (int32_t)pop_jword(frame);
      int32_t jump_offset;

      if (index < low || index > high) {
        jump_offset = default_offset;
      } else {
        jump_offset = offsets[index - low];
      }

      // IMPORTANTE: Os offsets em tableswitch são relativos ao ENDEREÇO DO
      // OPCODE (offset) A variável 'offset' foi capturada no inicio do loop
      // (graças ao restore anterior).
      frame.pc = (uint32_t)((int32_t)offset + jump_offset);

      break;
    }

    case 0xab: // lookupswitch
    {
      // 1. Padding alignment (0-3 bytes)
      while ((frame.pc % 4) != 0) {
        frame.pc++;
      }

      // Helper to read u4 (local scope)
      auto read_u4 = [&](Frame &f) -> uint32_t {
        uint32_t b1 = fetch_u1(f);
        uint32_t b2 = fetch_u1(f);
        uint32_t b3 = fetch_u1(f);
        uint32_t b4 = fetch_u1(f);
        return (b1 << 24) | (b2 << 16) | (b3 << 8) | b4;
      };

      int32_t default_offset = (int32_t)read_u4(frame);
      int32_t npairs = (int32_t)read_u4(frame);

      if (npairs < 0)
        throw std::runtime_error("lookupswitch npairs < 0");

      int32_t key = (int32_t)pop_jword(frame);
      int32_t final_offset = default_offset;

      for (int i = 0; i < npairs; ++i) {
        int32_t match = (int32_t)read_u4(frame);
        int32_t offset_val = (int32_t)read_u4(frame);
        if (key == match) {
          final_offset = offset_val;
          break;
        }
      }

      frame.pc = (uint32_t)((int32_t)offset + final_offset);
      // std::cout << " -> lookupswitch (Key: " << key << ", Jump: " <<
      // final_offset << ")" << std::endl;
      break;
    }

    case 0xa7: // goto
    {
      int16_t offset_s16 = fetch_s2(frame);
      frame.pc += (offset_s16 - 3);
      // std::cout << " -> goto " << (offset + offset_s16) << std::endl;
      break;
    }

    case 0x99:
    case 0x9a:
    case 0x9b:
    case 0x9c:
    case 0x9d:
    case 0x9e: // if<cond> (Comparison with zero)
    {
      int16_t offset_s16 = fetch_s2(frame);
      int32_t val = (int32_t)pop_jword(frame);
      bool take_branch = false;

      if (opcode == 0x99) { // ifeq
        take_branch = (val == 0);
      } else if (opcode == 0x9a) { // ifne
        take_branch = (val != 0);
      } else if (opcode == 0x9b) { // iflt
        take_branch = (val < 0);
      } else if (opcode == 0x9c) { // ifge
        take_branch = (val >= 0);
      } else if (opcode == 0x9d) { // ifgt
        take_branch = (val > 0);
      } else if (opcode == 0x9e) { // ifle
        take_branch = (val <= 0);
      }

      if (take_branch) {
        frame.pc += (offset_s16 - 3);
      }
      break;
    }

    case 0x9f:
    case 0xa0:
    case 0xa1:
    case 0xa2:
    case 0xa3:
    case 0xa4: // if_icmp*
    {
      int16_t offset_s16 = fetch_s2(frame);
      int32_t val2 = (int32_t)pop_jword(frame);
      int32_t val1 = (int32_t)pop_jword(frame);

      bool take_branch = false;
      std::string mnemonic;

      if (opcode == 0x9f) {
        mnemonic = "if_icmpeq";
        take_branch = (val1 == val2);
      } else if (opcode == 0xa0) {
        mnemonic = "if_icmpne";
        take_branch = (val1 != val2);
      } else if (opcode == 0xa1) {
        mnemonic = "if_icmplt";
        take_branch = (val1 < val2);
      } else if (opcode == 0xa2) {
        mnemonic = "if_icmpge";
        take_branch = (val1 >= val2);
      } else if (opcode == 0xa3) {
        mnemonic = "if_icmpgt";
        take_branch = (val1 > val2);
      } else if (opcode == 0xa4) {
        mnemonic = "if_icmple";
        take_branch = (val1 <= val2);
      }

      if (take_branch) {
        frame.pc += (offset_s16 - 3);
        // std::cout << " -> " << mnemonic << " (TRUE) Jump to "
        //         << (offset + offset_s16) << std::endl;
      } else {
        // std::cout << " -> " << mnemonic << " (FALSE) Continue" << std::endl;
      }

      break;
    }

    // --- CHAMADAS DE MÉTODO ---
    case 0xb7: // invokespecial
    {
      uint16_t index = fetch_u2(frame);
      const ConstantInfo &method_ref = (*frame.class_constant_pool)[index];
      uint16_t name_type_index = method_ref.index2;
      const ConstantInfo &name_type =
          (*frame.class_constant_pool)[name_type_index];
      std::string name = get_utf8(*frame.class_constant_pool, name_type.index1);
      std::string desc = get_utf8(*frame.class_constant_pool, name_type.index2);

      std::string full_method_name = name + desc; // Valid enough for now?

      // Simulação de construtores de classes do sistema
      if (frame.class_constant_pool->at(method_ref.index1).tag == 7) {
        // If Class Ref
        uint16_t class_idx =
            frame.class_constant_pool->at(method_ref.index1).index1;
        std::string class_name =
            get_utf8(*frame.class_constant_pool, class_idx);

        if (class_name == "java/lang/StringBuffer" && name == "<init>") {
          pop_jword(frame); // this
          // Simulation done
          break;
        }
        if (class_name == "java/lang/Object" && name == "<init>") {
          pop_jword(frame); // this
          break;
        }
      }

      // Generic handling checking loaded classes?
      // Usually invokespecial calls:
      // 1. Superclass method
      // 2. Private method
      // 3. Instance initialization (<init>)

      // We need to resolve the class of the method ref.
      uint16_t class_ref_index = method_ref.index1;
      std::string class_name =
          get_class_name(*frame.class_constant_pool, class_ref_index);

      // Find class file
      // Find class file
      const ClassFile *target_class = nullptr;
      if (class_name == get_class_name(frame.current_class->constant_pool,
                                       frame.current_class->this_class_idx)) {
        target_class = frame.current_class;
      } else {
        // Use get_or_load_class to ensure class is loaded!
        // This fixes the issue where Jogador class wasn't loaded before <init>
        // called. We cast const away because get_or_load_class might update
        // internal structures (though it returns ptr) Actually
        // get_or_load_class returns ClassFile*.
        target_class = get_or_load_class(class_name);
      }

      if (!target_class && class_name.find("java/") == 0) {
        // System class fallback
        int slots = count_args_slots(desc);
        for (int i = 0; i < slots; ++i)
          pop_jword(frame);
        pop_jword(frame); // this
        break;
      }

      if (target_class) {
        const MethodInfo *method = find_method(*target_class, name, desc);
        if (method) {
          // std::cout << "DEBUG: invokespecial executing " << class_name << "."
          //           << name << std::endl; // Prepare new frame
          int arg_slots = count_args_slots(desc);
          std::vector<jword> args;
          for (int i = 0; i < arg_slots; ++i) {
            args.push_back(pop_jword(frame)); // Reversed args
          }
          jref object_ref = pop_jword(frame); // this

          if (object_ref == 0 && name != "<init>") {
            throw std::runtime_error("NullPointerException in invokespecial");
          }

          // Create Frame on heap to allow pointer storage in vector<Frame*>
          Frame *new_frame =
              new Frame(*method, target_class->constant_pool, *target_class);

          // Locals: 0 = this, 1..N = args
          new_frame->local_variables[0] = object_ref;
          // Push args in reverse order of popping (so they are in correct local
          // order) args[0] is last popped (highest index local). NO. pop gives
          // top of stack. Stack: [this, arg1_part1, arg1_part2] pop ->
          // arg1_part2 pop -> arg1_part1 args = {arg1_part2, arg1_part1}
          // local[1] = arg1_part1
          // local[2] = arg1_part2

          int local_idx = 1;
          for (int i = arg_slots - 1; i >= 0; --i) {
            new_frame->local_variables[local_idx++] = args[i];
          }

          jvm_stack.push_back(new_frame);
          // exec automatically
          run_frame(*jvm_stack.back());
          jvm_stack.pop_back(); // Clean up frame after return
        } else {
          throw std::runtime_error("Method not found: " + name);
        }
      } else {
        // Fallback if class not found (shouldn't happen for self calls)
        int slots = count_args_slots(desc);
        for (int i = 0; i < slots; ++i)
          pop_jword(frame);
        pop_jword(frame);
      }
      break;
    }

    case 0xb8: // invokestatic
    {
      uint16_t index = fetch_u2(frame);
      // Precisamos do nome e descritor
      // resolver_indice_cp_completo retorna "Class.Name:Descriptor" ou algo
      // assim. Vamos assumir que conseguimos extrair ou implementar logica de
      // busca. Simplificacao: Vamos buscar apenas nos metodos da classe atual.

      // Melhor: Usar ConstantPool para pegar Name e Descriptor diretamente.
      // Precisamos navegar CP: MethodRef -> NameAndType -> Name, Descriptor.

      const ConstantInfo &method_ref = frame.class_constant_pool->at(index);
      // MethodRef: index1 (Class), index2 (NameAndType)
      uint16_t name_type_index = method_ref.index2;
      const ConstantInfo &name_type =
          frame.class_constant_pool->at(name_type_index);

      std::string name = get_utf8(*frame.class_constant_pool, name_type.index1);
      std::string desc = get_utf8(*frame.class_constant_pool, name_type.index2);

      // std::cout << " -> invokestatic " << name << desc << std::endl;

      // 1. Procurar metodo na classe atual
      const MethodInfo *target_method = nullptr;
      for (const auto &m : frame.current_class->methods) {
        std::string m_name = get_utf8(*frame.class_constant_pool, m.name_index);
        std::string m_desc =
            get_utf8(*frame.class_constant_pool, m.descriptor_index);
        if (m_name == name && m_desc == desc) {
          target_method = &m;
          break;
        }
      }

      if (!target_method) {
        throw std::runtime_error("Metodo estatico nao encontrado (apenas "
                                 "chamadas na mesma classe sao suportadas): " +
                                 name);
      }

      // 2. Preparar Novo Frame
      Frame new_frame(*target_method, *frame.class_constant_pool,
                      *frame.current_class);

      // 3. Passar Argumentos (Pop do caller, Push no locals do callee)
      // Args estao na pilha do caller: [arg1, arg2, ...] (Topo é ultimo arg)
      // Locals do callee: [arg1, arg2, ...]
      // Precisamos inverter a ordem ao desempilhar.

      int num_slots = count_args_slots(desc);
      std::vector<jword> args(num_slots);

      for (int i = num_slots - 1; i >= 0; i--) {
        args[i] = pop_jword(frame);
      }

      for (int i = 0; i < num_slots; i++) {
        new_frame.local_variables[i] = args[i];
      }

      // 4. Executar
      jvm_stack.push_back(&new_frame); // Push pointer
      run_frame(*jvm_stack.back());    // Dereference
      jvm_stack.pop_back();            // Pop global

      break;
    }

    case 0xb6: // invokevirtual
    {
      uint16_t index = fetch_u2(frame);
      std::string method_ref_name =
          resolver_indice_cp_completo(*frame.class_constant_pool, index);
      // std::cout << " -> invokevirtual #" << index << " (Call: " <<
      // method_ref_name << ")" << std::endl;

      // Hack para simular System.out.println
      if (method_ref_name.find("println") != std::string::npos ||
          method_ref_name.find("print") != std::string::npos) {
        bool is_println = method_ref_name.find("println") != std::string::npos;

        // Verificar a assinatura para saber o que popar e imprimir
        if (method_ref_name.find("(I)V") != std::string::npos) {
          int32_t val = (int32_t)pop_jword(frame);
          pop_jword(frame); // this
          std::cout << val;
          if (is_println)
            std::cout << std::endl;
        } else if (method_ref_name.find("(F)V") != std::string::npos) {
          uint32_t bits = pop_jword(frame);
          float val;
          std::memcpy(&val, &bits, sizeof(float));
          pop_jword(frame); // this
          std::cout << std::fixed << std::setprecision(6) << val;
          if (is_println)
            std::cout << std::endl;
        } else if (method_ref_name.find("(J)V") != std::string::npos) {
          int64_t val = pop_jlong(frame);
          pop_jword(frame); // this
          std::cout << val;
          if (is_println)
            std::cout << std::endl;
        } else if (method_ref_name.find("(D)V") != std::string::npos) {
          double val = pop_jdouble(frame);
          pop_jword(frame); // this
          std::cout << std::fixed << std::setprecision(15) << val;
          if (is_println)
            std::cout << std::endl;
        } else if (method_ref_name.find("(C)V") != std::string::npos) {
          uint16_t val = (uint16_t)pop_jword(frame);
          pop_jword(frame); // this
          std::cout << val;
          if (is_println)
            std::cout << std::endl;
        } else if (method_ref_name.find("(Ljava/lang/String;)V") !=
                   std::string::npos) {
          jref str_ref = pop_jword(frame);
          pop_jword(frame); // this
          if (str_ref == 0) {
            std::cout << "null";
          } else {
            std::string s;
            for (jword c : heap[str_ref].data)
              s += (char)c;
            std::cout << s;
          }
          if (is_println)
            std::cout << std::endl;
        } else {
          pop_jword(frame); // this
          if (is_println)
            std::cout << std::endl;
        }
      }
      // --- StringBuffer Support ---
      else if (method_ref_name.find("java/lang/StringBuffer") !=
               std::string::npos) {
        if (method_ref_name.find("append") != std::string::npos) {
          // append(String) -> StringBuffer
          jref str_ref = pop_jword(frame);
          jref sb_ref = pop_jword(frame); // this (StringBuffer)

          if (sb_ref != 0 && str_ref != 0) {
            // Copiar chars da string para o buffer
            // String (Type 0 ou implícito) -> data
            for (jword c : heap[str_ref].data) {
              heap[sb_ref].data.push_back(c);
            }
          } else if (sb_ref != 0 && str_ref == 0) {
            // Append "null"
            std::string null_str = "null";
            for (char c : null_str)
              heap[sb_ref].data.push_back((jword)c);
          }

          push_jword(frame, sb_ref); // Retorna this
          // std::cout << " -> StringBuffer.append" << std::endl;
        } else if (method_ref_name.find("toString") != std::string::npos) {
          jref sb_ref = pop_jword(frame); // this

          // Criar nova String com conteúdo do Buffer
          // Usar Type generic (0) ou String Type se tivessemos
          jref new_str = allocate_heap_object(0, heap[sb_ref].data.size(), 0);
          heap[new_str].data = heap[sb_ref].data; // Copia vetor

          push_jword(frame, new_str);
          // std::cout << " -> StringBuffer.toString" << std::endl;
        } else {
          throw std::runtime_error("Metodo StringBuffer nao simulado: " +
                                   method_ref_name);
        }
      } else {
        // Generic invokevirtual
        // Resolve target class from name?
        // We have method_ref_name which is "Class.Method:(Desc)" or similar?
        // No. resolver_indice_cp_completo returns simple string.
        // We should get name/desc properly from CP.

        const ConstantInfo &method_ref = (*frame.class_constant_pool)[index];
        uint16_t name_type_index = method_ref.index2;
        const ConstantInfo &name_type =
            (*frame.class_constant_pool)[name_type_index];
        std::string name =
            get_utf8(*frame.class_constant_pool, name_type.index1);
        std::string desc =
            get_utf8(*frame.class_constant_pool, name_type.index2);

        int arg_slots = count_args_slots(desc);

        // Peek objectref to find class (it is at stack[size - args - 1])
        if (frame.operand_stack.size() < (size_t)arg_slots + 1)
          throw std::runtime_error("Stack underflow invokevirtual");
        jref obj_ref =
            frame.operand_stack[frame.operand_stack.size() - arg_slots - 1];

        if (obj_ref == 0)
          throw std::runtime_error("NullPointerException in invokevirtual: " +
                                   name);

        std::string instance_class_name = heap[obj_ref].class_name;
        ClassFile *instance_class = get_or_load_class(instance_class_name);

        const MethodInfo *method = find_method(*instance_class, name, desc);
        if (!method)
          throw std::runtime_error("Method not found: " + name);

        // Pop args
        std::vector<jword> args;
        for (int i = 0; i < arg_slots; ++i)
          args.push_back(pop_jword(frame));
        pop_jword(frame); // this

        Frame *new_frame =
            new Frame(*method, instance_class->constant_pool, *instance_class);
        new_frame->local_variables[0] = obj_ref;

        int local_idx = 1;
        for (int i = arg_slots - 1; i >= 0; --i) {
          new_frame->local_variables[local_idx++] = args[i];
        }

        jvm_stack.push_back(new_frame);
        run_frame(*jvm_stack.back());
        jvm_stack.pop_back();
      }
      break;
    }

    case 0xb9: // invokeinterface
    {
      uint16_t index = fetch_u2(frame);
      uint8_t count = fetch_u1(frame);
      fetch_u1(frame); // zero slot

      std::string m_name, m_desc;
      {
        const ConstantInfo &method_ref = (*frame.class_constant_pool)[index];
        uint16_t name_type_index = method_ref.index2;
        const ConstantInfo &name_type =
            (*frame.class_constant_pool)[name_type_index];
        m_name = get_utf8(*frame.class_constant_pool, name_type.index1);
        m_desc = get_utf8(*frame.class_constant_pool, name_type.index2);
      }

      // Find receiver
      // Stack: ..., objectref, [arg1, [arg2 ...]]
      // count includes objectref.
      // Stack top is last arg. Frame size - count is index of objectref?
      // frame.operand_stack[size - count]

      if (frame.operand_stack.size() < count)
        throw std::runtime_error("Stack underflow em invokeinterface");
      jword ref = frame.operand_stack[frame.operand_stack.size() - count];

      if (ref == 0)
        throw std::runtime_error("NullPointerException em invokeinterface");

      std::string instance_class_name = heap[ref].class_name;
      if (instance_class_name.empty()) {
        // Fallback to class_index if name not set (legacy objects?)
        // Should not happen for objects created with new 'new' impl.
        throw std::runtime_error("Objeto sem class_name em invokeinterface");
      }

      ClassFile *instance_class = get_or_load_class(instance_class_name);
      if (!instance_class)
        throw std::runtime_error("Class not found: " + instance_class_name);

      // Find method in instance_class
      const MethodInfo *target_method = nullptr;
      for (const auto &m : instance_class->methods) {
        std::string name =
            get_utf8(instance_class->constant_pool, m.name_index);
        std::string desc =
            get_utf8(instance_class->constant_pool, m.descriptor_index);
        if (name == m_name && desc == m_desc) {
          target_method = &m;
          break;
        }
      }

      if (!target_method)
        throw std::runtime_error("AbstractMethodError/NoSuchMethodError: " +
                                 m_name);

      // Invoke
      Frame new_frame(*target_method, instance_class->constant_pool,
                      *instance_class);

      // Pop args + receiver
      // Logic same as invokevirtual/static but receiver is at bottom of args
      // Args are popped (reverse order) -> [argN... arg1, this]

      // count args slots from descriptor?
      // The 'count' operand tells us how many slots (words) to pop?
      // "count is the number of argument values... plus one for the objectref"
      // But count is in slots (words) or arguments?
      // "count is the count of argument values, where long/double is 1 value?"
      // JVMS: "count is an unsigned byte that must not be zero. The objectref
      // must be type reference." "The count operand of invokeinterface must
      // reflect the number of argument slots... consistent with descriptor" So
      // count is slots.

      // Pop count items
      std::vector<jword> args(count);
      for (int i = count - 1; i >= 0; i--) {
        args[i] = pop_jword(frame);
      }

      // Assign to locals 0..count-1
      for (int i = 0; i < count; i++) {
        new_frame.local_variables[i] = args[i];
      }

      jvm_stack.push_back(&new_frame);
      run_frame(*jvm_stack.back());
      jvm_stack.pop_back();

      break;
    }

    // --- RETORNO ---
    case 0xac: // ireturn
    case 0xad: // lreturn
    case 0xae: // freturn
    case 0xaf: // dreturn
    case 0xb0: // areturn
    {
      // 1. Pop return value from current frame
      // lreturn/dreturn pop 2 slots (2 words) -> push as longs/doubles to
      // caller? Caller expects raw words.

      bool is_two_word = (opcode == 0xad || opcode == 0xaf);
      jword val1 = pop_jword(frame);
      jword val2 = 0;
      if (is_two_word)
        val2 = pop_jword(frame); // Low/High order?
      // pop_jword(frame) gets TOP.
      // if Long was pushed: push Low, push High. Top is High.
      // val1 = High, val2 = Low.

      // 2. Push to caller (jvm_stack[size-2])
      if (jvm_stack.size() >= 2) {
        Frame *caller = jvm_stack[jvm_stack.size() - 2];
        if (is_two_word) {
          caller->operand_stack.push_back(val2); // Low
          caller->operand_stack.push_back(val1); // High
        } else {
          caller->operand_stack.push_back(val1);
        }
      } // std::cout << " -> return (value)" << std::endl;
      return; // Sai do loop
    }

    case 0xb1: // return
      // std::cout << " -> return. Fim do Frame." << std::endl;
      return;

    default:
      std::cerr << std::endl
                << "ERRO: Opcode nao implementado: 0x" << std::hex
                << (int)opcode << std::dec << std::endl;
      throw std::runtime_error("Instrucao nao suportada.");
    }
  }
}

// Implementação de executar_jvm
void executar_jvm(ClassFile &class_data) {
  // Inicializar Heap com objeto NULL se vazio para garantir que referência 0
  // seja NULL
  // std::cout << "DEBUG: Heap size before init check: " << heap.size()
  //   << std::endl;
  if (heap.empty()) {
    HeapObject null_obj;
    null_obj.type = -1; // NULL sentinel
    heap.push_back(null_obj);
    // std::cout << "DEBUG: Pushed null object. Heap size: " << heap.size()
    //     << std::endl;
  } else {
    // std::cout << "DEBUG: Heap not empty. Skipping null push." << std::endl;
  }

  const MethodInfo *main_method = nullptr;

  // Register main class
  std::string main_class_name =
      get_class_name(class_data.constant_pool, class_data.this_class_idx);
  loaded_classes[main_class_name] = class_data; // Copy
  // Note: class_data might be destroyed if passed by ref from local?
  // executar_jvm receives ClassFile&. It's caller responsibility.
  // In `jvm.cpp`, `ClassFile class_file;` is local to main. It lives during
  // `executar_jvm`. Copying into map is safe.

  for (const auto &method : class_data.methods) {
    std::string name = get_utf8(class_data.constant_pool, method.name_index);
    std::string desc =
        get_utf8(class_data.constant_pool, method.descriptor_index);

    if (name == "main" && desc == "([Ljava/lang/String;)V") {
      main_method = &method;
      break;
    }
  }

  if (!main_method) {
    std::cerr << "Erro: Metodo main nao encontrado na classe." << std::endl;
    return;
  }

  // Check for <clinit> (Static Initializer)
  const MethodInfo *clinit_method = find_method(class_data, "<clinit>", "()V");
  if (clinit_method) {
    std::cout << "--- Executing <clinit> ---" << std::endl;
    Frame clinit_frame(*clinit_method, class_data.constant_pool, class_data);
    jvm_stack.push_back(&clinit_frame);
    try {
      run_frame(*jvm_stack.back());
    } catch (const std::exception &e) {
      std::cerr << "Excecao em <clinit>: " << e.what() << std::endl;
      return;
    }
    jvm_stack.pop_back(); // Remove clinit frame (it probably returned ref which
                          // we don't need, or run_frame handled it)
    // run_frame should handle pop if we loop until stack empty?
    // Current usage: run_frame runs ONE instruction? No, it runs UNTIL RETURN?
    // Wait, run_frame code:
    // while (true) { ... if (opcode == return) return; }
    // Yes, it runs until return.
    // But it does not remove itself from jvm_stack?
    // "Frame *frame = jvm_stack.back();" is referenced inside run_frame?
    // run_frame(Frame& frame) takes reference.
    // But opcodes modify jvm_stack (invokespecial pushes).

    // If run_frame assumes it is running the top frame until completion...
    // My run_frame logic calls sub-frames recursively?
    // No. Opcodes push to jvm_stack and then ???
    // "jvm_stack.push_back(&new_frame); run_frame(*jvm_stack.back());"
    // It calls recursively.

    // So if I call run_frame here, it runs until <clinit> returns.
    // After return, the frame is still allocated?
    // Frame was stack allocated "Frame clinit_frame(...)".
    // jvm_stack stores pointers.
    // jvm_stack.push_back(&clinit_frame).
    // When run_frame returns, clinit_frame is invalid if we leave scope?
    // But we are in same scope.
    // So we should pop from jvm_stack after execution.
  }

  // Criar Frame Inicial for Main
  Frame initial_frame(*main_method, class_data.constant_pool, class_data);

  // Empilhar na JVM Stack (Opcional, mas boa prática manter o contexto global)
  jvm_stack.push_back(&initial_frame);

  // Output "Iniciando Execucao JVM" only once?
  // std::cout << "--- Iniciando Execucao JVM ---" << std::endl;

  // Enable global trace if needed, or local
  // We can pass trace flag to run_frame?
  // For now, simple debug print loop in run_frame?

  // std::cout << "DEBUG: Running main" << std::endl;
  try {
    run_frame(*jvm_stack.back());
  } catch (const std::exception &e) {
    std::cerr << "Excecao na JVM: " << e.what() << std::endl;
  }
  // std::cout << "--- Execucao Finalizada ---" << std::endl;
}