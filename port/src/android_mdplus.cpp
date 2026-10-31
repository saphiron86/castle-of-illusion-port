// Android MD+ pack import.
//
// A pack is a folder of some 30 WAV tracks (about 400 MB), a .cue sheet and a patched ROM. SDL's
// file dialog can't choose a folder on Android, so the activity (CoiActivity.java) asks for it
// with Android's folder picker and copies the pack into the app's private storage on a thread
// of its own; the port reads its progress here every frame. The pack is copied rather than read
// where it is so the tracks can be opened as plain files, as on a desktop.

#include "android_mdplus.h"

#include <SDL3/SDL.h>
#include <jni.h>

#include <filesystem>

namespace {

// Calls a static method of the activity's class (CoiActivity).
template <typename F>
auto with_activity(F call) -> decltype(call((JNIEnv*)nullptr, (jclass)nullptr))
{
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity)
        return {};
    jclass cls = env->GetObjectClass(activity);
    auto result = call(env, cls);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return result;
}

}  // namespace

void android_import_pack()
{
    const char* internal = SDL_GetAndroidInternalStoragePath();
    if (!internal)
        return;
    const std::string dest = (std::filesystem::path(internal) / "mdplus").string();
    with_activity([&](JNIEnv* env, jclass cls) {
        jmethodID start = env->GetStaticMethodID(cls, "startPackImport", "(Ljava/lang/String;)V");
        jstring path = env->NewStringUTF(dest.c_str());
        env->CallStaticVoidMethod(cls, start, path);
        env->DeleteLocalRef(path);
        return 0;
    });
}

PackImportStatus android_import_status()
{
    return with_activity([](JNIEnv* env, jclass cls) {
        PackImportStatus status;
        status.state = (PackImport)env->CallStaticIntMethod(cls, env->GetStaticMethodID(cls, "packImportState", "()I"));
        if (status.state == PackImport::Copying) {
            const jlong done = env->CallStaticLongMethod(cls, env->GetStaticMethodID(cls, "packImportDone", "()J"));
            const jlong total = env->CallStaticLongMethod(cls, env->GetStaticMethodID(cls, "packImportTotal", "()J"));
            status.progress = total > 0 ? (double)done / (double)total : 0.0;
        } else if (status.state == PackImport::Failed) {
            auto message = (jstring)env->CallStaticObjectMethod(cls, env->GetStaticMethodID(cls, "packImportMessage", "()Ljava/lang/String;"));
            if (message) {
                const char* text = env->GetStringUTFChars(message, nullptr);
                status.message = text;
                env->ReleaseStringUTFChars(message, text);
                env->DeleteLocalRef(message);
            }
        }
        return status;
    });
}

void android_import_clear()
{
    with_activity([](JNIEnv* env, jclass cls) {
        env->CallStaticVoidMethod(cls, env->GetStaticMethodID(cls, "packImportClear", "()V"));
        return 0;
    });
}
