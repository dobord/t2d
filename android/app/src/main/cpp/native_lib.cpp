// SPDX-License-Identifier: Apache-2.0
#include <jni.h>

#include <string>

extern "C" JNIEXPORT jstring JNICALL Java_io_t2d_MainActivity_nativeHello(JNIEnv *env, jclass)
{
    static const char *msg = "hello-from-native";
    return env->NewStringUTF(msg);
}
