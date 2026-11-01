//@ cpp: companion
// An ordinary C++ program. It includes the generated header and links the generated .cpp; CeeLing is not involved.
#include "11_lib_account.h"
#include <iostream>

int main() {
    auto a = std::make_unique<bank::Account>("ann", 100);   // a CeeLing object is built behind a smart pointer
    a->deposit(50);
    std::cout << a->balance << " " << a->lastDeposit() << " " << bank::fee(250) << " ";
    bank::Account* alias = a.get();                         // C++ may do what CeeLing code may not
    alias->deposit(1500);
    std::cout << bank::fee(alias->balance) << std::endl;
    return 0;
}
