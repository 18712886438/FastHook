#include "jni.h"
#include <string.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <bits/sysconf.h>

#include "fast_hook_manager.h"

void fake_jit_update_options(void* handle) {
    //do nothing
    LOGI("android q: art request update compiler options");
    return;
}

static inline void InitJit() {
    int max_units = 0;
    void *jit_lib = NULL;
    void *art_lib = NULL;

    if(pointer_size_ == kPointerSize32) {
        if (SDK_INT >= kAndroidQ) {
            jit_lib = fake_dlopen("/apex/com.android.runtime/lib/libart-compiler.so", RTLD_NOW);
            art_lib = fake_dlopen("/apex/com.android.runtime/lib/libart.so", RTLD_NOW);
        } else {
            jit_lib = fake_dlopen("/system/lib/libart-compiler.so", RTLD_NOW);
            art_lib = fake_dlopen("/system/lib/libart.so", RTLD_NOW);
        }
    }else {
        if (SDK_INT >= kAndroidQ) {
            jit_lib = fake_dlopen("/apex/com.android.runtime/lib64/libart-compiler.so", RTLD_NOW);
            art_lib = fake_dlopen("/apex/com.android.runtime/lib64/libart.so", RTLD_NOW);
        } else {
            jit_lib = fake_dlopen("/system/lib64/libart-compiler.so", RTLD_NOW);
            art_lib = fake_dlopen("/system/lib64/libart.so", RTLD_NOW);
        }
    }

    art_quick_to_interpreter_bridge_ = fake_dlsym(art_lib, "art_quick_to_interpreter_bridge");

    if (SDK_INT >= kAndroidN) {
        profileSaver_ForceProcessProfiles = (void (*)()) fake_dlsym(jit_lib, "_ZN3art12ProfileSaver20ForceProcessProfilesEv");
    }

    if (SDK_INT < kAndroidQ) {
        jit_compile_method_ = (bool (*)(void *, void *, void *, bool)) fake_dlsym(jit_lib, "jit_compile_method");
    } else {
        jit_compile_method_Q_ = (bool (*)(void *, void *, void *, bool, bool)) fake_dlsym(jit_lib, "jit_compile_method");
        origin_jit_update_options = (void (**)(void *))(fake_dlsym(art_lib, "_ZN3art3jit3Jit19jit_update_options_E"));
    }

    if (origin_jit_update_options != NULL) {
        *origin_jit_update_options = fake_jit_update_options;
    }

    jit_load_ = (void* (*)(bool*))(fake_dlsym(jit_lib, "jit_load"));
    bool will_generate_debug_symbols = false;

    if (jit_load_ == NULL) {
        jit_compiler_handle_ = *art_jit_compiler_handle_ = fake_dlsym(art_lib, "_ZN3art3jit3Jit20jit_compiler_handle_E");
    } else {
        jit_compiler_handle_ = (jit_load_)(&will_generate_debug_symbols);
        art_jit_compiler_handle_ = fake_dlsym(art_lib, "_ZN3art3jit3Jit20jit_compiler_handle_E");
    }

}

static inline void *EntryPointToCodePoint(void *entry_point) {
    long code = (long)entry_point;
    code &= ~0x1;

    return (void *)code;
}

static inline void *CodePointToEntryPoint(void *code_point) {
    long entry = (long)code_point;
    entry &= ~0x1;
    entry += 1;

    return (void *)entry;
}

static inline uint16_t GetArtMethodHotnessCount(void *art_method) {
    return ReadInt16((unsigned char *)art_method + kArtMethodHotnessCountOffset);
}

static inline void *GetArtMethodEntryPoint(void *art_method) {
    return (void *)ReadPointer((unsigned char *)art_method + kArtMethodQuickCodeOffset);
}

static inline void *GetArtMethodProfilingInfo(void *art_method) {
    return (void *)ReadPointer((unsigned char *) art_method + kArtMethodProfilingOffset);
}

static inline void *GetProfilingSaveEntryPoint(void *profiling) {
    return (void *)ReadPointer((unsigned char *) profiling + kProfilingSavedEntryPointOffset);
}

static inline bool GetProfilingCompileState(void *profiling) {
    return (bool)ReadInt8((unsigned char *) profiling + kProfilingCompileStateOffset);
}

static inline void SetProfilingSaveEntryPoint(void *profiling, void *entry_point) {
    memcpy((unsigned char *) profiling + kProfilingSavedEntryPointOffset, &entry_point, pointer_size_);
}

static inline void AddArtMethodAccessFlag(void *art_method, uint32_t flag) {
    uint32_t new_flag = ReadInt32((unsigned char *)art_method + kArtMethodAccessFlagsOffset);
    new_flag |= flag;

    memcpy((unsigned char *) art_method + kArtMethodAccessFlagsOffset,&new_flag,4);
}

static inline void disableFastInterpreterForQ(void *art_method) {
    if (SDK_INT < kAndroidQ) {
        return;
    }
    uint32_t new_flag = ReadInt32((unsigned char *)art_method + kArtMethodAccessFlagsOffset);
    new_flag &= ~kAccFastInterpreterToInterpreterInvoke;

    memcpy((unsigned char *) art_method + kArtMethodAccessFlagsOffset,&new_flag,4);
}

static inline void *CurrentThread() {
    return __get_tls()[kTLSSlotArtThreadSelf];
}

static inline void *CreatTrampoline(int type) {
    void *trampoline = NULL;
    uint32_t size = 0;
    switch(type) {
        case kHookTrampoline:
            size = RoundUp(sizeof(hook_trampoline_),pointer_size_);
            break;
        case kTargetTrampoline:
            size = RoundUp(sizeof(target_trampoline_),pointer_size_);
            break;
    }

    if(size == 0) {
        return 0;
    }

    trampoline = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);

    switch(type) {
        case kHookTrampoline:
            memcpy(trampoline, hook_trampoline_, sizeof(hook_trampoline_));
            break;
        case kTargetTrampoline:
            memcpy(trampoline, target_trampoline_, sizeof(target_trampoline_));
            break;
    }

    LOGI("Type:%d Trampoline:%p",type,trampoline);

    if(trampoline == MAP_FAILED) {
        return 0;
    }

    return trampoline;
}

void SignalHandle(int signal, siginfo_t *info, void *reserved) {
    ucontext_t* context = (ucontext_t*)reserved;
    void *addr = (void *)context->uc_mcontext.fault_address;
    LOGI("Signal%d FaultAddress:%p TargetCode:%p",signal,addr,sigaction_info_->addr);

    if(sigaction_info_->addr == addr) {
        void *target_code = sigaction_info_->addr;
        int len = sigaction_info_->len;

        long page_size = sysconf(_SC_PAGESIZE);
        unsigned alignment = (unsigned)((unsigned long long)target_code % page_size);
        int ret = mprotect((void *) (target_code - alignment), (size_t) (alignment + len),
                           PROT_READ | PROT_WRITE | PROT_EXEC);
        LOGI("Mprotect:%d Pagesize:%d Alignment:%d",ret,page_size,alignment);
    }
}

void static inline InitTrampoline(int version) {
#if defined(__arm__)
    switch(version) {
        case kAndroidQ:
        case kAndroidP:
            hook_trampoline_[6] = 0x18;
            break;
        case kAndroidOMR1:
        case kAndroidO:
            hook_trampoline_[6] = 0x1c;
            break;
        case kAndroidNMR1:
        case kAndroidN:
            hook_trampoline_[6] = 0x20;
            break;
        case kAndroidM:
            hook_trampoline_[6] = 0x24;
            break;
        case kAndroidLMR1:
            hook_trampoline_[6] = 0x2c;
            break;
        case kAndroidL:
            hook_trampoline_[6] = 0x28;
            break;
    }
#elif defined(__aarch64__)
    switch(version) {
        case kAndroidQ:
        case kAndroidP:
            hook_trampoline_[5] = 0x10;
            break;
        case kAndroidOMR1:
        case kAndroidO:
            hook_trampoline_[5] = 0x14;
            break;
        case kAndroidNMR1:
        case kAndroidN:
            hook_trampoline_[5] = 0x18;
            break;
        case kAndroidM:
            hook_trampoline_[5] = 0x18;
            break;
        case kAndroidLMR1:
            hook_trampoline_[5] = 0x1c;
            break;
        case kAndroidL:
            hook_trampoline_[5] = 0x14;
            break;
    }
#endif
}

jint Init(JNIEnv *env, jclass clazz, jint version) {
    int ret = 0;

    SDK_INT = version;
    pointer_size_ = sizeof(long);

    switch(SDK_INT) {
        case kAndroidQ:
            kTLSSlotArtThreadSelf = 7;
            kAccCompileDontBother = 0x02000000;
            kArtMethodHotnessCountOffset = 18;
            kArtMethodProfilingOffset = RoundUp(4 * 4 +  2* 2,pointer_size_);
            kArtMethodQuickCodeOffset = RoundUp(4 * 4 +  2* 2,pointer_size_) + pointer_size_;
            kProfilingCompileStateOffset = (pointer_size_ * 2) + 4 + 2;
            kProfilingSavedEntryPointOffset = pointer_size_;
            kHotMethodThreshold = 10000;
            kHotMethodMaxCount = 50;
            break;
        case kAndroidP:
            kTLSSlotArtThreadSelf = 7;
            kAccCompileDontBother = 0x02000000;
            kArtMethodHotnessCountOffset = 18;
            kArtMethodProfilingOffset = RoundUp(4 * 4 +  2* 2,pointer_size_);
            kArtMethodQuickCodeOffset = RoundUp(4 * 4 +  2* 2,pointer_size_) + pointer_size_;
            kProfilingCompileStateOffset = 4 + pointer_size_;
            kProfilingSavedEntryPointOffset = 4 + pointer_size_ + sizeof(bool) * 2 + 2;
            kHotMethodThreshold = 10000;
            kHotMethodMaxCount = 50;
            break;
        case kAndroidOMR1:
            kAccCompileDontBother = 0x02000000;
        case kAndroidO:
            kTLSSlotArtThreadSelf = 7;
            kArtMethodHotnessCountOffset = 18;
            kArtMethodProfilingOffset = RoundUp(4 * 4 +  2* 2,pointer_size_) + pointer_size_;
            kArtMethodQuickCodeOffset = RoundUp(4 * 4 +  2* 2,pointer_size_) + pointer_size_ * 2;
            kProfilingCompileStateOffset = 4 + pointer_size_;
            kProfilingSavedEntryPointOffset = 4 + pointer_size_ + sizeof(bool) * 2 + 2;
            kHotMethodThreshold = 10000;
            kHotMethodMaxCount = 50;
            break;
        case kAndroidNMR1:
        case kAndroidN:
            kTLSSlotArtThreadSelf = 7;
            kAccCompileDontBother = 0x01000000;
            kArtMethodHotnessCountOffset = 18;
            kArtMethodProfilingOffset = RoundUp(4 * 4 +  2* 2,pointer_size_) + pointer_size_ * 2;
            kArtMethodQuickCodeOffset = RoundUp(4 * 4 +  2* 2,pointer_size_) + pointer_size_ * 3;
            kProfilingCompileStateOffset = 4 + pointer_size_;
            kProfilingSavedEntryPointOffset = 4 + pointer_size_ + sizeof(bool) * 2 + 2;
            kHotMethodThreshold = 10000;
            kHotMethodMaxCount = 50;
            break;
        case kAndroidM:
	        kArtMethodAccessFlagsOffset = 4 * 3;
            kArtMethodInterpreterEntryOffset = RoundUp(4 * 7,pointer_size_);
            kArtMethodQuickCodeOffset = RoundUp(4 * 7,pointer_size_) + pointer_size_ * 2;
            break;
        case kAndroidLMR1:
	        kArtMethodAccessFlagsOffset = 4 * 2 + 4 * 3;
            kArtMethodInterpreterEntryOffset = RoundUp(4 * 2 + 4 * 7,pointer_size_);
            kArtMethodQuickCodeOffset = RoundUp(4 * 2 + 4 * 7,pointer_size_) + pointer_size_ * 2;
            break;
        case kAndroidL:
	        kArtMethodAccessFlagsOffset = 4 * 2 + 4 * 4 + 8 * 4;
            kArtMethodInterpreterEntryOffset = 4 * 2 + 4 * 4;
            kArtMethodQuickCodeOffset = 4 * 2 + 4 * 4 + 8 * 2;
            break;
    }

    InitTrampoline(SDK_INT);

    sigaction_info_ = (struct SigactionInfo *)malloc(sizeof(struct SigactionInfo));
    sigaction_info_->addr = NULL;
    sigaction_info_->len = 0;

    if(kTLSSlotArtThreadSelf > 0) {
        InitJit();
    }

    return ret;
}

void DisableJITInline(JNIEnv *env, jclass clazz) {
    int max_units = 0;

    if (profileSaver_ForceProcessProfiles != NULL) {
        profileSaver_ForceProcessProfiles();
    }

    void *art_compiler_options = (void *)ReadPointer((unsigned char *)(*art_jit_compiler_handle_) + pointer_size_);

    memcpy((unsigned char *)art_compiler_options + 6 * pointer_size_,&max_units,pointer_size_);
    LOGI("DisableJITInline %d",ReadInt32((unsigned char *)art_compiler_options + 6 * pointer_size_));
}

long GetMethodEntryPoint(JNIEnv *env, jclass clazz, jobject method) {
    void *art_method = (void *)(*env)->FromReflectedMethod(env, method);

    long entry_point = ReadPointer((unsigned char *)art_method + kArtMethodQuickCodeOffset);

    return entry_point;
}

bool CompileMethod(JNIEnv *env, jclass clazz, jobject method) {
    bool ret = false;

    void *art_method = (void *)(*env)->FromReflectedMethod(env, method);
    void *thread = CurrentThread();
    int old_flag_and_state = ReadInt32(thread);

    if (SDK_INT >= kAndroidQ) {
        ret = jit_compile_method_Q_(jit_compiler_handle_, art_method, thread, false, false);
    } else {
        ret = jit_compile_method_(jit_compiler_handle_, art_method, thread, false);
    }

    memcpy(thread,&old_flag_and_state,4);
    LOGI("CompileMethod:%d",ret);

    return ret;
}

bool IsCompiled(JNIEnv *env, jclass clazz, jobject method) {
    bool ret = false;

    void *art_method = (void *)(*env)->FromReflectedMethod(env, method);
    void *method_entry = (void *)ReadPointer((unsigned char *)art_method + kArtMethodQuickCodeOffset);
    int hotness_count = GetArtMethodHotnessCount(art_method);

    if(method_entry != art_quick_to_interpreter_bridge_)
        ret = true;

    if(kTLSSlotArtThreadSelf > 0) {
        if(!ret && hotness_count >= kHotMethodThreshold)
            ret = true;
    }

    LOGI("IsCompiled:%d",ret);
    return ret;
}

bool IsNativeMethod(JNIEnv *env, jclass clazz, jobject method) {
    void *art_method = (void *)(*env)->FromReflectedMethod(env, method);

    uint32_t flag = ReadPointer((unsigned char *)art_method + kArtMethodAccessFlagsOffset);
    if(flag & kAccNative) {
        return true;
    }

    return false;
}

void SetNativeMethod(JNIEnv *env, jclass clazz, jobject method) {
    void *art_method = (void *)(*env)->FromReflectedMethod(env, method);

    AddArtMethodAccessFlag(art_method,kAccNative);
}

int CheckJitState(JNIEnv *env, jclass clazz, jobject target_method) {
    void *art_method = (void *)(*env)->FromReflectedMethod(env, target_method);

    AddArtMethodAccessFlag(art_method, kAccCompileDontBother);

    disableFastInterpreterForQ(art_method);

    uint32_t hotness_count = GetArtMethodHotnessCount(art_method);

    LOGI("TargetMethod:%p QuickToInterpreterBridge:%p hotness_count:%hd",art_method,art_quick_to_interpreter_bridge_,hotness_count);
    if(hotness_count >= kHotMethodThreshold) {
        long entry_point = (long)GetArtMethodEntryPoint(art_method);
        if((void *)entry_point == art_quick_to_interpreter_bridge_) {
            void *profiling = GetArtMethodProfilingInfo(art_method);
            void *save_entry_point = GetProfilingSaveEntryPoint(profiling);
            if(save_entry_point) {
                return kCompile;
            }else {
                bool being_compiled = GetProfilingCompileState(profiling);
                if(being_compiled) {
                    return kCompiling;
                }else {
                    return kCompilingOrFailed;
                }
            }
        }

        return kCompile;
    }else {
        uint32_t assumed_hotness_count = hotness_count + kHotMethodMaxCount;
        if(assumed_hotness_count > kHotMethodThreshold) {
            return kCompiling;
        }
    }

    return kNone;
}

jint DoReplaceHook(JNIEnv *env, jclass clazz, jobject target_method, jobject hook_method, jobject forward_method, jboolean is_native, jobject target_record) {
    void *art_target_method = (void *)(*env)->FromReflectedMethod(env, target_method);
    void *art_hook_method = (void *)(*env)->FromReflectedMethod(env, hook_method);
    void *art_forward_method = NULL;
    if(forward_method != NULL) {
        art_forward_method = (void *)(*env)->FromReflectedMethod(env, forward_method);
    }

    void *target_entry = NULL;
    if(is_native) {
        target_entry = GetArtMethodEntryPoint(art_target_method);
    }else {
        target_entry = art_quick_to_interpreter_bridge_;
    }

    void *hook_trampoline = CreatTrampoline(kHookTrampoline);
    void *target_trampoline = CreatTrampoline(kTargetTrampoline);

#if defined(__arm__)

    int hook_trampoline_len = 3 * 4;
    int target_trampoline_len = 4 * 4;

    int hook_trampoline_target_index = 8;
    int target_trampoline_target_index = 8;
    int target_trampoline_target_entry_index = 12;

    void *new_target_entry = CodePointToEntryPoint(hook_trampoline);
    void *new_forward_entry =  CodePointToEntryPoint(target_trampoline);

#elif defined(__aarch64__)

    int hook_trampoline_len = 5 * 4;
	int target_trampoline_len = 7 * 4;

	int hook_trampoline_target_index = 12;
	int target_trampoline_target_index = 12;
	int target_trampoline_target_entry_index = 20;

	void *new_target_entry = hook_trampoline;
	void *new_forward_entry =  target_trampoline;

#endif

    memcpy((unsigned char *) hook_trampoline + hook_trampoline_target_index, &art_hook_method, pointer_size_);

    LOGI("HookTrampoline:%p HookMethod:%p",hook_trampoline,art_hook_method);
    for(int i = 0; i < hook_trampoline_len/4; i++) {
        LOGI("HookTrampoline[%d] %x %x %x %x",i,((unsigned char*)hook_trampoline)[i*4+0],((unsigned char*)hook_trampoline)[i*4+1],((unsigned char*)hook_trampoline)[i*4+2],((unsigned char*)hook_trampoline)[i*4+3]);
    }

    if(art_forward_method) {
        memcpy((unsigned char *) target_trampoline + hook_trampoline_target_index, &art_target_method, pointer_size_);
        memcpy((unsigned char *) target_trampoline + target_trampoline_target_entry_index, &target_entry, pointer_size_);

        if(kTLSSlotArtThreadSelf) {
            uint32_t hotness_count = GetArtMethodHotnessCount(art_target_method);
            LOGI("TargetTrampoline:%p TargetMethod:%p QuickToInterpreterBridge:%p",target_trampoline,art_target_method,art_quick_to_interpreter_bridge_);
            if(hotness_count >= kHotMethodThreshold) {
                void *profiling = GetArtMethodProfilingInfo(art_target_method);
                void *save_entry_point = GetProfilingSaveEntryPoint(profiling);
                if(save_entry_point) {
                    SetProfilingSaveEntryPoint(profiling,art_quick_to_interpreter_bridge_);
                }
            }

        }

        for(int i = 0; i < target_trampoline_len/4; i++) {
            LOGI("TargetTrampoline[%d] %x %x %x %x",i,((unsigned char*)target_trampoline)[i*4+0],((unsigned char*)target_trampoline)[i*4+1],((unsigned char*)target_trampoline)[i*4+2],((unsigned char*)target_trampoline)[i*4+3]);
        }
    }

    memcpy((unsigned char *) art_target_method + kArtMethodQuickCodeOffset,&new_target_entry,pointer_size_);
    LOGI("Target NewEntry:%p",ReadPointer((unsigned char *) art_target_method + kArtMethodQuickCodeOffset));
    if(art_forward_method) {
        memcpy((unsigned char *) art_forward_method + kArtMethodQuickCodeOffset,&new_forward_entry,pointer_size_);
        LOGI("Forward NewEntry:%p",ReadPointer((unsigned char *) art_forward_method + kArtMethodQuickCodeOffset));
    }

    if(kArtMethodInterpreterEntryOffset > 0) {
        if(art_forward_method) {
            memcpy((unsigned char *) art_forward_method + kArtMethodInterpreterEntryOffset,(unsigned char *) art_target_method + kArtMethodInterpreterEntryOffset,pointer_size_);
        }
        memcpy((unsigned char *) art_target_method + kArtMethodInterpreterEntryOffset,(unsigned char *) art_hook_method + kArtMethodInterpreterEntryOffset,pointer_size_);
    }

    (*env)->SetLongField(env, target_record, kHookRecordClassInfo.hook_trampoline_, (long)hook_trampoline);
    if(art_forward_method) {
        (*env)->SetLongField(env, target_record, kHookRecordClassInfo.target_trampoline_, (long)target_trampoline);
    }

    __builtin___clear_cache(hook_trampoline, hook_trampoline + hook_trampoline_len);
    if(art_forward_method) {
        __builtin___clear_cache(target_trampoline, target_trampoline + target_trampoline_len);
    }

    return 0;
}

static JNINativeMethod JniMethods[] = {

        {"init",               				  "(I)I",                                                         (void *) Init},
        {"disableJITInline",               	  "()V",                                                          (void *) DisableJITInline},
        {"getMethodEntryPoint",               "(Ljava/lang/reflect/Member;)J",                                (void *) GetMethodEntryPoint},
        {"compileMethod",            		  "(Ljava/lang/reflect/Member;)Z",                                (void *) CompileMethod},
        {"isCompiled",            		      "(Ljava/lang/reflect/Member;)Z",                               (void *) IsCompiled},
        {"isNativeMethod",                    "(Ljava/lang/reflect/Member;)Z",                                (void *) IsNativeMethod},
        {"setNativeMethod",                    "(Ljava/lang/reflect/Member;)V",                               (void *) SetNativeMethod},
        {"checkJitState",                     "(Ljava/lang/reflect/Member;)I",                               (void *) CheckJitState},
        {"doReplaceHook",                     "(Ljava/lang/reflect/Member;Ljava/lang/reflect/Member;Ljava/lang/reflect/Member;ZLpers/turing/technician/fasthook/FastHookManager$HookRecord;)I",
                (void *) DoReplaceHook}
};

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = NULL;

    if ((*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6) != JNI_OK) {
        return -1;
    }

    jvm_ = vm;

    jclass class = (*env)->FindClass(env,kClassName);

    if(class == NULL)
    {
        LOGI( "%s: Can't find class %s", __FUNCTION__, kClassName );
        return -1;
    }

    if ((*env)->RegisterNatives(env, class, JniMethods,
                                sizeof(JniMethods) / sizeof(JniMethods[0])) != JNI_OK) {
        return -1;
    }

    jclass hook_record_class = (*env)->FindClass(env, kHookRecordClassName);
    kHookRecordClassInfo.hook_trampoline_ = (*env)->GetFieldID(env,hook_record_class, "mHookTrampoline", "J");
    kHookRecordClassInfo.target_trampoline_ = (*env)->GetFieldID(env,hook_record_class, "mTargetTrampoline", "J");

    return JNI_VERSION_1_6;
}
