#include <iostream>
struct node { long long value; node *next; };
long long burn() {
    node *arr[2];
    arr[0] = nullptr;
    arr[1] = nullptr;
    node *p = new node;
    p->value = 7;
    p->next = nullptr;
    arr[0] = p;
    node *c = arr[0];
    long long t = 0;
    while (c != nullptr) {
        t += c->value;
        c = c->next;
    }
    return t;
}
int main() {
    std::cout << burn() << "\n";
    return 0;
}
