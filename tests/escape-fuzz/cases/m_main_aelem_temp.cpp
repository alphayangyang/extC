#include <iostream>
struct node { long long value; node *next; };
int main() {
    node *arr[2];
    arr[0] = nullptr;
    arr[1] = nullptr;
    node *p = new node;
    p->value = 7;
    p->next = nullptr;
    node *pt = p;
    arr[0] = pt;
    node *c = arr[0];
    long long t = 0;
    while (c != nullptr) {
        t += c->value;
        c = c->next;
    }
    std::cout << t << "\n";
    return 0;
}
