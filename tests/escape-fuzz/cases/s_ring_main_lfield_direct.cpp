#include <iostream>
struct node { long long value; node *next; };
int main() {
    node *a = new node;
    a->value = 1;
    a->next = nullptr;
    node *b = new node;
    b->value = 2;
    b->next = nullptr;
    node *ca = new node;
    ca->value = 3;
    ca->next = nullptr;
    a->next = b;
    b->next = ca;
    ca->next = a;
    node *c = a;
    long long t = 0;
    long long s = 0;
    while (s < 6) {
        if (c != nullptr) {
            t += c->value;
            c = c->next;
        }
        s++;
    }
    std::cout << t << "\n";
    return 0;
}
