#include "wasm-builder.hh"
// wasm.h has been slightly modified as to not
// declare any functions, instead it merely declares
// the types of these functions for later dlsym-ing.
#include "derivation-goal.hh"
#include "globals.hh"
#include "local-store.hh"
#include "processes.hh"
#include "wasm.h"
#include "daemon.hh"
#include "worker.hh"
#include "remote-store.hh"
#include "remote-store-connection.hh"
#include <sys/mman.h>
#include <dlfcn.h>

namespace nix {
namespace {

/*
 This is the builder of WASM derivations.
 We do not want to bundle a full WASM runtime inside
 the Nix executable/library, hence we go for runtime dynamic linking.
 This also gives the user the option of choosing the WASM runtime they prefer.

 We make use of the wasm-c-api, but unfortunately, this API is both
 crippled, _and_, the API it does give you isn't implemented by most
 runtimes beyond the most basic stuff!

 Truly a shame.

 Hence, we avoid the use of any non-basic features, and stick
 to the most trivial functionality provided by the WASM library.

 We can not even make externrefs, hence we pass host values around
 as i64s. FIXME: Make sure loaded WASM module can't mess with pointers
 by "representing" them as externrefs and rewriting the module before loading.

 FIXME: make host functions err correctly,
 asserts when the issue lies with the code or WASM runtime,
 trap when the issue lies with the derivation's code.

 FIXME: make safe wrappers for unsafe things,
 bugs here are security vulnerabilities.
*/
class WasmBuilder final : public BuilderInterface {
    public:
    ~WasmBuilder() override;
    void tryLocalBuild();
    int getChildStatus() override ;
    SingleDrvOutputs registerOutputs() override;
    void cleanupHookFinally() override {};
    void cleanupPreChildKill() override {};
    void cleanupPostChildKill() override {};
    bool cleanupDecideWhetherDiskFull() override { return false; } // FIXME: return true if file creation or resize failed. 
    void cleanupPostOutputsRegisteredModeCheck() override {};
    void cleanupPostOutputsRegisteredModeNonCheck() override {};
    void handleChildOutput(int fd, std::string_view data) override;
    void killChild() override { assert(false); };

    AutoCloseFD builderOut;
    AutoCloseFD debugOut;
    AutoCloseFD registerOutputsOut;
    std::string registerOutputsBuffer;
    Pid pid;
    DerivationGoal& goal;
    Worker& worker;
    LocalStore& store;
    std::thread daemonThread;

    WasmBuilder(DerivationGoal& goal, LocalStore& store) : goal(goal), worker(goal.worker), store(store) {}
};

struct WasmEngine {
    wasm_engine_t* engine;

    wasm_extern_as_func* wasm_extern_as_func_;
    wasm_extern_as_memory* wasm_extern_as_memory_;
    wasm_extern_delete* wasm_extern_delete_;
    wasm_func_as_extern* wasm_func_as_extern_;
    wasm_func_call* wasm_func_call_;
    wasm_func_new_with_env* wasm_func_new_with_env_;
    wasm_functype_new* wasm_functype_new_;
    wasm_instance_delete* wasm_instance_delete_;
    wasm_instance_exports* wasm_instance_exports_;
    wasm_instance_new* wasm_instance_new_;
    wasm_memory_data_size* wasm_memory_data_size_;
    wasm_memory_data* wasm_memory_data_;
    wasm_module_delete* wasm_module_delete_;
    wasm_module_exports* wasm_module_exports_;
    wasm_module_new* wasm_module_new_;
    wasm_store_delete* wasm_store_delete_;
    wasm_store_new* wasm_store_new_;
    wasm_trap_new* wasm_trap_new_;
    wasm_valtype_new* wasm_valtype_new_;
    wasm_valtype_vec_new_empty* wasm_valtype_vec_new_empty_;
    wasm_valtype_vec_new* wasm_valtype_vec_new_;
    wasmtime_error_message* wasmtime_error_message_;
    wasmtime_wat2wasm* wasmtime_wat2wasm_;
    wasm_trap_message* wasm_trap_message_;
};

void* dlsym_helper(std::string_view path, void* handle, const char* name) {
    void* symbol = dlsym(handle, name);
    if (!symbol) {
        throw SysError("%s doesn't export %s", path, name);
    } else {
        return symbol;
    }
}

const WasmEngine* get_wasm_engine() {
    static WasmEngine engine;

    debug("get_wasm_engine");

    // FIXME is this needed? Unsure if called in multi-threaded context.
    // Try removing?
    // FIXME could be made lockless for common scenario.
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    if (engine.engine) return &engine;

    debug("get_wasm_engine entered mutex to init wasm");
    auto path = settings.wasmEngine.get();
    if (path == "") {
        throw SysError("Please set wasmEngine setting to use wasm-derivations");
    }
    auto so = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!so) {
        throw SysError("Nix WASM support: dlopen(%s, ...) failed", path);
    }

    wasm_engine_new* wasm_engine_new_ = (wasm_engine_new*)dlsym_helper(path, so, "wasm_engine_new");
    engine.wasm_extern_as_func_ = (wasm_extern_as_func*)dlsym_helper(path, so, "wasm_extern_as_func");
    engine.wasm_extern_as_memory_ = (wasm_extern_as_memory*)dlsym_helper(path, so, "wasm_extern_as_memory");
    engine.wasm_extern_delete_ = (wasm_extern_delete*)dlsym_helper(path, so, "wasm_extern_delete");
    engine.wasm_func_as_extern_ = (wasm_func_as_extern*)dlsym_helper(path, so, "wasm_func_as_extern");
    engine.wasm_func_call_ = (wasm_func_call*)dlsym_helper(path, so, "wasm_func_call");
    engine.wasm_func_new_with_env_ = (wasm_func_new_with_env*)dlsym_helper(path, so, "wasm_func_new_with_env");
    engine.wasm_functype_new_ = (wasm_functype_new*)dlsym_helper(path, so, "wasm_functype_new");
    engine.wasm_instance_delete_ = (wasm_instance_delete*)dlsym_helper(path, so, "wasm_instance_delete");
    engine.wasm_instance_exports_ = (wasm_instance_exports*)dlsym_helper(path, so, "wasm_instance_exports");
    engine.wasm_instance_new_ = (wasm_instance_new*)dlsym_helper(path, so, "wasm_instance_new");
    engine.wasm_memory_data_size_ = (wasm_memory_data_size*)dlsym_helper(path, so, "wasm_memory_data_size");
    engine.wasm_memory_data_ = (wasm_memory_data*)dlsym_helper(path, so, "wasm_memory_data");
    engine.wasm_module_delete_ = (wasm_module_delete*)dlsym_helper(path, so, "wasm_module_delete");
    engine.wasm_module_exports_ = (wasm_module_exports*)dlsym_helper(path, so, "wasm_module_exports");
    engine.wasm_module_new_ = (wasm_module_new*)dlsym_helper(path, so, "wasm_module_new");
    engine.wasm_store_delete_ = (wasm_store_delete*)dlsym_helper(path, so, "wasm_store_delete");
    engine.wasm_store_new_ = (wasm_store_new*)dlsym_helper(path, so, "wasm_store_new");
    engine.wasm_trap_new_ = (wasm_trap_new*)dlsym_helper(path, so, "wasm_trap_new");
    engine.wasm_valtype_new_ = (wasm_valtype_new*)dlsym_helper(path, so, "wasm_valtype_new");
    engine.wasm_valtype_vec_new_empty_ = (wasm_valtype_vec_new_empty*)dlsym_helper(path, so, "wasm_valtype_vec_new_empty");
    engine.wasm_valtype_vec_new_ = (wasm_valtype_vec_new*)dlsym_helper(path, so, "wasm_valtype_vec_new");
    engine.wasm_trap_new_ = (wasm_trap_new*)dlsym_helper(path, so, "wasm_trap_new");
    engine.wasmtime_error_message_ = (wasmtime_error_message*)dlsym_helper(path, so, "wasmtime_error_message");
    engine.wasmtime_wat2wasm_ = (wasmtime_wat2wasm*)dlsym_helper(path, so, "wasmtime_wat2wasm");
    engine.wasm_trap_message_ = (wasm_trap_message*)dlsym_helper(path, so, "wasm_trap_message");

    auto engine_ = wasm_engine_new_();
    if (!engine_) {
        dlclose(so);
        throw SysError("Nix WASM support: Couldn't create engine");
    }

    engine.engine = engine_;

    debug("finished initialising wasm engine");

    return &engine;
}

[[ noreturn ]] void exitWith(std::exception & e) {
    printMsg(lvlError, "error when building WASM drv: %1%", e.what());
    _exit(1);
}

struct NonCopyable {
    NonCopyable() {}
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable(NonCopyable&&) {}
};

const wasm_message_t to_wasm_message(const char* str) {
    return wasm_message_t{
        .size = strlen(str)+1,
        .data = const_cast<char*>(str),
    };
}

struct HostFile : NonCopyable {
    AutoCloseFD fd;
    AutoDelete autoDelete;
    size_t size;
    bool readOnly;
};

struct HostString : NonCopyable {
    std::string string;
};

struct HostStringList : NonCopyable {
    std::vector<std::string> strings; 
};

typedef std::variant<
    std::monostate,
    HostFile,
    HostString,
    HostStringList,
    StorePath
> HostVal;

struct WasmEnv : NonCopyable {
    const WasmEngine* engine;
    wasm_store_t* store;
    wasm_instance_t* instance;
    wasm_memory_t* memory;
    int builderIn;
    int registerOutputsIn;
    std::unique_ptr<Store> nix_store;
    std::vector<HostVal> host_vals;
};

wasm_trap_t* wasm_trap(WasmEnv* env, unsigned line = __builtin_LINE()) {
    static char msg[128];
    snprintf(msg, 128, "wasm_trap called from line %i\n", line);
    auto msg_ = to_wasm_message(msg);
    return env->engine->wasm_trap_new_(env->store, &msg_);
}

typedef wasm_trap_t* (*nix_wasm_callback_t)(WasmEnv* env, const wasm_val_vec_t* args, wasm_val_vec_t* results);

wasm_func_callback_with_env_t cast_callback(nix_wasm_callback_t callback) {
    return (wasm_func_callback_with_env_t)callback;
}

static wasm_functype_t* new_functype(
    WasmEnv* env,
    std::vector<wasm_valkind_enum> params,
    std::vector<wasm_valkind_enum> results)
{
    std::vector<wasm_valtype_t*> params_;
    for (auto param : params) {
        params_.push_back(env->engine->wasm_valtype_new_(param));
    }
    std::vector<wasm_valtype_t*> results_;
    for (auto result : results) {
        results_.push_back(env->engine->wasm_valtype_new_(result));
    }
    wasm_valtype_vec_t params__, results__;
    env->engine->wasm_valtype_vec_new_(&params__, params_.size(), params_.data());
    env->engine->wasm_valtype_vec_new_(&results__, results_.size(), results_.data());
    return env->engine->wasm_functype_new_(&params__, &results__);
}

inline uint32_t arg_i32(WasmEnv* env, const wasm_val_vec_t* args, size_t idx) {
    assert(args->size > idx);
    assert(args->data[idx].kind == WASM_I32);
    uint32_t arg = args->data[idx].of.i32;
    return arg;
}

inline void result_i32(WasmEnv* env, const wasm_val_vec_t* results, size_t idx, uint32_t n) {
    assert(results->size > idx);
    assert(results->data[idx].kind == WASM_I32);
    results->data[idx].of.i32 = n;
}

template<typename T> inline std::variant<T, wasm_trap_t*> arg_enum(WasmEnv* env, const wasm_val_vec_t* args, size_t idx) {
    assert(args->size > idx);
    assert(args->data[idx].kind == WASM_I32);
    uint32_t b = args->data[idx].of.i32;
    if (b > (uint32_t)T::__end) return wasm_trap(env);
    return (T)b;
}

#define try_trap(t, r) ({ \
    if (std::holds_alternative<wasm_trap_t*>(r)) { return std::get<wasm_trap_t*>(r); }  \
    std::get<t>(r); })

inline std::variant<std::string_view, wasm_trap_t*> arg_string_view(WasmEnv* env, const wasm_val_vec_t* args, size_t idx_ptr, size_t idx_len) {
    assert(args->size > idx_ptr);
    assert(args->data[idx_ptr].kind == WASM_I32);
    uintptr_t ptr = args->data[idx_ptr].of.i32;
    assert(args->size > idx_len);
    assert(args->data[idx_len].kind == WASM_I32);
    uint32_t len = args->data[idx_len].of.i32;
    auto size = env->engine->wasm_memory_data_size_(env->memory);
    if (ptr + len > size) return wasm_trap(env);
    auto host_ptr = env->engine->wasm_memory_data_(env->memory) + ptr;
    std::string_view view(host_ptr, len);
    return std::move(view);
}

inline std::variant<HostVal*, wasm_trap_t*> arg_host_val_(WasmEnv* env, const wasm_val_vec_t* args, size_t idx) {
    assert(args->size > idx);
    assert(args->data[idx].kind == WASM_I32);
    if (args->data[idx].of.i32 >= env->host_vals.size()) return wasm_trap(env);
    HostVal* v = &env->host_vals[args->data[idx].of.i32];
    return v;
}

template<typename T> inline std::variant<T*, wasm_trap_t*> arg_host_val(WasmEnv* env, const wasm_val_vec_t* args, size_t idx) {
    auto v = arg_host_val_(env, args, idx);
    if (std::holds_alternative<wasm_trap_t*>(v)) return std::get<wasm_trap_t*>(v); 
    auto v_ = std::get<HostVal*>(v);
    if (!std::holds_alternative<T>(*v_)) {
        return wasm_trap(env);
    }
    auto& v__ = std::get<T>(*v_);
    return &v__;
}

void result_host_val(WasmEnv* env, wasm_val_vec_t* results, size_t idx, HostVal&& val) {
    assert(results->size > idx);
    assert(results->data[idx].kind == WASM_I32);
    env->host_vals.push_back(std::move(val));
    results->data[idx].of.i32 = env->host_vals.size()-1;
}

// void log(char *str, size_t str_len);
wasm_trap_t* nix_log_impl(WasmEnv* env, const wasm_val_vec_t* args, wasm_val_vec_t* results) {
    try {
        debug("nix_log");
        if (!env->instance) return wasm_trap(env);
        assert(args->size == 2);
        assert(results->size == 0);
        auto msg = try_trap(std::string_view, arg_string_view(env, args, 0, 1));
        writeFull(env->builderIn, msg);
        return NULL;
    } catch (std::exception & e) {
        exitWith(e);
    }
}

wasm_func_t* make_nix_log(WasmEnv* env) {
    auto nix_log_type = new_functype(
        env,
        {WASM_I32, WASM_I32},
        {}
    );
    auto nix_log = env->engine->wasm_func_new_with_env_(env->store, nix_log_type, cast_callback(nix_log_impl), env, NULL);
    return nix_log;
}

// void fail();
wasm_trap_t* nix_fail_impl(WasmEnv* env, const wasm_val_vec_t* args, wasm_val_vec_t* results) {
    try {
        debug("nix_fail");
        if (!env->instance) return wasm_trap(env);
        assert(args->size == 0);
        assert(results->size == 0);
        return wasm_trap(env);
    } catch (std::exception & e) {
        exitWith(e);
    }
}

wasm_func_t* make_nix_fail(WasmEnv* env) {
    auto nix_fail_type = new_functype(
        env,
        {},
        {}
    );
    auto nix_fail = env->engine->wasm_func_new_with_env_(env->store, nix_fail_type, cast_callback(nix_fail_impl), env, NULL);
    return nix_fail;
}

// fd mktemp();
wasm_trap_t* nix_mktemp_impl(WasmEnv* env, const wasm_val_vec_t* args, wasm_val_vec_t* results) {
    try {
        debug("nix_mktemp");
        if (!env->instance) return wasm_trap(env);
        assert(args->size == 0);
        assert(results->size == 1);
        auto [fd, path] = createTempFile();
        assert(fd);
        AutoDelete autoDelete(path);
        auto file = HostFile{ .fd = std::move(fd), .autoDelete = std::move(autoDelete), .size = 0, .readOnly = false};
        result_host_val(env, results, 0, std::move(file));
        return NULL;
    } catch (std::exception & e) {
        exitWith(e);
    }
}

wasm_func_t* make_nix_mktemp(WasmEnv* env) {
    auto nix_mktemp_type = new_functype(env, {}, {WASM_I32});
    auto nix_mktemp = env->engine->wasm_func_new_with_env_(env->store, nix_mktemp_type, cast_callback(nix_mktemp_impl), env, NULL);
    return nix_mktemp;
}

// void resize(fd, i32);
wasm_trap_t* nix_resize_impl(WasmEnv* env, const wasm_val_vec_t* args, wasm_val_vec_t* results) {
    try {
        debug("nix_resize");
        if (!env->instance) return wasm_trap(env);
        assert(args->size == 2);
        assert(results->size == 0);
        auto& file = *try_trap(HostFile*, arg_host_val<HostFile>(env, args, 0)); 
        auto size = arg_i32(env, args, 1);
        if (ftruncate(file.fd.get(), size) == -1) {
            return wasm_trap(env);
        }
        file.size = args->data[1].of.i32;
        return NULL;
    } catch (std::exception & e) {
        exitWith(e);
    }
}

wasm_func_t* make_nix_resize(WasmEnv* env) {
    auto nix_resize_type = new_functype(env, {WASM_I32, WASM_I32}, {});
    auto nix_resize = env->engine->wasm_func_new_with_env_(env->store, nix_resize_type, cast_callback(nix_resize_impl), env, NULL);
    return nix_resize;
}

// i32 size(fd);
wasm_trap_t* nix_size_impl(WasmEnv* env, const wasm_val_vec_t* args, wasm_val_vec_t* results) {
    try {
        debug("nix_size");
        if (!env->instance) return wasm_trap(env);
        assert(args->size == 1);
        assert(results->size == 1);
        auto& file = *try_trap(HostFile*, arg_host_val<HostFile>(env, args, 0)); 
        result_i32(env, results, 0, file.size);
        return NULL;
    } catch (std::exception & e) {
        exitWith(e);
    }
}

wasm_func_t* make_nix_size(WasmEnv* env) {
    auto nix_size_type = new_functype(env, {WASM_I32},{WASM_I32});
    auto nix_size = env->engine->wasm_func_new_with_env_(env->store, nix_size_type, cast_callback(nix_size_impl), env, NULL);
    return nix_size;
}

// void close(fd);
wasm_trap_t* nix_close_impl(WasmEnv* env, const wasm_val_vec_t* args, wasm_val_vec_t* results) {
    try {
        debug("nix_close");
        if (!env->instance) return wasm_trap(env);
        assert(args->size == 1);
        assert(results->size == 0);
        auto& file_ = *try_trap(HostVal*, arg_host_val_(env, args, 0)); 
        if (!std::holds_alternative<HostFile>(file_)) {
            return wasm_trap(env);
        }
        file_ = std::monostate {};
        return NULL;
    } catch (std::exception & e) {
        exitWith(e);
    }
}

wasm_func_t* make_nix_close(WasmEnv* env) {
    auto nix_close_type = new_functype(env,{WASM_I32},{});
    auto nix_close = env->engine->wasm_func_new_with_env_(env->store, nix_close_type, cast_callback(nix_close_impl), env, NULL);
    return nix_close;
}

wasm_trap_t* nix_read_impl(WasmEnv* env, const wasm_val_vec_t* args, wasm_val_vec_t* results) {
    try {
        debug("nix_read");
        if (!env->instance) return wasm_trap(env);
        assert(args->size == 4);
        assert(results->size == 0);
        auto& file = *try_trap(HostFile*, arg_host_val<HostFile>(env, args, 0)); 
        auto view = try_trap(std::string_view, arg_string_view(env, args, 1, 2));
        auto offset = arg_i32(env, args, 3);
        if (offset + view.size() >= file.size) return wasm_trap(env); // out of bounds
        readFullAt(file.fd.get(), (char*)view.data(), view.size(), offset);
        return NULL;
    } catch (std::exception & e) {
        exitWith(e);
    }
}

wasm_func_t* make_nix_read(WasmEnv* env) {
    auto nix_read_type = new_functype(env,{WASM_I32,WASM_I32,WASM_I32,WASM_I32},{});
    auto nix_read = env->engine->wasm_func_new_with_env_(env->store, nix_read_type, cast_callback(nix_read_impl), env, NULL);
    return nix_read;
}

wasm_trap_t* nix_write_impl(WasmEnv* env, const wasm_val_vec_t* args, wasm_val_vec_t* results) {
    try {
        debug("nix_write");
        if (!env->instance) return wasm_trap(env);
        assert(args->size == 4);
        assert(results->size == 0);
        auto& file = *try_trap(HostFile*, arg_host_val<HostFile>(env, args, 0)); 
        if (file.readOnly) return wasm_trap(env);
        auto view = try_trap(std::string_view, arg_string_view(env, args, 1, 2));
        auto offset = arg_i32(env, args, 3);
        if (offset > file.size) return wasm_trap(env); // out of bounds
        file.size = std::max(file.size, offset + view.size()); // resize implicitly
        writeFullAt(file.fd.get(), view.data(), view.size(), offset);
        return NULL;
    } catch (std::exception & e) {
        exitWith(e);
    }
}

wasm_func_t* make_nix_write(WasmEnv* env) {
    auto nix_write_type = new_functype(env, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},{});
    auto nix_write = env->engine->wasm_func_new_with_env_(env->store, nix_write_type, cast_callback(nix_write_impl), env, NULL);
    return nix_write;
}

enum struct DumpMethod { Flat, Recursive, __end, };

FileSerialisationMethod unDumpMethod(DumpMethod dumpMethod) {
    switch (dumpMethod) {
        case DumpMethod::Flat:
            return FileSerialisationMethod::Flat;
        case DumpMethod::Recursive:
            return FileSerialisationMethod::Recursive;
        case DumpMethod::__end:
            assert(false);
    }
};

enum struct HashMethod { Text, Flat, Recursive, Git, __end, };

ContentAddressMethod unHashMethod(HashMethod hashMethod) {
    switch (hashMethod) {
        case HashMethod::Flat:
            return FileIngestionMethod::Flat;
        case HashMethod::Recursive:
            return FileIngestionMethod::Recursive;
        case HashMethod::Text:
            return TextIngestionMethod {};
        case HashMethod::Git:
            return FileIngestionMethod::Git;
        case HashMethod::__end:
            assert(false);
    }
};

enum struct HashAlg { MD5, SHA1, SHA256, SHA512, __end };

HashAlgorithm unHashAlg(HashAlg hashAlg) {
    switch (hashAlg) {
        case HashAlg::MD5:
            return HashAlgorithm::MD5;
        case HashAlg::SHA1:
            return HashAlgorithm::SHA1;
        case HashAlg::SHA256:
            return HashAlgorithm::SHA256;
        case HashAlg::SHA512:
            return HashAlgorithm::SHA512;
        case HashAlg::__end:
            assert(false);
    }
};

class DumbSource : public Source {
    public:

    int fd;
    size_t size;
    off_t offset = 0;
    size_t read(char* data, size_t len) {
        if (offset >= size) throw EndOfFile("EOF");
        auto res = pread(fd, data, len, offset);
        if (res == -1) {
            if (errno == EINTR) return read(data, len);
            else throw SysError("reading from file");
        } else if (res == 0) {
            throw EndOfFile("EOF");
        } else {
            offset += res;
            return res;
        }
    }

    explicit DumbSource(int fd, size_t size) : fd(fd), size(size) {}
};

// FIXME: don't take FD, let builder write streamingly
// FIXME: maybe worker.markContentsGood?
wasm_trap_t* nix_add_dumb_impl(WasmEnv* env, const wasm_val_vec_t* args, wasm_val_vec_t* results) {
    try {
        debug("nix_add_dumb");
        if (!env->instance) return wasm_trap(env);
        assert(args->size == 6);
        assert(results->size == 1);
        auto name = try_trap(std::string_view, arg_string_view(env, args, 0, 1));
        for (char c : name) {
            if (c == '\0') return wasm_trap(env);
        }
        DumpMethod dumpMethod = try_trap(DumpMethod, arg_enum<DumpMethod>(env, args, 2));
        HashMethod hashMethod = try_trap(HashMethod, arg_enum<HashMethod>(env, args, 3));
        HashAlg hashAlg = try_trap(HashAlg, arg_enum<HashAlg>(env, args, 4));
        auto& file = *try_trap(HostFile*, arg_host_val<HostFile>(env, args, 5)); 
        DumbSource dump(file.fd.get(), file.size);
        auto path = env->nix_store->addToStoreFromDump(dump, name, unDumpMethod(dumpMethod), unHashMethod(hashMethod), unHashAlg(hashAlg));
        result_host_val(env, results, 0, std::move(path));
        return NULL;
    } catch (std::exception & e) {
        exitWith(e);
    }
}


wasm_func_t* make_nix_add_dumb(WasmEnv* env) {
    auto nix_add_dumb_type = new_functype(env, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32},{WASM_I32});
    auto nix_add_dumb = env->engine->wasm_func_new_with_env_(env->store, nix_add_dumb_type, cast_callback(nix_add_dumb_impl), env, NULL);
    return nix_add_dumb;
}

wasm_trap_t* nix_output_impl(WasmEnv* env, const wasm_val_vec_t* args, wasm_val_vec_t* results) {
    try {
        debug("nix_add_dumb");
        if (!env->instance) return wasm_trap(env);
        assert(args->size == 3);
        assert(results->size == 0);
        auto output_name = try_trap(std::string_view, arg_string_view(env, args, 0, 1));
        auto& path = *try_trap(StorePath*, arg_host_val<StorePath>(env, args, 2)); 
        writeFull(env->registerOutputsIn, path.to_string());
        return NULL;
    } catch (std::exception & e) {
        exitWith(e);
    }
}


wasm_func_t* make_nix_output(WasmEnv* env) {
    auto ty = new_functype(env, {WASM_I32,WASM_I32,WASM_I32},{});
    return env->engine->wasm_func_new_with_env_(env->store, ty, cast_callback(nix_output_impl), env, NULL);
}

struct PipeRemoteStoreConfig : public virtual RemoteStoreConfig {
    PipeRemoteStoreConfig(const Params & params)
        : StoreConfig(params)
        , RemoteStoreConfig(params)
    {
    }

    const std::string name() override {
        return "";
    }
};

class PipeRemoteStore : public virtual PipeRemoteStoreConfig, public virtual RemoteStore {
    public:

    struct Connection : RemoteStore::Connection {
        void closeWrite() override {}
    };

    ref<Connection> conn;

    PipeRemoteStore(ref<Connection> conn, const Params & params) : StoreConfig(params), RemoteStoreConfig(params), PipeRemoteStoreConfig(params), Store(params), RemoteStore(params), conn(conn) {}

    ref<RemoteStore::Connection> openConnection() override {
        return conn;
    }


    std::string getUri() override {
        return "";
    }

    std::optional<std::string> getBuildLogExact(const StorePath & path) override
    { unsupported("getBuildLogExact"); }
};

template<typename Func>
pid_t startProcessMoveOnly(Func f) {
    return startProcess([&]{
        f();
    });
}

void WasmBuilder::tryLocalBuild() {
    auto engine = get_wasm_engine();

    Pipe log_pipe;
    log_pipe.create();
    Pipe debug_pipe;
    debug_pipe.create();
    Pipe register_outputs_pipe;
    register_outputs_pipe.create();

    builderOut = std::move(log_pipe.readSide);
    debugOut = std::move(debug_pipe.readSide);
    registerOutputsOut = std::move(register_outputs_pipe.readSide);
    AutoCloseFD builderIn = std::move(log_pipe.writeSide);
    AutoCloseFD debugIn = std::move(debug_pipe.writeSide);
    AutoCloseFD registerOutputsIn = std::move(register_outputs_pipe.writeSide);

    Pipe to_store;
    to_store.create();
    AutoCloseFD to_store_write = std::move(to_store.writeSide);
    AutoCloseFD to_store_read = std::move(to_store.readSide);
    Pipe from_store;
    from_store.create();
    AutoCloseFD from_store_write = std::move(from_store.writeSide);
    AutoCloseFD from_store_read = std::move(from_store.readSide);

    {
        ref<Store> store(worker.store.shared_from_this());

        daemonThread = std::thread([store = std::move(store), from_store_write = std::move(from_store_write), to_store_read = std::move(to_store_read)]() {
            debug("inside daemonThread");
            FdSource source = to_store_read.get();
            FdSink sink = from_store_write.get();
            daemon::processConnection(store, source, sink, NotTrusted, daemon::Recursive);
            debug("daemon done");
        });
    }

    debug("will start process now");

    std::string rootDir = *store.rootDir.get();

    // We use another process rather than a thread because:
    // - htop etc. will show it as a "job" properly
    // - it can be trivially cancelled (vs. doing a signal and cleaning it up somehow)
    // - we don't have to worry about memory leaks
    pid = startProcessMoveOnly([
        this,
        &engine,
        rootDir = std::move(rootDir),
        debugIn = std::move(debugIn),
        builderIn = std::move(builderIn),
        to_store_write = std::move(to_store_write),
        from_store_read = std::move(from_store_read),
        registerOutputsIn = std::move(registerOutputsIn)
    ]() mutable {
        writeFull(debugIn.get(), "djuwajduawjdu\n");
        // to ensure we don't use any other FDs by accident
        closeMostFDs({builderIn.get(), debugIn.get(), to_store_write.get(), from_store_read.get(), registerOutputsIn.get()});
        dup2(debugIn.get(), STDOUT_FILENO);
        dup2(debugIn.get(), STDERR_FILENO);
        debug("inside process");
        try {
            debug("inside try");
            writeLine(builderIn.get(), "build started");

            auto conn = make_ref<PipeRemoteStore::Connection>();
            conn->from.fd = from_store_read.get();
            conn->to.fd = to_store_write.get();
            conn->startTime = std::chrono::steady_clock::now();

            Store::Params params;
            auto nix_store = std::make_unique<PipeRemoteStore>(conn, std::move(params));
            nix_store->connect();

            auto builderPath = rootDir + "/" + goal.drv->builder;
            AutoCloseFD builder = open(builderPath.c_str(), O_RDONLY);
            if (!builder) {
                throw SysError("open on builder '%s' failed", builderPath);
            }
            debug("opened builder");
            struct stat fileInfo;
            if (fstat(builder.get(), &fileInfo) == -1) {
                throw SysError("fstat on builder '%s' failed", goal.drv->builder);
            }
            void* data = mmap(NULL, fileInfo.st_size, PROT_READ, MAP_PRIVATE, builder.get(), 0);
            if (data == MAP_FAILED) {
                throw SysError("mmap on builder '%s' failed", goal.drv->builder);
            }
            debug("mmapped contents");

            auto store = engine->wasm_store_new_(engine->engine);
            wasm_byte_vec_t bytes = {
                .size = (size_t)fileInfo.st_size,
                .data = (wasm_byte_t*)data,
            };
            debug("made store");

            WasmEnv* env = std::make_unique<WasmEnv>(WasmEnv{
                .engine = engine,
                .store = store,
                .instance = NULL,
                .builderIn = builderIn.get(),
                .registerOutputsIn = registerOutputsIn.get(),
                .nix_store = std::move(nix_store),
            }).release();

            wasm_extern_t* externs[] = {
                engine->wasm_func_as_extern_(make_nix_log(env)),
                engine->wasm_func_as_extern_(make_nix_fail(env)),
                engine->wasm_func_as_extern_(make_nix_mktemp(env)),
                // engine->wasm_func_as_extern_(make_nix_open(env)),
                engine->wasm_func_as_extern_(make_nix_close(env)),
                engine->wasm_func_as_extern_(make_nix_read(env)),
                engine->wasm_func_as_extern_(make_nix_write(env)),
                engine->wasm_func_as_extern_(make_nix_size(env)),
                engine->wasm_func_as_extern_(make_nix_resize(env)),
                engine->wasm_func_as_extern_(make_nix_add_dumb(env)),
                engine->wasm_func_as_extern_(make_nix_output(env)), 
            };
            debug("made externs[]");
            wasm_extern_vec_t imports = {
                .size = sizeof(externs)/sizeof(externs[0]),
                .data = externs,
            };
            debug("size of imports: %d", imports.size);

            wasm_module_t* module;
            if (bytes.size > 0 && bytes.data[0] == '(') {
                debug("WASM module is textual");
                wasm_byte_vec_t out;
                wasmtime_error_t* error = engine->wasmtime_wat2wasm_(bytes.data, bytes.size, &out);
                if (error) {
                    wasm_name_t msg;
                    engine->wasmtime_error_message_(error, &msg);
                    std::string_view msg_(msg.data, msg.size);
                    throw BuildError("failed to parse WAT: %s", msg_);
                }
                module = engine->wasm_module_new_(store, &out);
            } else {
                debug("WASM module is binary");
                module = engine->wasm_module_new_(store, &bytes);
            }
            if (!module) {
                throw BuildError("couldn't compile WASM module");
            }
            debug("loaded module");
            auto instance = engine->wasm_instance_new_(store, module, &imports, NULL);
            if (!instance) {
                throw BuildError("couldn't instantiate WASM module");
            }
            debug("instance made");

            wasm_extern_vec_t exports;
            engine->wasm_instance_exports_(instance, &exports);
            if (exports.size != 2) {
                throw BuildError("WASM module must have exactly two exports");
            }
            auto memory = engine->wasm_extern_as_memory_(exports.data[0]);
            if (!memory) {
                throw BuildError("export 0 of WASM module isn't memory");
            }
            auto build_derivation = engine->wasm_extern_as_func_(exports.data[1]);
            if (!build_derivation) {
                throw BuildError("export 1 of WASM module isn't a function");
            }

            // init env completely
            env->instance = instance;
            env->memory = memory;

            wasm_val_vec_t args = WASM_EMPTY_VEC;
            wasm_val_vec_t results = WASM_EMPTY_VEC;
            if (auto trap = engine->wasm_func_call_(build_derivation, &args, &results)) {
                wasm_message_t msg;
                engine->wasm_trap_message_(trap, &msg);
                std::string_view msg_(msg.data, msg.size);
                throw BuildError("function execution trapped: %s", msg_);
            }

            writeLine(builderIn.get(), "build done");
            { 
                auto _env = std::move(*env);
            }
            _exit(0);
        } catch (std::exception & e) {
            exitWith(e);
        }
    });
    debug("started process, pid is %i", (int)pid);

    pid.setSeparatePG(true);
    worker.childStarted(goal.shared_from_this(), {builderOut.get(), debugOut.get(), registerOutputsOut.get()}, true, true);
}

SingleDrvOutputs WasmBuilder::registerOutputs() {
    debug("registerOutputs() called");

    StorePath path(registerOutputsBuffer);
    auto hash = hashDerivationModulo(store, *goal.drv, true);
    // FIXME: support other output names
    auto r = Realisation {
        .id = DrvOutput {
            hash.hashes.at("out"),
            "out"
        },
        .outPath = path
    };
    goal.signRealisation(r);
    store.registerDrvOutput(r);

    return goal.assertPathValidity();
}


std::string prepend_every_line(std::string prepended, std::string_view sv) {
    std::string result;
    bool newLine = true;

    for (char ch : sv) {
        if (newLine) {
            result += prepended;
            newLine = false;
        }

        result += ch;

        if (ch == '\n') {
            newLine = true;
        }
    }

    return result;
}

void WasmBuilder::handleChildOutput(int fd, std::string_view data) {
    debug("handleChildOutput");
    if (fd == builderOut.get()) goal.writeToLog(data);
    else if (fd == debugOut.get()) {
        // FIXME: use structured data
        writeFull(STDOUT_FILENO, prepend_every_line("inside wasm builder: ", data));
    }
    else if (fd == registerOutputsOut.get()) registerOutputsBuffer.append(data);
    else assert(false);
}

int WasmBuilder::getChildStatus()
{
    return pid.kill();
}

WasmBuilder::~WasmBuilder() {
    if (daemonThread.joinable()) {
        daemonThread.join();
    }
}

}

std::unique_ptr<BuilderInterface> make_wasm_builder(DerivationGoal& goal) {
    auto store = dynamic_cast<LocalStore*>(&goal.worker.store);
    assert(store);
    if (!(goal.derivationType->isCA() && goal.derivationType->isSandboxed())) {
        throw Error("derivation '%s' must be CA and sandboxed if it is to use WASM",
            store->printStorePath(goal.drvPath));
    }
    auto builder = std::make_unique<WasmBuilder>(goal, *store);
    builder->tryLocalBuild();
    return builder;
}
}
