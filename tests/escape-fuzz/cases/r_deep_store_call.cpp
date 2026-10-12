#include <iostream>
struct node { long long val; node *next; };
struct list { node *head; };
void stash(list *l, node *cell) { l->head = cell; }
int main() {
    list l; l.head = nullptr;
    {
        node *x = new node;
        x->val = 1;
        x->next = nullptr;
        stash(&l, x);
    }
    std::cout << l.head->val << "\n";
    return 0;
}
