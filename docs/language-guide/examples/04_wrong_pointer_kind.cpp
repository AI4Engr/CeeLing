//@ cpp: compiles
//@ warns: none
// The opposite mistake: every owner is a shared_ptr "to be safe", also where one owner would do.
// Nothing is wrong with the output; the cost is a reference count on every copy, and an object
// that no longer has a single owner to reason about.
#include <iostream>
#include <memory>

struct Texture {
    int id;
    explicit Texture(int i) : id(i) {}
};

struct Sprite {
    std::shared_ptr<Texture> texture;       // shared: several sprites use one texture
    std::shared_ptr<Sprite> next;           // shared by habit: only the previous sprite owns it
    Sprite(std::shared_ptr<Texture> t) : texture(std::move(t)) {}
};

int main() {
    auto rock = std::make_shared<Texture>(7);
    Sprite a(rock);
    a.next = std::make_shared<Sprite>(rock);
    std::cout << a.next->texture->id << " " << rock.use_count() << std::endl;
    return 0;
}
