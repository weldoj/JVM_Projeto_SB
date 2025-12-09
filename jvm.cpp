// jvm.cpp

#include <iostream>
#include <string>
#include <stdexcept>
#include <vector>
#include <iomanip>

// Inclusão dos módulos do seu projeto
#include "classfile.h"      // Leitura e Estruturas
#include "disassembler.h"   // Exibição e Desmontagem
#include "interpreter.h"    // Execução e Runtime (Corretude)

/**
 * @brief Função principal da Máquina Virtual Java (JVM).
 * * Responsável por:
 * 1. Processar as flags de comando (-display ou -run).
 * 2. Chamar o leitor (classfile.cpp) para carregar o arquivo.
 * 3. Chamar o exibidor (disassembler.cpp) ou o interpretador (interpreter.cpp).
 * 4. Gerenciar o fluxo de exceções.
 */
int main(int argc, char* argv[]) {
    
    // --- 1. Processamento de Argumentos ---
    if (argc != 3) {
        std::cerr << "Uso incorreto." << std::endl;
        std::cerr << "Uso: " << argv[0] << " <flag> <arquivo.class>" << std::endl;
        std::cerr << "Flags validas: -display (Exibir bytecode) ou -run (Interpretar)" << std::endl;
        return 1;
    }

    std::string flag = argv[1];
    std::string filename = argv[2];
    ClassFile class_data;

    std::cout << "==================================================" << std::endl;
    std::cout << "🚀 JVM v1.0: Processando arquivo " << filename << std::endl;
    std::cout << "==================================================" << std::endl;

    try {
        // --- 2. FASE DE LEITURA (Comum a ambas as flags) ---
        // Chama a função principal de leitura do módulo classfile.cpp
        ler_class_file(filename, class_data); 
        std::cout << "✅ Leitura do arquivo .class concluída com sucesso." << std::endl;

        // REGISTRAR NA METHOD AREA
        std::string class_name = get_class_name(class_data.constant_pool, class_data.this_class_idx);
        method_area[class_name] = std::move(class_data);
        // Nota: class_data agora está vazio (move), usamos a referência do map
        ClassFile& loaded_class = method_area[class_name];

        // --- 3. FASE DE CONTROLE E EXECUÇÃO ---
        if (flag == "-display") {
            // Requisito: Leitor de ponto class e exibidor de bytecode.
            std::cout << "\n--- Modo: EXIBIDOR/DESMONTAGEM ---" << std::endl;
            
            // Exibir cabeçalho e informações gerais
            exibir_class_info(class_data);
            
            // Exibir Constant Pool (Parte essencial da avaliação)
            exibir_constant_pool(class_data.constant_pool);
            
            // Exibir Fields
            exibir_fields(class_data);
            
            // Exibir Methods (Isso chama a desmontagem do bytecode)
            exibir_methods(class_data);
            
            std::cout << "\n==================================================" << std::endl;
            std::cout << "Exibicao Concluida." << std::endl;
            std::cout << "==================================================" << std::endl;

        } else if (flag == "-run") {
            // Requisito: Corretude da máquina virtual (Interpretar).
            std::cout << "\n--- Modo: INTERPRETADOR (EXECUÇÃO) ---" << std::endl;
            
            // Chama a função principal de execução do módulo interpreter.cpp
            executar_jvm(loaded_class); 

            std::cout << "\n==================================================" << std::endl;
            std::cout << "Execucao Concluida." << std::endl;
            std::cout << "==================================================" << std::endl;

        } else {
            std::cerr << "ERRO: Flag de operacao desconhecida: " << flag << std::endl;
            return 1;
        }

    } catch (const std::exception& e) {
        // Captura e reporta erros de I/O, formato, ou runtime da JVM
        std::cerr << "\n==================================================" << std::endl;
        std::cerr << "❌ ERRO FATAL: " << e.what() << std::endl;
        std::cerr << "==================================================" << std::endl;
        return 1;
    }

    return 0;
}