namespace hepp {

class Foo
{
public:
    Foo(int* a)
        : b(a)
    {
    }

    int* b;
};

}

int main(int argc, char** argv)
{
    int h = argc;
    hepp::Foo f(&h);
    return *f.b;
}
