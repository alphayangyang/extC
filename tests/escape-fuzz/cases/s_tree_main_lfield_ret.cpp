#include <iostream>
struct node { long long value; node *left; node *right; node *parent; };
node *mk(long long v) {
    node *n = new node;
    n->value = v;
    n->left = nullptr;
    n->right = nullptr;
    n->parent = nullptr;
    return n;
}
int main() {
    node *root = new node;
    root->value = 1;
    root->left = nullptr;
    root->right = nullptr;
    root->parent = nullptr;
    node *l = mk(2);
    node *r = mk(3);
    root->left = l;
    root->right = r;
    l->parent = root;
    r->parent = root;
    node *c = root;
    long long t = 0;
    if (c != nullptr) {
        t += c->value;
        if (c->left != nullptr) { t += c->left->value; }
        if (c->right != nullptr) { t += c->right->value; }
    }
    std::cout << t << "\n";
    return 0;
}
