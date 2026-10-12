#include <iostream>
struct node { long long value; node *next; };
node *g = nullptr;
int main() {
    node *p = new node;
    p->value = 7;
    p->next = nullptr;
    g = p;
    std::cout << g->value << "\n";
    return 0;
}
