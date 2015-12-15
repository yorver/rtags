template <typename T>
T *create()
{
    T *t = new T;
    t->func();
    return t;
}

struct Foo
{
    Foo() {}
    void func() {}
};

struct Bar
{
    void func() {}
};

void foo()
{
    create<Foo>();
    create<Bar>();
}
