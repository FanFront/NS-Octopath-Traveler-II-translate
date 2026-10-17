#include "loader.h"
#include <exception>
#include <iostream>
int main(int argc, char** argv) {
    if (argc!=2) { std::cerr << "Usage: ot2-inspect <ExeFS>\n"; return 2; }
    try { std::cout << ot2::inspect_modules(argv[1]) << '\n'; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
