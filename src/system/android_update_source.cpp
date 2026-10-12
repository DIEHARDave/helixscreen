// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/android_update_source.h"

#ifdef __ANDROID__
#include "system/android_jni.h"

#include <spdlog/spdlog.h>

#include <SDL.h>
#include <jni.h>
#endif

namespace helix::android {

std::string update_url(const std::string& installer_package, const OfferedRelease& offered) {
    if (installer_package == kPlayStoreInstaller) {
        return kPlayStoreMarketUrl;
    }
    return release_page_url(offered);
}

#ifdef __ANDROID__
std::string installer_package() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) {
        return {};
    }

    // Cached global ref owned by helix_activity_class(), never released here.
    jclass cls = helix_activity_class(env);
    if (!cls) {
        return {};
    }

    jmethodID method = env->GetStaticMethodID(cls, "getInstallerPackage", "()Ljava/lang/String;");
    if (!method) {
        spdlog::error("[android_update_source] Failed to find getInstallerPackage method");
        env->ExceptionClear();
        return {};
    }

    auto j_result = static_cast<jstring>(env->CallStaticObjectMethod(cls, method));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        if (j_result) {
            env->DeleteLocalRef(j_result);
        }
        return {};
    }
    if (!j_result) {
        return {};
    }

    std::string result;
    if (const char* chars = env->GetStringUTFChars(j_result, nullptr)) {
        result.assign(chars);
        env->ReleaseStringUTFChars(j_result, chars);
    } else {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(j_result);
    return result;
}
#endif

} // namespace helix::android
