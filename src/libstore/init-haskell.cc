extern "C" void hs_init(int *argc, char **argv[]);
extern "C" void hs_exit(void);

struct InitHaskell {
    InitHaskell() {
        hs_init(nullptr, nullptr);
    }
    ~InitHaskell() {
        hs_exit();
    }
};

static InitHaskell _init{};
