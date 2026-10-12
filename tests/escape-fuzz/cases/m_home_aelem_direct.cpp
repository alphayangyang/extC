#include <iostream>
struct node { long long value; node *next; };
node *build() {
    node *arr[2];
    arr[0] = nullptr;
    arr[1] = nullptr;
    node *p = new node;
    p->value = 7;
    p->next = nullptr;
    arr[0] = p;
    return arr[0];
}
int main() {
    node *h = build();
    node *c = h;
    long long t = 0;
    while (c != nullptr) {
        t += c->value;
        c = c->next;
    }
    std::cout << t << "\n";
    return 0;
}
